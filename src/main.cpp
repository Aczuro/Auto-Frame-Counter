#include "MacroTypes.hpp"
#include "MacroParserManager.hpp"
#include "AdvancedFrameCounterPopup.hpp"
#include "AnticHUDLayer.hpp"
#include "StateSnapshot.hpp"
#include "TrajectorySim.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <deque>
#include <algorithm>

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
// 1. PLAYLAYER HOOK: Quản lý Lifecycle, Practice Checkpoint, AnticHUDLayer & Trajectory
// ============================================================================
class $modify(AFCPlayLayer, PlayLayer) {
    struct Fields {
        int currentFrame = 0;

        bool holdingP1 = false;
        bool holdingP2 = false;
        bool ignoreInjectedInput = false;

        std::vector<MacroAction> macroActions;
        MacroExtension activeMacroExtension = MacroExtension::INTERNAL;
        size_t playbackCursor = 0;
        bool macroLoadedAndAnalyzed = false;
        int windowTally[MAX_BUCKET] = {0};

        std::deque<FullPlayLayerSnapshot> historySnapshots;
        std::vector<std::pair<Ref<CheckpointObject>, int>> checkpointFrames;

        CCDrawNode* drawNode = nullptr;
        AnticHUDLayer* anticHud = nullptr;
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

        // Ưu tiên nạp dữ liệu từ MacroParserManager nếu người dùng đã Import tệp Macro bên ngoài
        auto& parser = MacroParserManager::get();
        if (!parser.getEvents().empty()) {
            f->macroActions = parser.exportAsMacroActions();
            f->activeMacroExtension = parser.getActiveExtension();
            recomputeTally(f->macroActions, f->windowTally);
        } else {
            // Nếu chưa Import tệp ngoài, nạp tệp JSON nội bộ theo ID màn chơi
            loadMacroData(level, f->macroLoadedAndAnalyzed, f->macroActions, f->windowTally);
            f->activeMacroExtension = MacroExtension::INTERNAL;
        }

        // Khởi tạo lớp vẽ quỹ đạo (Trajectory & White Trail) gắn vào m_objectLayer
        f->drawNode = CCDrawNode::create();
        if (f->drawNode && m_objectLayer) {
            m_objectLayer->addChild(f->drawNode, 999);
        }

        // Khởi tạo giao diện AnticHUDLayer chuyên biệt gắn vào PlayLayer
        f->anticHud = AnticHUDLayer::create();
        if (f->anticHud) {
            this->addChild(f->anticHud, 1000);
        }

        refreshAnticHud();
        return true;
    }

    // Đồng bộ lại danh sách Macro sau khi người dùng bấm "Import Macro File" trong Popup
    void reloadFromUniversalParser() {
        auto* f = m_fields.self();
        auto& parser = MacroParserManager::get();
        if (parser.getEvents().empty()) return;

        f->macroActions = parser.exportAsMacroActions();
        f->activeMacroExtension = parser.getActiveExtension();
        f->lastAnalyzedActionIdx = -1;
        f->playbackCursor = 0;
        while (f->playbackCursor < f->macroActions.size() &&
               f->macroActions[f->playbackCursor].frame < f->currentFrame) {
            ++f->playbackCursor;
        }
        recomputeTally(f->macroActions, f->windowTally);
        refreshAnticHud();
    }

    void resetLevel() {
        if (TrajectorySim::isSimulating()) {
            PlayLayer::resetLevel();
            return;
        }

        auto* f = m_fields.self();
        const auto mode = getCurrentOperatingMode();

        PlayLayer::resetLevel();

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
            restoredFrame = 0;
            f->checkpointFrames.clear();
        }

        f->currentFrame = restoredFrame;
        f->holdingP1 = false;
        f->holdingP2 = false;
        f->historySnapshots.clear();
        f->whiteTrail.clear();
        f->cachedTrajectory.clear();

        if (mode == OperatingMode::RecordPractice && f->activeMacroExtension == MacroExtension::INTERNAL) {
            std::erase_if(f->macroActions, [restoredFrame](const MacroAction& act) {
                return act.frame >= restoredFrame;
            });
            for (size_t i = 0; i < f->macroActions.size(); ++i) {
                f->macroActions[i].index = static_cast<int>(i);
            }
            saveMacroData(m_level, false, f->macroActions);
        }

        f->playbackCursor = 0;
        while (f->playbackCursor < f->macroActions.size() &&
               f->macroActions[f->playbackCursor].frame < restoredFrame) {
            ++f->playbackCursor;
        }

        refreshAnticHud();
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

    void refreshAnticHud() {
        auto* f = m_fields.self();
        if (!f->anticHud) return;

        recomputeTally(f->macroActions, f->windowTally);

        int feasibleFrames = 0;
        int bwdFrames = 0;
        int fwdFrames = 0;

        if (f->lastAnalyzedActionIdx >= 0 &&
            static_cast<size_t>(f->lastAnalyzedActionIdx) < f->macroActions.size()) {
            const auto& act = f->macroActions[f->lastAnalyzedActionIdx];
            feasibleFrames = act.windowSize;
            bwdFrames      = act.backwardFrames;
            fwdFrames      = act.forwardFrames;
        }

        f->anticHud->updateHUD(
            this,
            f->activeMacroExtension,
            f->currentFrame,
            feasibleFrames,
            bwdFrames,
            fwdFrames,
            static_cast<int>(f->macroActions.size()),
            f->windowTally
        );
    }

    void renderVisualOverlays() {
        auto* f = m_fields.self();
        if (!f->drawNode) return;
        f->drawNode->clear();

        if (!isModEnabled()) return;

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
// 2. PAUSELAYER HOOK: Thêm nút mở AdvancedFrameCounterPopup ngay trong Pause Menu
// ============================================================================
class $modify(AFCPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto* btnSpr = ButtonSprite::create("AFC Menu", 64, true, "bigFont.fnt", "GJ_button_04.png", 26.f, 0.45f);
        auto* btn = CCMenuItemSpriteExtra::create(
            btnSpr, this, menu_selector(AFCPauseLayer::onOpenAFCPopup)
        );

        auto* menu = CCMenu::create();
        const auto winSize = CCDirector::sharedDirector()->getWinSize();
        menu->setPosition({54.f, winSize.height - 32.f});
        menu->addChild(btn);
        this->addChild(menu, 100);
    }

    void onOpenAFCPopup(CCObject*) {
        auto* popup = AdvancedFrameCounterPopup::create();
        if (!popup) return;
        popup->setOnMacroImportedCallback([]() {
            if (auto* pl = PlayLayer::get()) {
                static_cast<AFCPlayLayer*>(pl)->reloadFromUniversalParser();
            }
        });
        popup->show();
    }
};

// ============================================================================
// 3. GJBASEGAMELAYER HOOK: Đồng bộ Fixed Timestep Physics & Giả lập Input
// ============================================================================
class $modify(AFCBaseGameLayer, GJBaseGameLayer) {
    void processCommands(float dt) {
        auto* pl = typeinfo_cast<PlayLayer*>(this);
        if (!pl || !isModEnabled()) {
            GJBaseGameLayer::processCommands(dt);
            return;
        }

        if (TrajectorySim::isSimulating()) {
            GJBaseGameLayer::processCommands(dt);
            return;
        }

        auto* afcPl = static_cast<AFCPlayLayer*>(pl);
        auto* f = afcPl->m_fields.self();

        if (pl->m_player1 && !pl->m_player1->m_isDead) {
            ++f->currentFrame;
            const int tickFrame = f->currentFrame;
            const auto mode = getCurrentOperatingMode();

            // A. Lưu Snapshot vào Ring-buffer lịch sử để phục vụ Quét Lùi (Backward Scan)
            if (mode == OperatingMode::AutoAnalyzeReplay || AdvancedFrameCounterPopup::isAutoDetectInjectionEnabled()) {
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

                                if (f->anticHud) {
                                    f->anticHud->spawnPlayerPositionBadge(
                                        pl, act.windowSize, act.backwardFrames, act.forwardFrames
                                    );
                                }
                                saveMacroData(pl->m_level, true, f->macroActions);
                            }
                        }

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

            // D. Cập nhật đường dự đoán quỹ đạo
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

        GJBaseGameLayer::processCommands(dt);

        afcPl->refreshAnticHud();
        afcPl->renderVisualOverlays();
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

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

        const auto mode = getCurrentOperatingMode();

        // Ghi nhận Macro trong chế độ Record Practice hoặc khi bật Auto-detect External Macro Injection
        if (mode == OperatingMode::RecordPractice || AdvancedFrameCounterPopup::isAutoDetectInjectionEnabled()) {
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

                // Nếu bật Auto-detect External Macro Injection, tự động phân tích ngay lập tức
                // input vừa được bơm vào từ bot bên ngoài (Echo, xdBot, Silicate, MegaHack)
                if (AdvancedFrameCounterPopup::isAutoDetectInjectionEnabled() &&
                    mode != OperatingMode::RecordPractice &&
                    !f->historySnapshots.empty()) {
                    const size_t newIdx = f->macroActions.size() - 1;
                    const FullPlayLayerSnapshot& baseSnap = f->historySnapshots.front();
                    FrameWindowResult winRes = TrajectorySim::analyzeActionWindow(
                        pl, baseSnap, f->macroActions, newIdx,
                        MAX_BACKWARD_FRAMES, MAX_FORWARD_FRAMES, SURVIVE_CHECK_FRAMES
                    );
                    if (winRes.valid) {
                        f->macroActions[newIdx].backwardFrames = winRes.backwardFrames;
                        f->macroActions[newIdx].forwardFrames  = winRes.forwardFrames;
                        f->macroActions[newIdx].windowSize     = winRes.windowSize;
                        f->macroActions[newIdx].analyzed       = true;
                        f->lastAnalyzedActionIdx               = static_cast<int>(newIdx);
                        if (f->anticHud) {
                            f->anticHud->spawnPlayerPositionBadge(
                                pl, winRes.windowSize, winRes.backwardFrames, winRes.forwardFrames
                            );
                        }
                    }
                }

                saveMacroData(pl->m_level, false, f->macroActions);
                afcPl->refreshAnticHud();
            }
        }
    }
};
