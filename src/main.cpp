#include "MacroTypes.hpp"
#include "StateSnapshot.hpp"
#include "TrajectorySim.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <deque>
#include <algorithm>
#include <sstream>

using namespace geode::prelude;

namespace {
    enum class OperatingMode {
        RecordPractice,
        AutoAnalyzeReplay,
        ReplayOnly
    };

    OperatingMode getCurrentOperatingMode() {
        auto modeStr = Mod::get()->getSettingValue<std::string>("work-mode");
        if (modeStr == "Record Macro (Practice)") {
            return OperatingMode::RecordPractice;
        }
        if (modeStr == "Replay Saved Data Only") {
            return OperatingMode::ReplayOnly;
        }
        return OperatingMode::AutoAnalyzeReplay;
    }

    bool isModEnabled() {
        return Mod::get()->getSettingValue<bool>("enable-mod");
    }
} // namespace

// ============================================================================
// 1. PLAYLAYER HOOK: Quản lý Lifecycle, Practice Checkpoint, HUD & Trajectory
// ============================================================================
class $modify(AFCPlayLayer, PlayLayer) {
    struct Fields {
        // Bộ đếm Frame vật lý cố định chuẩn yBot / Echo
        int currentFrame = 0;

        // Trạng thái giữ phím thực tế tại mỗi tick vật lý
        bool holdingP1 = false;
        bool holdingP2 = false;
        bool ignoreInjectedInput = false;

        // Dữ liệu Macro và con trỏ phát lại theo Fixed Timestep
        std::vector<MacroAction> macroActions;
        size_t playbackCursor = 0;
        size_t analyzeCursor = 0;
        bool macroLoadedAndAnalyzed = false;
        int windowTally[MAX_BUCKET] = {0};

        // Ring-buffer lưu trữ các Snapshot gần nhất (tối đa MAX_BACKWARD_FRAMES + 4)
        // Giúp dịch chuyển lùi về quá khứ tức thì khi cần phân tích Frame Window
        std::deque<FullPlayLayerSnapshot> historySnapshots;

        // Lưu lịch sử frame của từng Practice Checkpoint để cắt (truncate) Macro khi Respawn
        std::vector<std::pair<Ref<CheckpointObject>, int>> checkpointFrames;

        // Thành phần giao diện (HUD & Đường vẽ quỹ đạo)
        CCDrawNode* drawNode = nullptr;
        CCLayerColor* hudBg = nullptr;
        CCLabelBMFont* hudLabel = nullptr;
        std::deque<CCPoint> whiteTrail;
        std::vector<TrajectoryPoint> cachedTrajectory;
        int lastAnalyzedActionIdx = -1;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        auto* f = m_fields.self();
        f->currentFrame = 0;
        f->playbackCursor = 0;
        f->analyzeCursor = 0;

        // Nạp dữ liệu Macro đã lưu của Level (nếu có)
        loadMacroData(level, f->macroLoadedAndAnalyzed, f->macroActions, f->windowTally);

        // Khởi tạo lớp vẽ quỹ đạo (Trajectory & White Trail) gắn vào m_objectLayer
        f->drawNode = CCDrawNode::create();
        if (f->drawNode && m_objectLayer) {
            m_objectLayer->addChild(f->drawNode, 999);
        }

        // Khởi tạo HUD hiển thị thông số Frame & Frame Window
        f->hudLabel = CCLabelBMFont::create("", "bigFont.fnt");
        if (f->hudLabel) {
            f->hudLabel->setAnchorPoint(ccp(0.f, 1.f));
            this->addChild(f->hudLabel, 1000);
        }

        updateHudLayoutAndText();
        return true;
    }

    // Đồng bộ lại currentFrame và cắt bỏ các input thừa khi chết / chơi lại từ đầu
    void resetLevel() {
        // Nếu đang trong vòng lặp Headless Simulation, tuyệt đối không chạy logic reset UI
        if (TrajectorySim::isSimulating()) {
            PlayLayer::resetLevel();
            return;
        }

        auto* f = m_fields.self();
        const auto mode = getCurrentOperatingMode();

        PlayLayer::resetLevel();

        // Xác định frame xuất phát sau khi reset (0 hoặc frame tại Practice Checkpoint)
        int restoredFrame = 0;
        if (m_isPracticeMode && m_checkpointArray && m_checkpointArray->count() > 0) {
            auto* lastCkpt = static_cast<CheckpointObject*>(m_checkpointArray->lastObject());
            for (auto it = f->checkpointFrames.rbegin(); it != f->checkpointFrames.rend(); ++it) {
                if (it->first == lastCkpt) {
                    restoredFrame = it->second;
                    break;
                }
            }
        } else {
            // Chơi từ 0%: đồng bộ về frame 0
            restoredFrame = 0;
            f->checkpointFrames.clear();
        }

        f->currentFrame = restoredFrame;
        f->holdingP1 = false;
        f->holdingP2 = false;
        f->historySnapshots.clear();
        f->whiteTrail.clear();
        f->cachedTrajectory.clear();

        if (mode == OperatingMode::RecordPractice) {
            // Cắt bỏ mọi hành động được ghi sau thời điểm Checkpoint để tránh chồng chéo dữ liệu
            std::erase_if(f->macroActions, [restoredFrame](const MacroAction& act) {
                return act.frame >= restoredFrame;
            });
            for (size_t i = 0; i < f->macroActions.size(); ++i) {
                f->macroActions[i].index = static_cast<int>(i);
            }
            saveMacroData(m_level, false, f->macroActions);
        }

        // Đặt lại con trỏ phát lại (playbackCursor) tới đúng hành động đầu tiên >= restoredFrame
        f->playbackCursor = 0;
        while (f->playbackCursor < f->macroActions.size() &&
               f->macroActions[f->playbackCursor].frame < restoredFrame) {
            ++f->playbackCursor;
        }

        f->analyzeCursor = f->playbackCursor;
        updateHudLayoutAndText();
    }

    CheckpointObject* markCheckpoint() {
        auto* ckpt = PlayLayer::markCheckpoint();
        if (ckpt && !TrajectorySim::isSimulating()) {
            m_fields->checkpointFrames.emplace_back(ckpt, m_fields->currentFrame);
        }
        return ckpt;
    }

    void removeCheckpoint(bool first) {
        PlayLayer::removeCheckpoint(first);
        if (!TrajectorySim::isSimulating() && !m_fields->checkpointFrames.empty()) {
            m_fields->checkpointFrames.pop_back();
        }
    }

    // Chặn hiệu ứng nổ, âm thanh và rung màn hình khi nhân vật chết trong quá trình mô phỏng ngầm
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (TrajectorySim::isSimulating()) {
            if (object && object == m_anticheatSpike) {
                return;
            }
            if (player) {
                player->m_isDead = true;
            }
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void updateHudLayoutAndText() {
        auto* f = m_fields.self();
        if (!f->hudLabel) return;

        if (!isModEnabled()) {
            f->hudLabel->setVisible(false);
            if (f->drawNode) f->drawNode->setVisible(false);
            return;
        }

        f->hudLabel->setVisible(true);
        if (f->drawNode) f->drawNode->setVisible(true);

        const float scale = static_cast<float>(Mod::get()->getSettingValue<double>("font-size"));
        const ccColor3B color = Mod::get()->getSettingValue<ccColor3B>("text-color");
        const std::string posMode = Mod::get()->getSettingValue<std::string>("hud-position");
        const std::string dispMode = Mod::get()->getSettingValue<std::string>("display-mode");

        f->hudLabel->setScale(scale);
        f->hudLabel->setColor(color);

        const auto winSize = CCDirector::sharedDirector()->getWinSize();
        constexpr float margin = 12.f;

        if (posMode == "Top-Left") {
            f->hudLabel->setAnchorPoint(ccp(0.f, 1.f));
            f->hudLabel->setPosition(ccp(margin, winSize.height - margin));
        } else if (posMode == "Bottom-Left") {
            f->hudLabel->setAnchorPoint(ccp(0.f, 0.f));
            f->hudLabel->setPosition(ccp(margin, margin));
        } else if (posMode == "Bottom-Right") {
            f->hudLabel->setAnchorPoint(ccp(1.f, 0.f));
            f->hudLabel->setPosition(ccp(winSize.width - margin, margin));
        } else {
            // Mặc định: Top-Right
            f->hudLabel->setAnchorPoint(ccp(1.f, 1.f));
            f->hudLabel->setPosition(ccp(winSize.width - margin, winSize.height - margin));
        }

        recomputeTally(f->macroActions, f->windowTally);

        int totalAnalyzed = 0;
        for (int i = 1; i < MAX_BUCKET; ++i) {
            totalAnalyzed += f->windowTally[i];
        }

        std::ostringstream oss;
        oss << "Physics Frame: " << f->currentFrame << "\n";

        if (f->lastAnalyzedActionIdx >= 0 &&
            static_cast<size_t>(f->lastAnalyzedActionIdx) < f->macroActions.size()) {
            const auto& lastAct = f->macroActions[f->lastAnalyzedActionIdx];
            if (dispMode == "Detailed (Diff / Total)") {
                oss << fmt::format(
                    "Last Action #{}: [-{}f / +{}f] -> {}F Window\n",
                    lastAct.index + 1,
                    lastAct.backwardFrames,
                    lastAct.forwardFrames,
                    lastAct.windowSize
                );
            } else if (dispMode == "Only Delta (Diff)") {
                oss << fmt::format(
                    "Delta: -{}f / +{}f ({}F)\n",
                    lastAct.backwardFrames,
                    lastAct.forwardFrames,
                    lastAct.windowSize
                );
            }
        }

        oss << "Total Clicks: " << f->macroActions.size() << " (Analyzed: " << totalAnalyzed << ")\n";
        oss << "1F: " << f->windowTally[1]
            << " | 2F: " << f->windowTally[2]
            << " | 3F: " << f->windowTally[3]
            << " | 4F+: " << (totalAnalyzed - f->windowTally[1] - f->windowTally[2] - f->windowTally[3]);

        f->hudLabel->setString(oss.str().c_str());
    }

    void renderVisualOverlays() {
        auto* f = m_fields.self();
        if (!f->drawNode) return;
        f->drawNode->clear();

        if (!isModEnabled()) return;

        // 1. Vẽ đường bay thực tế của nhân vật (White Player Trail)
        if (Mod::get()->getSettingValue<bool>("render-trail") && f->whiteTrail.size() >= 2) {
            for (size_t i = 1; i < f->whiteTrail.size(); ++i) {
                f->drawNode->drawSegment(
                    f->whiteTrail[i - 1],
                    f->whiteTrail[i],
                    0.85f,
                    ccc4f(1.f, 1.f, 1.f, 0.75f)
                );
            }
        }

        // 2. Vẽ đường dự đoán quỹ đạo (Trajectory Preview: Xanh = Sống, Đỏ = Chết)
        if (Mod::get()->getSettingValue<bool>("render-tray") && f->cachedTrajectory.size() >= 2) {
            const bool endsInDeath = (f->cachedTrajectory.back().type == TrajectoryPointType::Death);
            const ccColor4F lineColor = endsInDeath
                ? ccc4f(1.0f, 0.22f, 0.22f, 0.85f)
                : ccc4f(0.18f, 0.95f, 0.45f, 0.85f);

            for (size_t i = 1; i < f->cachedTrajectory.size(); ++i) {
                f->drawNode->drawSegment(
                    f->cachedTrajectory[i - 1].pos,
                    f->cachedTrajectory[i].pos,
                    1.1f,
                    lineColor
                );
            }
        }
    }
};

// ============================================================================
// 2. GJBASEGAMELAYER HOOK: Đồng bộ Fixed Timestep Physics & Giả lập Input
//    Tham chiếu trực tiếp kiến trúc yBot / Echo / Amethyst / Geode SDK
// ============================================================================
class $modify(AFCBaseGameLayer, GJBaseGameLayer) {
    // Hook hàm xử lý bước nhảy vật lý cố định (Fixed Timestep = 1/240s)
    // Trong GD 2.2, GJBaseGameLayer::update(dt) chia dt thành các bước cố định
    // và gọi processCommands() ở đầu mỗi bước vật lý.
    void processCommands(float dt) {
        auto* pl = typeinfo_cast<PlayLayer*>(this);
        if (!pl || !isModEnabled()) {
            GJBaseGameLayer::processCommands(dt);
            return;
        }

        // Nếu đang ở trong vòng lặp Headless Simulation của TrajectorySim:
        // Chỉ chạy bộ xử lý vật lý gốc, KHÔNG tăng currentFrame thực tế và KHÔNG gọi đệ quy Analyze!
        if (TrajectorySim::isSimulating()) {
            GJBaseGameLayer::processCommands(dt);
            return;
        }

        auto* afcPl = static_cast<AFCPlayLayer*>(pl);
        auto* f = afcPl->m_fields.self();

        // Chỉ tăng bộ đếm frame vật lý khi nhân vật còn sống và màn chơi đang chạy
        if (pl->m_player1 && !pl->m_player1->m_isDead) {
            // Tăng số frame vật lý cố định trước khi xử lý lệnh của tick hiện tại
            ++f->currentFrame;
            const int tickFrame = f->currentFrame;
            const auto mode = getCurrentOperatingMode();

            // A. Lưu Snapshot vào Ring-buffer lịch sử để phục vụ Quét Lùi (Backward Scan)
            if (mode == OperatingMode::AutoAnalyzeReplay) {
                FullPlayLayerSnapshot snap;
                if (snap.capture(pl, tickFrame, f->playbackCursor, f->holdingP1, f->holdingP2)) {
                    f->historySnapshots.push_back(std::move(snap));
                    while (f->historySnapshots.size() > static_cast<size_t>(MAX_BACKWARD_FRAMES + 4)) {
                        f->historySnapshots.pop_front();
                    }
                }
            }

            // B. PHÁT LẠI ĐẦU VÀO (MACRO REPLAY) TẠI ĐÚNG DELTA-FRAME VẬT LÝ CỐ ĐỊNH
            if (mode == OperatingMode::AutoAnalyzeReplay || mode == OperatingMode::ReplayOnly) {
                while (f->playbackCursor < f->macroActions.size() &&
                       f->macroActions[f->playbackCursor].frame <= tickFrame) {
                    auto& act = f->macroActions[f->playbackCursor];

                    if (act.frame == tickFrame) {
                        // Nếu đang ở chế độ Auto (Analyze + Replay) và hành động này chưa được phân tích:
                        // Thực hiện quét lùi & quét tiến từ Snapshot gốc cách đây MAX_BACKWARD_FRAMES
                        if (mode == OperatingMode::AutoAnalyzeReplay && !act.analyzed && !f->historySnapshots.empty()) {
                            const FullPlayLayerSnapshot& baseSnap = f->historySnapshots.front();
                            FrameWindowResult winRes = TrajectorySim::analyzeActionWindow(
                                pl,
                                baseSnap,
                                f->macroActions,
                                f->playbackCursor,
                                MAX_BACKWARD_FRAMES,
                                MAX_FORWARD_FRAMES,
                                SURVIVE_CHECK_FRAMES
                            );

                            if (winRes.valid) {
                                act.backwardFrames = winRes.backwardFrames;
                                act.forwardFrames  = winRes.forwardFrames;
                                act.windowSize     = winRes.windowSize;
                                act.analyzed       = true;
                                f->lastAnalyzedActionIdx = static_cast<int>(f->playbackCursor);

                                // Lưu kết quả phân tích xuống JSON ngay khi cập nhật xong
                                saveMacroData(pl->m_level, true, f->macroActions);
                            }
                        }

                        // Giả lập nhấn/nhả nút chính xác 100% tại tick vật lý cố định
                        f->ignoreInjectedInput = true;
                        const bool isPush = (act.type == ActionType::Press);
                        this->handleButton(isPush, act.button, act.player1);
                        f->ignoreInjectedInput = false;

                        if (act.player1) f->holdingP1 = isPush;
                        else             f->holdingP2 = isPush;
                    }
                    ++f->playbackCursor;
                }
            }

            // C. Lưu vết đường bay thực tế của Player 1 (White Player Trail)
            f->whiteTrail.push_back(pl->m_player1->getPosition());
            while (f->whiteTrail.size() > static_cast<size_t>(WHITE_TRAIL_MAX_PTS)) {
                f->whiteTrail.pop_front();
            }

            // D. Cập nhật đường dự đoán quỹ đạo (mỗi 4 tick vật lý để tối ưu FPS trên Main Thread)
            if (Mod::get()->getSettingValue<bool>("render-tray") && (tickFrame % 4 == 0)) {
                FullPlayLayerSnapshot previewSnap;
                if (previewSnap.capture(pl, tickFrame, f->playbackCursor, f->holdingP1, f->holdingP2)) {
                    f->cachedTrajectory = TrajectorySim::previewTrajectory(
                        pl,
                        previewSnap,
                        f->macroActions,
                        f->playbackCursor,
                        PREVIEW_STEPS
                    );
                }
            }
        }

        // Gọi hàm xử lý vật lý gốc của Engine
        GJBaseGameLayer::processCommands(dt);

        // Cập nhật HUD và vẽ overlay ở cuối tick
        afcPl->updateHudLayoutAndText();
        afcPl->renderVisualOverlays();
    }

    // Hook trực tiếp vào bộ nhận tín hiệu nút bấm vật lý của Engine (handleButton)
    // Đảm bảo ghi nhận chính xác tuyệt đối frame vật lý khi người chơi nhấn/nhả chuột
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        // Không ghi nhận input ảo phát sinh từ Headless Simulation hoặc từ chính bộ phát Replay
        if (TrajectorySim::isSimulating()) {
            return;
        }

        auto* pl = typeinfo_cast<PlayLayer*>(this);
        if (!pl || !isModEnabled()) {
            return;
        }

        auto* afcPl = static_cast<AFCPlayLayer*>(pl);
        auto* f = afcPl->m_fields.self();

        if (isPlayer1) f->holdingP1 = down;
        else           f->holdingP2 = down;

        if (f->ignoreInjectedInput) {
            return;
        }

        // Ghi Macro khi đang ở chế độ Record Macro
        if (getCurrentOperatingMode() == OperatingMode::RecordPractice) {
            if (pl->m_player1 && !pl->m_player1->m_isDead) {
                MacroAction act;
                act.index          = static_cast<int>(f->macroActions.size());
                act.frame          = f->currentFrame;
                act.type           = down ? ActionType::Press : ActionType::Release;
                act.button         = button;
                act.player1        = isPlayer1;
                act.x              = pl->m_player1->getPositionX();
                act.y              = pl->m_player1->getPositionY();
                act.analyzed       = false;
                act.backwardFrames = 0;
                act.forwardFrames  = 0;
                act.windowSize     = 1;

                f->macroActions.push_back(act);
                saveMacroData(pl->m_level, false, f->macroActions);
                afcPl->updateHudLayoutAndText();
            }
        }
    }
};
