#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CheckpointObject.hpp>
#include "MacroTypes.hpp"
#include "StateSnapshot.hpp"
#include "TrajectorySim.hpp"
#include <deque>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;

#ifdef GEODE_IS_WINDOWS
static int parseCustomKeyString(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.empty()) return 0;

    if (s.size() == 1) {
        char c = s[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<int>(c);
    }
    if (s == "SPACE") return VK_SPACE;
    if (s == "TAB") return VK_TAB;
    if (s == "SHIFT") return VK_SHIFT;
    if (s == "CTRL" || s == "CONTROL") return VK_CONTROL;
    if (s == "ALT") return VK_MENU;
    if (s == "BACKSPACE" || s == "BACK") return VK_BACK;
    if (s[0] == 'F' && s.size() >= 2 && s.size() <= 3) {
        int fn = std::atoi(s.c_str() + 1);
        if (fn >= 1 && fn <= 12) return VK_F1 + (fn - 1);
    }
    return 0;
}
#endif

// Gắn Snapshot trực tiếp vào CheckpointObject bằng Fields (Geode Docs Idiom)
class $modify(AFCCheckpointObject, CheckpointObject) {
    struct Fields {
        FullPlayLayerSnapshot snapshot;
    };
};

class $modify(AFCPlayLayer, PlayLayer) {
    struct Fields {
        bool enabled = true;
        bool showWhiteTrail = true;
        bool showAheadTraj = true;
        int targetFps = 240;

        BotStage stage = BotStage::Record;
        bool isSimulating = false;
        bool simDead = false;

        int currentFrame = 0;
        size_t playbackIndex = 0;
        bool holdingP1 = false;
        bool holdingP2 = false;
        bool analysisCompleted = false;

        std::vector<MacroAction> actions;

        // Trạng thái Quét cuốn chiếu (Rolling Scan)
        bool rollingScanInitialized = false;
        size_t rollingActionIdx = 0;
        int rollingMainFrame = 0;
        size_t rollingMainMacroIdx = 0;
        FullPlayLayerSnapshot rollingMainSnapshot;
        std::vector<TrajectoryBranch> activeBranches;

        int tally[MAX_BUCKET + 2] = {};
        std::deque<CCPoint> whiteTrail;
        std::string lastPopupText;
        int popupTimer = 0;

        // Dummy PlayerObject phục vụ ShowTrajectory chuẩn vật lý gốc
        PlayerObject* dummyPlayer = nullptr;

        CCNode* hudRoot = nullptr;
        CCDrawNode* worldDrawer = nullptr;
        CCLabelBMFont* tallyLabel = nullptr;
        CCLabelBMFont* statusLabel = nullptr;
        CCLabelBMFont* popupLabel = nullptr;
        CCLabelBMFont* playerPopupLabel = nullptr;
    };

    void syncSettings() {
        auto* f = m_fields.self();
        f->enabled        = Mod::get()->getSettingValue<bool>("enable-mod");
        f->showWhiteTrail = Mod::get()->getSettingValue<bool>("show-white-trail");
        f->showAheadTraj  = Mod::get()->getSettingValue<bool>("show-ahead-trajectory");
        f->targetFps      = std::clamp(static_cast<int>(Mod::get()->getSettingValue<int64_t>("target-fps")), 60, 1000);
    }

    void refreshHudText() {
        auto* f = m_fields.self();
        if (!f->hudRoot) return;
        f->hudRoot->setVisible(f->enabled);
        if (f->worldDrawer) f->worldDrawer->setVisible(f->enabled);
        if (!f->enabled) return;

        // Bảng thống kê 1F..10F+ bên trái (Đã xóa hoàn toàn chữ NanCounter)
        if (f->tallyLabel) {
            std::string s;
            for (int w = 1; w <= MAX_BUCKET; w++) {
                if (!s.empty()) s += "\n";
                s += fmt::format("{}F: {}", w, f->tally[w]);
            }
            s += fmt::format("\n{}+F: {}", MAX_BUCKET + 1, f->tally[MAX_BUCKET + 1]);
            f->tallyLabel->setString(s.c_str());
        }

        if (f->statusLabel) {
            std::string modeStr;
            if (f->stage == BotStage::Record) {
                modeStr = fmt::format(
                    "[RECORD] Frame: {} | Actions: {} (Type 1/2)",
                    f->currentFrame,
                    f->actions.size()
                );
            } else if (f->stage == BotStage::RollingScan) {
                int pct = f->actions.empty()
                    ? 0
                    : static_cast<int>((f->rollingActionIdx * 100) / f->actions.size());
                modeStr = fmt::format(
                    "[ROLLING SCAN -> JSON] {}/{} ({}%) | Frame: {}",
                    f->rollingActionIdx,
                    f->actions.size(),
                    pct,
                    f->rollingMainFrame
                );
            } else {
                modeStr = fmt::format(
                    "[REPLAY JSON] Frame: {} | Actions: {}",
                    f->currentFrame,
                    f->actions.size()
                );
            }
            f->statusLabel->setString(modeStr.c_str());
        }

        if (f->popupLabel) {
            if (f->popupTimer > 0 && !f->lastPopupText.empty()) {
                f->popupLabel->setString(f->lastPopupText.c_str());
                f->popupLabel->setVisible(true);
            } else {
                f->popupLabel->setVisible(false);
            }
        }
    }

    void renderWorldTrajectories() {
        auto* f = m_fields.self();
        if (!f->worldDrawer || !m_player1) return;
        f->worldDrawer->clear();
        if (!f->enabled) return;

        // 1. Vẽ White Trajectory Arc đi theo nhân vật
        if (f->showWhiteTrail && f->whiteTrail.size() >= 2) {
            ccColor4F whiteCol = {1.0f, 1.0f, 1.0f, 0.85f};
            for (size_t i = 0; i + 1 < f->whiteTrail.size(); i++) {
                f->worldDrawer->drawSegment(f->whiteTrail[i], f->whiteTrail[i + 1], 1.6f, whiteCol);
            }
        }

        // 2. Vẽ chùm nhánh Trajectory Backward/Forward của thao tác vừa kích hoạt
        for (const auto& branch : f->activeBranches) {
            if (branch.points.size() < 2) continue;
            ccColor4F col;
            float width = 1.2f;
            if (branch.delta == 0) {
                col = {1.0f, 0.92f, 0.15f, 0.95f};
                width = 1.9f;
            } else if (branch.survived) {
                col = {0.15f, 1.0f, 0.35f, 0.70f};
            } else {
                col = {1.0f, 0.20f, 0.20f, 0.45f};
            }

            for (size_t i = 0; i + 1 < branch.points.size(); i++) {
                f->worldDrawer->drawSegment(branch.points[i], branch.points[i + 1], width, col);
            }
            f->worldDrawer->drawDot(branch.points.back(), branch.survived ? 2.4f : 3.2f, col);
        }

        // 3. Vẽ Ahead Trajectory bằng Dummy PlayerObject chạy hàm vật lý thật của GD
        if (f->showAheadTraj && f->dummyPlayer && f->stage != BotStage::RollingScan) {
            TrajectorySimulator::drawAheadTrajectoryWithDummy(
                this,
                f->dummyPlayer,
                f->worldDrawer,
                f->targetFps,
                f->isSimulating,
                f->simDead
            );
        }
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto* f = m_fields.self();
        syncSettings();

        // Tạo Dummy PlayerObject ẩn phục vụ ShowTrajectory
        f->dummyPlayer = TrajectorySimulator::createDummyPlayer(this);

        bool hasJsonMacro = MacroStorage::loadFromJson(level, f->analysisCompleted, f->actions, f->tally);
        std::string modeSetting = Mod::get()->getSettingValue<std::string>("work-mode");

        if (modeSetting == "Record Macro (Practice)") {
            f->stage = BotStage::Record;
        } else if (modeSetting == "Replay Saved Data Only") {
            f->stage = BotStage::Replay;
        } else {
            if (hasJsonMacro && f->analysisCompleted) {
                f->stage = BotStage::Replay;
            } else if (hasJsonMacro && !f->analysisCompleted) {
                f->stage = BotStage::RollingScan;
            } else {
                f->stage = BotStage::Record;
            }
        }

        auto ws = CCDirector::get()->getWinSize();

        f->worldDrawer = CCDrawNode::create();
        if (m_objectLayer) {
            m_objectLayer->addChild(f->worldDrawer, 1500);
        }

        f->playerPopupLabel = CCLabelBMFont::create("", "bigFont.fnt");
        f->playerPopupLabel->setScale(0.42f);
        f->playerPopupLabel->setVisible(false);
        if (m_objectLayer) {
            m_objectLayer->addChild(f->playerPopupLabel, 1600);
        }

        f->hudRoot = CCNode::create();
        this->addChild(f->hudRoot, 1000);

        f->tallyLabel = CCLabelBMFont::create("", "bigFont.fnt");
        f->tallyLabel->setScale(0.28f);
        f->tallyLabel->setOpacity(215);
        f->tallyLabel->setAnchorPoint({0.f, 1.f});
        f->tallyLabel->setPosition({8.f, ws.height - 8.f});
        f->hudRoot->addChild(f->tallyLabel);

        f->popupLabel = CCLabelBMFont::create("", "bigFont.fnt");
        f->popupLabel->setScale(0.50f);
        f->popupLabel->setOpacity(235);
        f->popupLabel->setAnchorPoint({0.5f, 1.f});
        f->popupLabel->setPosition({ws.width * 0.5f, ws.height - 10.f});
        f->hudRoot->addChild(f->popupLabel);

        f->statusLabel = CCLabelBMFont::create("", "chatFont.fnt");
        f->statusLabel->setScale(0.55f);
        f->statusLabel->setOpacity(190);
        f->statusLabel->setAnchorPoint({0.5f, 0.f});
        f->statusLabel->setPosition({ws.width * 0.5f, 6.f});
        f->hudRoot->addChild(f->statusLabel);

        auto menu = CCMenu::create();
        menu->setPosition({ws.width - 88.f, 22.f});
        f->hudRoot->addChild(menu, 10);

        auto recSpr = ButtonSprite::create("REC", 32, true, "bigFont.fnt", "GJ_button_01.png", 22.f, 0.4f);
        auto btnRec = CCMenuItemSpriteExtra::create(recSpr, this, menu_selector(AFCPlayLayer::onSwitchRecord));
        btnRec->setPosition({-48.f, 0.f});

        auto anaSpr = ButtonSprite::create("ANA", 32, true, "bigFont.fnt", "GJ_button_02.png", 22.f, 0.4f);
        auto btnAna = CCMenuItemSpriteExtra::create(anaSpr, this, menu_selector(AFCPlayLayer::onSwitchAnalyze));
        btnAna->setPosition({0.f, 0.f});

        auto repSpr = ButtonSprite::create("REP", 32, true, "bigFont.fnt", "GJ_button_04.png", 22.f, 0.4f);
        auto btnRep = CCMenuItemSpriteExtra::create(repSpr, this, menu_selector(AFCPlayLayer::onSwitchReplay));
        btnRep->setPosition({48.f, 0.f});

        menu->addChild(btnRec);
        menu->addChild(btnAna);
        menu->addChild(btnRep);

        refreshHudText();
        return true;
    }

    void onSwitchRecord(CCObject*) {
        auto* f = m_fields.self();
        f->stage = BotStage::Record;
        f->actions.clear();
        f->activeBranches.clear();
        f->analysisCompleted = false;
        f->rollingScanInitialized = false;
        std::fill(std::begin(f->tally), std::end(f->tally), 0);
        this->resetLevel();
        refreshHudText();
    }

    void onSwitchAnalyze(CCObject*) {
        auto* f = m_fields.self();
        if (f->actions.empty()) {
            MacroStorage::loadFromJson(m_level, f->analysisCompleted, f->actions, f->tally);
        }
        if (f->actions.empty()) {
            FLAlertLayer::create(
                "Auto Frame Counter",
                "Chua co du lieu Macro! Hay bat Practice Mode va ghi Macro (REC) truoc.",
                "OK"
            )->show();
            return;
        }
        startRollingScan();
    }

    void onSwitchReplay(CCObject*) {
        auto* f = m_fields.self();
        if (f->actions.empty()) {
            MacroStorage::loadFromJson(m_level, f->analysisCompleted, f->actions, f->tally);
        }
        f->stage = BotStage::Replay;
        f->rollingScanInitialized = false;
        this->resetLevel();
        refreshHudText();
    }

    void startRollingScan() {
        auto* f = m_fields.self();
        f->stage = BotStage::RollingScan;
        f->rollingScanInitialized = false;
        f->rollingActionIdx = 0;
        f->rollingMainFrame = 0;
        f->rollingMainMacroIdx = 0;
        f->analysisCompleted = false;
        std::fill(std::begin(f->tally), std::end(f->tally), 0);
        for (auto& act : f->actions) act.analyzed = false;
        refreshHudText();
    }

    CheckpointObject* createCheckpoint() {
        auto ckpt = PlayLayer::createCheckpoint();
        auto* f = m_fields.self();
        if (ckpt && f->enabled && !f->isSimulating && f->stage == BotStage::Record) {
            auto* ckptMod = static_cast<AFCCheckpointObject*>(ckpt);
            ckptMod->m_fields->snapshot = FullPlayLayerSnapshot::capture(
                this,
                f->currentFrame,
                f->actions.size(),
                f->holdingP1,
                f->holdingP2,
                false
            );
        }
        return ckpt;
    }

    void resetLevel() {
        this->handleButton(false, 1, true);
        PlayLayer::resetLevel();

        auto* f = m_fields.self();
        if (!f->isSimulating) {
            f->whiteTrail.clear();
        }

        if (f->enabled && f->stage == BotStage::Record && !f->isSimulating) {
            if (m_isPracticeMode && m_checkpointArray && m_checkpointArray->count() > 0) {
                auto* lastCkpt = static_cast<AFCCheckpointObject*>(m_checkpointArray->lastObject());
                if (lastCkpt && lastCkpt->m_fields->snapshot.valid) {
                    const auto& snap = lastCkpt->m_fields->snapshot;
                    f->currentFrame = snap.frame;
                    if (f->actions.size() > snap.macroIndex) {
                        f->actions.resize(snap.macroIndex);
                    }
                    snap.restore(this);
                    f->holdingP1 = snap.holdingP1;
                    f->holdingP2 = snap.holdingP2;
                    refreshHudText();
                    return;
                }
            }
            f->currentFrame = 0;
            f->actions.clear();
            f->holdingP1 = false;
            f->holdingP2 = false;
            refreshHudText();
        } else if (!f->isSimulating) {
            f->currentFrame = 0;
            f->playbackIndex = 0;
            f->holdingP1 = false;
            f->holdingP2 = false;
        }
    }

    void levelComplete() {
        auto* f = m_fields.self();
        if (f->isSimulating) return;

        PlayLayer::levelComplete();
        if (!f->enabled) return;

        if (f->stage == BotStage::Record && !f->actions.empty()) {
            f->analysisCompleted = false;
            MacroStorage::saveToJson(m_level, f->targetFps, false, f->actions, f->tally);
            startRollingScan();
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (object == m_anticheatSpike) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }

        auto* f = m_fields.self();
        // Chặn hoàn toàn chết thật khi Dummy Player hoặc Rolling Scan va chạm
        if (f->isSimulating || (f->dummyPlayer && player == f->dummyPlayer)) {
            if (player) player->m_isDead = true;
            f->simDead = true;
            return;
        }

        PlayLayer::destroyPlayer(player, object);
    }

    void stepRollingScanForAction(size_t actionIdx) {
        auto* f = m_fields.self();
        if (actionIdx >= f->actions.size()) return;

        bool savedPractice = m_isPracticeMode;
        m_isPracticeMode = false;
        f->isSimulating = true;

        float dt = 1.0f / static_cast<float>(f->targetFps);

        if (!f->rollingScanInitialized) {
            this->resetLevel();
            f->rollingMainFrame = 0;
            f->rollingMainMacroIdx = 0;
            f->holdingP1 = false;
            f->holdingP2 = false;
            f->rollingScanInitialized = true;
        } else {
            f->rollingMainSnapshot.restore(this);
            f->rollingMainFrame = f->rollingMainSnapshot.frame;
            f->rollingMainMacroIdx = f->rollingMainSnapshot.macroIndex;
            f->holdingP1 = f->rollingMainSnapshot.holdingP1;
            f->holdingP2 = f->rollingMainSnapshot.holdingP2;
        }

        int actionFrame = f->actions[actionIdx].frame;
        int desiredSnapshotFrame = std::max(0, actionFrame - MAX_BACKWARD_FRAMES - 2);
        if (actionIdx > 0) {
            desiredSnapshotFrame = std::max(desiredSnapshotFrame, f->actions[actionIdx - 1].frame + 1);
        }
        desiredSnapshotFrame = std::min(desiredSnapshotFrame, actionFrame);

        while (f->rollingMainFrame < desiredSnapshotFrame) {
            while (f->rollingMainMacroIdx < actionIdx &&
                   f->actions[f->rollingMainMacroIdx].frame == f->rollingMainFrame) {
                const auto& mAct = f->actions[f->rollingMainMacroIdx];
                bool isDown = (mAct.type == ActionType::Press);
                if (mAct.player1) f->holdingP1 = isDown;
                else f->holdingP2 = isDown;
                this->handleButton(isDown, mAct.button, mAct.player1);
                f->rollingMainMacroIdx++;
            }

            GJBaseGameLayer::processCommands(dt, false, true);
            f->rollingMainFrame++;
        }

        while (f->rollingMainMacroIdx < actionIdx &&
               f->actions[f->rollingMainMacroIdx].frame <= f->rollingMainFrame) {
            const auto& mAct = f->actions[f->rollingMainMacroIdx];
            bool isDown = (mAct.type == ActionType::Press);
            if (mAct.player1) f->holdingP1 = isDown;
            else f->holdingP2 = isDown;
            this->handleButton(isDown, mAct.button, mAct.player1);
            f->rollingMainMacroIdx++;
        }

        f->rollingMainSnapshot = FullPlayLayerSnapshot::capture(
            this,
            f->rollingMainFrame,
            f->rollingMainMacroIdx,
            f->holdingP1,
            f->holdingP2,
            true
        );

        f->activeBranches.clear();

        // 1. Nhánh gốc (delta = 0)
        f->activeBranches.push_back(TrajectorySimulator::simulateShiftedBranch(
            this, f->rollingMainSnapshot, f->actions, actionIdx, 0, f->targetFps, f->simDead
        ));

        // 2. Nhánh lùi (Backward Frame Check)
        int backwardValid = 0;
        for (int b = 1; b <= MAX_BACKWARD_FRAMES; b++) {
            auto br = TrajectorySimulator::simulateShiftedBranch(
                this, f->rollingMainSnapshot, f->actions, actionIdx, -b, f->targetFps, f->simDead
            );
            bool ok = br.survived;
            f->activeBranches.push_back(std::move(br));
            if (ok) backwardValid++;
            else break;
        }

        // 3. Nhánh tiến (Forward Frame Check)
        int forwardValid = 0;
        for (int fw = 1; fw <= MAX_FORWARD_FRAMES; fw++) {
            auto br = TrajectorySimulator::simulateShiftedBranch(
                this, f->rollingMainSnapshot, f->actions, actionIdx, +fw, f->targetFps, f->simDead
            );
            bool ok = br.survived;
            f->activeBranches.push_back(std::move(br));
            if (ok) forwardValid++;
            else break;
        }

        f->rollingMainSnapshot.restore(this);

        int totalWindow = 1 + backwardValid + forwardValid;
        auto& act = f->actions[actionIdx];
        act.index          = static_cast<int>(actionIdx);
        act.analyzed       = true;
        act.backwardFrames = backwardValid;
        act.forwardFrames  = forwardValid;
        act.windowSize     = totalWindow;
        act.branches       = f->activeBranches;

        int bucket = (totalWindow > MAX_BUCKET) ? (MAX_BUCKET + 1) : totalWindow;
        f->tally[bucket]++;

        f->isSimulating = false;
        m_isPracticeMode = savedPractice;
    }

    void triggerActionFeedback(const MacroAction& act) {
        auto* f = m_fields.self();
        const char* shortType = (act.type == ActionType::Press) ? "Type 1: PRESS" : "Type 2: RELEASE";
        f->lastPopupText = fmt::format(
            "{}F [{} | 1 + {}B + {}A]",
            act.windowSize,
            shortType,
            act.backwardFrames,
            act.forwardFrames
        );
        f->popupTimer = 95;

        if (!act.branches.empty()) {
            f->activeBranches = act.branches;
        }

        if (f->playerPopupLabel && m_player1) {
            f->playerPopupLabel->setString(fmt::format("{}F", act.windowSize).c_str());
            f->playerPopupLabel->setPosition({
                m_player1->getPositionX(),
                m_player1->getPositionY() + 28.f
            });
            f->playerPopupLabel->setVisible(true);
        }

        if (Mod::get()->getSettingValue<bool>("enable-sound")) {
            FMODAudioEngine::sharedEngine()->playEffect("crystal01.ogg");
        }
    }

    void pollCustomShortcutKeys() {
#ifdef GEODE_IS_WINDOWS
        auto* f = m_fields.self();
        static bool prevToggle = false, prevMode = false, prevTraj = false, prevReset = false;

        int vkToggle = parseCustomKeyString(Mod::get()->getSettingValue<std::string>("toggle-key"));
        int vkMode   = parseCustomKeyString(Mod::get()->getSettingValue<std::string>("mode-key"));
        int vkTraj   = parseCustomKeyString(Mod::get()->getSettingValue<std::string>("trajectory-key"));
        int vkReset  = parseCustomKeyString(Mod::get()->getSettingValue<std::string>("reset-key"));

        bool curToggle = vkToggle ? ((GetAsyncKeyState(vkToggle) & 0x8000) != 0) : false;
        bool curMode   = vkMode   ? ((GetAsyncKeyState(vkMode)   & 0x8000) != 0) : false;
        bool curTraj   = vkTraj   ? ((GetAsyncKeyState(vkTraj)   & 0x8000) != 0) : false;
        bool curReset  = vkReset  ? ((GetAsyncKeyState(vkReset)  & 0x8000) != 0) : false;

        if (curToggle && !prevToggle) {
            f->enabled = !f->enabled;
            Mod::get()->setSettingValue<bool>("enable-mod", f->enabled);
            refreshHudText();
        }
        if (f->enabled) {
            if (curMode && !prevMode) {
                if (f->stage == BotStage::Record) {
                    MacroStorage::saveToJson(m_level, f->targetFps, false, f->actions, f->tally);
                    this->onSwitchAnalyze(nullptr);
                } else if (f->stage == BotStage::RollingScan) {
                    this->onSwitchReplay(nullptr);
                } else {
                    this->onSwitchRecord(nullptr);
                }
            }
            if (curTraj && !prevTraj) {
                f->showAheadTraj = !f->showAheadTraj;
                Mod::get()->setSettingValue<bool>("show-ahead-trajectory", f->showAheadTraj);
            }
            if (curReset && !prevReset) {
                auto path = MacroStorage::getLevelJsonPath(m_level);
                if (std::filesystem::exists(path)) {
                    std::filesystem::remove(path);
                }
                f->actions.clear();
                f->activeBranches.clear();
                std::fill(std::begin(f->tally), std::end(f->tally), 0);
                refreshHudText();
            }
        }

        prevToggle = curToggle;
        prevMode   = curMode;
        prevTraj   = curTraj;
        prevReset  = curReset;
#endif
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto* f = m_fields.self();
        if (f->isSimulating) return;

        pollCustomShortcutKeys();
        if (!f->enabled) return;

        if (f->stage == BotStage::RollingScan) {
            if (f->rollingActionIdx < f->actions.size()) {
                for (int step = 0; step < ACTIONS_PER_UI_TICK && f->rollingActionIdx < f->actions.size(); step++) {
                    stepRollingScanForAction(f->rollingActionIdx);
                    f->rollingActionIdx++;
                }
                renderWorldTrajectories();
                refreshHudText();

                if (f->rollingActionIdx >= f->actions.size()) {
                    f->analysisCompleted = true;
                    f->rollingMainSnapshot = FullPlayLayerSnapshot{};
                    f->activeBranches.clear();
                    MacroStorage::saveToJson(m_level, f->targetFps, true, f->actions, f->tally);
                    f->stage = BotStage::Replay;
                    this->resetLevel();
                }
                return;
            } else {
                f->stage = BotStage::Replay;
                this->resetLevel();
            }
        }

        if (f->popupTimer > 0) {
            f->popupTimer--;
            if (f->popupTimer == 0 && f->playerPopupLabel) {
                f->playerPopupLabel->setVisible(false);
            }
        }

        renderWorldTrajectories();
        refreshHudText();
    }
};

class $modify(AFCGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (auto* pl = static_cast<AFCPlayLayer*>(PlayLayer::get())) {
            auto* f = pl->m_fields.self();
            if (f->enabled && !f->isSimulating && f->stage == BotStage::Record) {
                bool alreadyHolding = isPlayer1 ? f->holdingP1 : f->holdingP2;
                if (down != alreadyHolding) {
                    if (isPlayer1) f->holdingP1 = down;
                    else f->holdingP2 = down;

                    CCPoint pos = pl->m_player1 ? pl->m_player1->getPosition() : CCPoint{0.f, 0.f};
                    MacroAction act;
                    act.index          = static_cast<int>(f->actions.size());
                    act.frame          = f->currentFrame;
                    act.type           = down ? ActionType::Press : ActionType::Release;
                    act.button         = button;
                    act.player1        = isPlayer1;
                    act.x              = pos.x;
                    act.y              = pos.y;
                    act.analyzed       = false;
                    act.backwardFrames = 0;
                    act.forwardFrames  = 0;
                    act.windowSize     = 1;

                    f->actions.push_back(std::move(act));
                }
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (auto* pl = static_cast<AFCPlayLayer*>(PlayLayer::get())) {
            auto* f = pl->m_fields.self();
            if (f->enabled && !f->isSimulating && f->stage != BotStage::RollingScan) {
                if (f->stage == BotStage::Replay) {
                    while (f->playbackIndex < f->actions.size() &&
                           f->actions[f->playbackIndex].frame == f->currentFrame) {
                        const auto& act = f->actions[f->playbackIndex];
                        bool isDown = (act.type == ActionType::Press);
                        GJBaseGameLayer::handleButton(isDown, act.button, act.player1);
                        if (act.analyzed) {
                            pl->triggerActionFeedback(act);
                        }
                        f->playbackIndex++;
                    }
                }

                if (pl->m_player1 && !pl->m_player1->m_isDead) {
                    f->whiteTrail.push_back(pl->m_player1->getPosition());
                    if (f->whiteTrail.size() > WHITE_TRAIL_MAX_PTS) {
                        f->whiteTrail.pop_front();
                    }
                }

                f->currentFrame++;
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};
