#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CheckpointObject.hpp>
#include <Geode/loader/SettingV3.hpp>

using namespace geode::prelude;

constexpr int MAX_SCAN_DELTA   = 10;
constexpr int LOOKAHEAD_TICKS  = 90;
constexpr int MAX_BUCKET       = 10;

struct MacroAction {
    int tick;
    bool down;
    int button;
    bool player1;
};

struct TrajectoryLine {
    int delta;
    bool passed;
    std::vector<CCPoint> points;
};

struct ActionAnalysis {
    int actionIndex;
    int originalTick;
    bool isDown;
    int minDelta;
    int maxDelta;
    int windowSize;
    std::vector<TrajectoryLine> trajectories;
};

enum class ModMode {
    Idle,
    Recording,
    Replaying,
    Scanning
};

struct CheckpointState {
    int tick;
    size_t actionCount;
};

struct EngineState {
    bool modEnabled = true; // Trạng thái Bật/Tắt mod
    ModMode mode = ModMode::Idle;
    bool isSimulating = false;
    bool simDead = false;

    int currentTick = 0;
    size_t playbackIdx = 0;

    std::vector<MacroAction> actions;
    std::unordered_map<CheckpointObject*, CheckpointState> ckptMap;

    int scanActionIdx = 0;
    int scanDelta = -MAX_SCAN_DELTA;
    ActionAnalysis currentAnalysis;
    std::vector<ActionAnalysis> allAnalyses;
    int tally[MAX_BUCKET + 2] = {};

    bool showTrajectory = true;
    int viewedActionIdx = -1;
    CCDrawNode* trajDrawer = nullptr;
    CCNode* hudContainer = nullptr; // Chứa toàn bộ HUD để ẩn/hiện khi bật/tắt mod

    CCLabelBMFont* bigLabel = nullptr;
    CCLabelBMFont* infoLabel = nullptr;
    CCLabelBMFont* tallyLabel = nullptr;
    CCLabelBMFont* frameLabel = nullptr;
};

inline EngineState g_state;

static CCLabelBMFont* makeHudLabel(CCNode* parent, float scale, CCPoint anchor, CCPoint pos) {
    auto label = CCLabelBMFont::create("", "bigFont.fnt");
    label->setScale(scale);
    label->setOpacity(210);
    label->setAnchorPoint(anchor);
    label->setPosition(pos);
    parent->addChild(label, 1000);
    return label;
}

static void drawCurrentTrajectories() {
    if (!g_state.trajDrawer) return;
    g_state.trajDrawer->clear();
    if (!g_state.modEnabled || !g_state.showTrajectory || g_state.allAnalyses.empty()) return;

    int idx = g_state.viewedActionIdx;
    if (idx < 0 || idx >= static_cast<int>(g_state.allAnalyses.size())) {
        idx = static_cast<int>(g_state.allAnalyses.size()) - 1;
    }

    const auto& analysis = g_state.allAnalyses[idx];

    for (const auto& line : analysis.trajectories) {
        if (line.points.size() < 2) continue;

        ccColor4F col;
        float thickness = 1.2f;
        if (line.delta == 0) {
            col = {1.0f, 0.9f, 0.1f, 0.95f}; // Vàng: Quỹ đạo gốc
            thickness = 2.0f;
        } else if (line.passed) {
            col = {0.15f, 1.0f, 0.25f, 0.65f}; // Xanh lá: Sống
        } else {
            col = {1.0f, 0.15f, 0.15f, 0.45f}; // Đỏ: Chết
        }

        for (size_t i = 0; i + 1 < line.points.size(); i++) {
            g_state.trajDrawer->drawSegment(line.points[i], line.points[i + 1], thickness, col);
        }
        g_state.trajDrawer->drawDot(line.points.back(), line.passed ? 2.5f : 3.5f, col);
    }
}

static void updateHudVisibility() {
    if (g_state.hudContainer) {
        g_state.hudContainer->setVisible(g_state.modEnabled);
    }
    if (g_state.trajDrawer) {
        g_state.trajDrawer->setVisible(g_state.modEnabled);
    }
}

static void updateHud() {
    if (!g_state.bigLabel || !g_state.infoLabel || !g_state.tallyLabel) return;

    std::string tallyStr = "FRAME WINDOW COUNTER";
    for (int w = 1; w <= MAX_BUCKET; w++) {
        tallyStr += fmt::format("\n{}f: {}", w, g_state.tally[w]);
    }
    tallyStr += fmt::format("\n{}+: {}", MAX_BUCKET + 1, g_state.tally[MAX_BUCKET + 1]);
    g_state.tallyLabel->setString(tallyStr.c_str());

    if (!g_state.modEnabled) {
        g_state.bigLabel->setString("OFF");
        g_state.infoLabel->setString("Mod is OFF (Press F2 to toggle)");
        return;
    }

    if (g_state.mode == ModMode::Recording) {
        g_state.bigLabel->setString("REC");
        g_state.infoLabel->setString(fmt::format(
            "Actions recorded: {}\nPlay 0-100% (F3 to Stop/Start)",
            g_state.actions.size()
        ).c_str());
        return;
    }

    if (g_state.mode == ModMode::Scanning) {
        g_state.bigLabel->setString("...");
        g_state.infoLabel->setString(fmt::format(
            "Scanning action {}/{} (delta {:+d})",
            g_state.scanActionIdx + 1,
            g_state.actions.size(),
            g_state.scanDelta
        ).c_str());
        return;
    }

    if (!g_state.allAnalyses.empty()) {
        int idx = g_state.viewedActionIdx;
        if (idx < 0 || idx >= static_cast<int>(g_state.allAnalyses.size())) {
            idx = static_cast<int>(g_state.allAnalyses.size()) - 1;
        }
        const auto& a = g_state.allAnalyses[idx];

        g_state.bigLabel->setString(fmt::format("{}f", a.windowSize).c_str());

        std::string mapBar;
        for (const auto& t : a.trajectories) {
            if (t.delta == 0) mapBar += (t.passed ? '[' : 'x');
            else mapBar += (t.passed ? 'O' : '.');
        }

        g_state.infoLabel->setString(fmt::format(
            "Action #{}/{} ({} @ tick {})\nBack: {}f | Front: +{}f (Window: {:+d} to {:+d})\n{}",
            idx + 1, g_state.allAnalyses.size(),
            a.isDown ? "CLICK" : "RELEASE",
            a.originalTick,
            -a.minDelta, a.maxDelta,
            a.minDelta, a.maxDelta,
            mapBar
        ).c_str());
    } else {
        g_state.bigLabel->setString("-");
        g_state.infoLabel->setString(fmt::format(
            "Macro: {} actions\nF3: REC | F4: SCAN | F2: Toggle",
            g_state.actions.size()
        ).c_str());
    }
}

static std::vector<MacroAction> buildPerturbedMacro(int targetIdx, int delta) {
    auto modified = g_state.actions;
    int origTick = modified[targetIdx].tick;
    int newTick = std::max(0, origTick + delta);

    if (targetIdx > 0 && newTick <= modified[targetIdx - 1].tick) {
        newTick = modified[targetIdx - 1].tick + 1;
    }
    int actualShift = newTick - origTick;
    modified[targetIdx].tick = newTick;

    if (modified[targetIdx].down && targetIdx + 1 < static_cast<int>(modified.size())) {
        if (!modified[targetIdx + 1].down && modified[targetIdx + 1].tick <= newTick) {
            modified[targetIdx + 1].tick = std::max(newTick + 1, modified[targetIdx + 1].tick + actualShift);
        }
    }
    return modified;
}

class $modify(TrajPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g_state = EngineState{};
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto ws = CCDirector::get()->getWinSize();

        // Tạo node chứa HUD để có thể ẩn/hiện toàn bộ khi tắt mod
        g_state.hudContainer = CCNode::create();
        this->addChild(g_state.hudContainer, 1000);

        g_state.bigLabel   = makeHudLabel(g_state.hudContainer, 0.9f,  {0.5f, 1.f}, {ws.width / 2, ws.height - 6.f});
        g_state.infoLabel  = makeHudLabel(g_state.hudContainer, 0.28f, {0.5f, 1.f}, {ws.width / 2, ws.height - 48.f});
        g_state.tallyLabel = makeHudLabel(g_state.hudContainer, 0.28f, {0.f, 1.f},  {6.f, ws.height - 6.f});
        g_state.frameLabel = makeHudLabel(g_state.hudContainer, 0.4f,  {0.5f, 0.f}, {ws.width / 2, 6.f});

        g_state.trajDrawer = CCDrawNode::create();
        if (m_objectLayer) {
            m_objectLayer->addChild(g_state.trajDrawer, 1500);
        }

        // Menu nút bấm trên màn hình (vẫn giữ để dùng song song trên PC hoặc Mobile)
        auto menu = CCMenu::create();
        menu->setPosition({ws.width - 115.f, 24.f});
        g_state.hudContainer->addChild(menu, 10);

        auto recSpr = ButtonSprite::create("REC", 36, true, "bigFont.fnt", "GJ_button_01.png", 24.f, 0.45f);
        auto btnRec = CCMenuItemSpriteExtra::create(recSpr, this, menu_selector(TrajPlayLayer::onToggleRecord));
        btnRec->setPosition({-65.f, 0.f});

        auto scanSpr = ButtonSprite::create("SCAN", 42, true, "bigFont.fnt", "GJ_button_02.png", 24.f, 0.45f);
        auto btnScan = CCMenuItemSpriteExtra::create(scanSpr, this, menu_selector(TrajPlayLayer::onStartScan));
        btnScan->setPosition({-12.f, 0.f});

        auto prevSpr = ButtonSprite::create("<", 20, true, "bigFont.fnt", "GJ_button_04.png", 24.f, 0.45f);
        auto btnPrev = CCMenuItemSpriteExtra::create(prevSpr, this, menu_selector(TrajPlayLayer::onPrevAction));
        btnPrev->setPosition({34.f, 0.f});

        auto nextSpr = ButtonSprite::create(">", 20, true, "bigFont.fnt", "GJ_button_04.png", 24.f, 0.45f);
        auto btnNext = CCMenuItemSpriteExtra::create(nextSpr, this, menu_selector(TrajPlayLayer::onNextAction));
        btnNext->setPosition({64.f, 0.f});

        auto trajSpr = ButtonSprite::create("TRAJ", 38, true, "bigFont.fnt", "GJ_button_05.png", 24.f, 0.4f);
        auto btnTraj = CCMenuItemSpriteExtra::create(trajSpr, this, menu_selector(TrajPlayLayer::onToggleTraj));
        btnTraj->setPosition({98.f, 0.f});

        menu->addChild(btnRec);
        menu->addChild(btnScan);
        menu->addChild(btnPrev);
        menu->addChild(btnNext);
        menu->addChild(btnTraj);

        updateHudVisibility();
        updateHud();
        return true;
    }

    void onToggleRecord(CCObject*) {
        if (!g_state.modEnabled) return;
        if (g_state.mode == ModMode::Recording) {
            g_state.mode = ModMode::Idle;
            updateHud();
            return;
        }
        g_state.actions.clear();
        g_state.ckptMap.clear();
        g_state.allAnalyses.clear();
        if (g_state.trajDrawer) g_state.trajDrawer->clear();

        g_state.mode = ModMode::Recording;
        this->resetLevel();
        updateHud();
    }

    void onStartScan(CCObject*) {
        if (!g_state.modEnabled) return;
        if (g_state.mode == ModMode::Scanning) {
            g_state.mode = ModMode::Idle;
            this->resetLevel();
            updateHud();
            return;
        }
        if (g_state.actions.empty()) {
            FLAlertLayer::create("No Record", "Hãy bấm REC và hoàn thành một lượt chạy trong Practice Mode trước!", "OK")->show();
            return;
        }

        g_state.allAnalyses.clear();
        std::fill(std::begin(g_state.tally), std::end(g_state.tally), 0);

        g_state.scanActionIdx = 0;
        g_state.scanDelta = -MAX_SCAN_DELTA;
        g_state.currentAnalysis = ActionAnalysis{
            0, g_state.actions[0].tick, g_state.actions[0].down, 0, 0, 0, {}
        };
        g_state.mode = ModMode::Scanning;
        updateHud();
    }

    void onPrevAction(CCObject*) {
        if (!g_state.modEnabled || g_state.allAnalyses.empty()) return;
        g_state.viewedActionIdx = (g_state.viewedActionIdx <= 0) ? static_cast<int>(g_state.allAnalyses.size()) - 1 : g_state.viewedActionIdx - 1;
        drawCurrentTrajectories();
        updateHud();
    }

    void onNextAction(CCObject*) {
        if (!g_state.modEnabled || g_state.allAnalyses.empty()) return;
        g_state.viewedActionIdx = (g_state.viewedActionIdx + 1) % static_cast<int>(g_state.allAnalyses.size());
        drawCurrentTrajectories();
        updateHud();
    }

    void onToggleTraj(CCObject*) {
        if (!g_state.modEnabled) return;
        g_state.showTrajectory = !g_state.showTrajectory;
        drawCurrentTrajectories();
    }

    // Bắt sự kiện phím bàn phím trên PC
    bool keyDown(enumKeyCodes key) {
        auto toggleKey = Mod::get()->getSettingValue<geode::Keybind>("toggle-mod-key");
        auto recKey = Mod::get()->getSettingValue<geode::Keybind>("rec-key");
        auto scanKey = Mod::get()->getSettingValue<geode::Keybind>("scan-key");

        if (toggleKey.isKeyImmediate(key)) {
            g_state.modEnabled = !g_state.modEnabled;
            updateHudVisibility();
            updateHud();
            return true;
        }

        if (g_state.modEnabled) {
            if (recKey.isKeyImmediate(key)) {
                this->onToggleRecord(nullptr);
                return true;
            }
            if (scanKey.isKeyImmediate(key)) {
                this->onStartScan(nullptr);
                return true;
            }
        }

        return PlayLayer::keyDown(key);
    }

    CheckpointObject* createCheckpoint() {
        auto ckpt = PlayLayer::createCheckpoint();
        if (ckpt && g_state.modEnabled && g_state.mode == ModMode::Recording) {
            g_state.ckptMap[ckpt] = CheckpointState{ g_state.currentTick, g_state.actions.size() };
        }
        return ckpt;
    }

    void resetLevel() {
        this->handleButton(false, 1, true);
        PlayLayer::resetLevel();

        if (!g_state.modEnabled) return;

        if (g_state.mode == ModMode::Recording) {
            if (m_isPracticeMode && m_checkpointArray && m_checkpointArray->count() > 0) {
                auto lastCkpt = static_cast<CheckpointObject*>(m_checkpointArray->lastObject());
                auto it = g_state.ckptMap.find(lastCkpt);
                if (it != g_state.ckptMap.end()) {
                    g_state.currentTick = it->second.tick;
                    if (g_state.actions.size() > it->second.actionCount) {
                        g_state.actions.resize(it->second.actionCount);
                    }
                    updateHud();
                    return;
                }
            }
            g_state.currentTick = 0;
            g_state.actions.clear();
            updateHud();
        } else if (!g_state.isSimulating) {
            g_state.currentTick = 0;
            g_state.playbackIdx = 0;
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (object == m_anticheatSpike) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }
        PlayLayer::destroyPlayer(player, object);

        if (g_state.isSimulating && player->m_isDead) {
            g_state.simDead = true;
        }
    }

    void runSingleTrajectorySim(int actionIdx, int delta) {
        auto modifiedMacro = buildPerturbedMacro(actionIdx, delta);
        int baseTick = g_state.actions[actionIdx].tick;
        int targetEndTick = baseTick + LOOKAHEAD_TICKS;
        if (actionIdx + 1 < static_cast<int>(g_state.actions.size())) {
            targetEndTick = std::max(targetEndTick, g_state.actions[actionIdx + 1].tick + 30);
        }
        int drawStartTick = std::max(0, baseTick - MAX_SCAN_DELTA - 5);

        bool savedPractice = m_isPracticeMode;
        m_isPracticeMode = false;

        g_state.isSimulating = true;
        g_state.simDead = false;
        this->resetLevel();

        g_state.currentTick = 0;
        size_t macroIdx = 0;
        TrajectoryLine trajLine;
        trajLine.delta = delta;
        trajLine.passed = true;

        constexpr float FIXED_DT = 1.f / 240.f;

        while (g_state.currentTick <= targetEndTick) {
            while (macroIdx < modifiedMacro.size() && modifiedMacro[macroIdx].tick == g_state.currentTick) {
                this->handleButton(modifiedMacro[macroIdx].down, modifiedMacro[macroIdx].button, modifiedMacro[macroIdx].player1);
                macroIdx++;
            }

            GJBaseGameLayer::processCommands(FIXED_DT, false, true);

            if (g_state.currentTick >= drawStartTick && m_player1) {
                trajLine.points.push_back(m_player1->getPosition());
            }

            if (g_state.simDead || (m_player1 && m_player1->m_isDead)) {
                trajLine.passed = false;
                break;
            }
            if (m_hasCompletedLevel) {
                trajLine.passed = true;
                break;
            }

            g_state.currentTick++;
        }

        this->handleButton(false, 1, true);
        g_state.isSimulating = false;
        m_isPracticeMode = savedPractice;

        g_state.currentAnalysis.trajectories.push_back(std::move(trajLine));
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (g_state.frameLabel && !g_state.isSimulating) {
            g_state.frameLabel->setString(fmt::format("Tick: {}", g_state.currentTick).c_str());
        }

        if (g_state.modEnabled && g_state.mode == ModMode::Scanning) {
            runSingleTrajectorySim(g_state.scanActionIdx, g_state.scanDelta);

            g_state.scanDelta++;
            if (g_state.scanDelta > MAX_SCAN_DELTA) {
                int minD = 0, maxD = 0, winLen = 0;
                for (const auto& t : g_state.currentAnalysis.trajectories) {
                    if (t.passed) winLen++;
                }

                for (int d = -1; d >= -MAX_SCAN_DELTA; d--) {
                    auto it = std::find_if(g_state.currentAnalysis.trajectories.begin(), g_state.currentAnalysis.trajectories.end(), [d](const TrajectoryLine& l) { return l.delta == d; });
                    if (it != g_state.currentAnalysis.trajectories.end() && it->passed) minD = d;
                    else break;
                }
                for (int d = 1; d <= MAX_SCAN_DELTA; d++) {
                    auto it = std::find_if(g_state.currentAnalysis.trajectories.begin(), g_state.currentAnalysis.trajectories.end(), [d](const TrajectoryLine& l) { return l.delta == d; });
                    if (it != g_state.currentAnalysis.trajectories.end() && it->passed) maxD = d;
                    else break;
                }

                int contiguousWindow = (maxD - minD) + 1;
                auto zeroIt = std::find_if(g_state.currentAnalysis.trajectories.begin(), g_state.currentAnalysis.trajectories.end(), [](const TrajectoryLine& l) { return l.delta == 0; });
                if (zeroIt != g_state.currentAnalysis.trajectories.end() && !zeroIt->passed) {
                    contiguousWindow = winLen;
                }

                g_state.currentAnalysis.minDelta = minD;
                g_state.currentAnalysis.maxDelta = maxD;
                g_state.currentAnalysis.windowSize = contiguousWindow;

                if (contiguousWindow > 0) {
                    int bucket = contiguousWindow > MAX_BUCKET ? MAX_BUCKET + 1 : contiguousWindow;
                    g_state.tally[bucket]++;
                }

                g_state.allAnalyses.push_back(g_state.currentAnalysis);
                g_state.viewedActionIdx = static_cast<int>(g_state.allAnalyses.size()) - 1;
                drawCurrentTrajectories();

                g_state.scanActionIdx++;
                if (g_state.scanActionIdx >= static_cast<int>(g_state.actions.size())) {
                    g_state.mode = ModMode::Replaying;
                    this->resetLevel();
                    updateHud();
                    return;
                }

                g_state.scanDelta = -MAX_SCAN_DELTA;
                g_state.currentAnalysis = ActionAnalysis{
                    g_state.scanActionIdx, g_state.actions[g_state.scanActionIdx].tick, g_state.actions[g_state.scanActionIdx].down, 0, 0, 0, {}
                };
            }
            updateHud();
        }
    }
};

class $modify(TrajGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_state.modEnabled && PlayLayer::get() && !g_state.isSimulating && g_state.mode == ModMode::Recording) {
            g_state.actions.push_back(MacroAction{ g_state.currentTick, down, button, isPlayer1 });
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (g_state.modEnabled && PlayLayer::get() && !g_state.isSimulating) {
            if (g_state.mode == ModMode::Replaying) {
                while (g_state.playbackIdx < g_state.actions.size() &&
                       g_state.actions[g_state.playbackIdx].tick == g_state.currentTick) {
                    const auto& act = g_state.actions[g_state.playbackIdx];
                    GJBaseGameLayer::handleButton(act.down, act.button, act.player1);

                    if (g_state.playbackIdx < g_state.allAnalyses.size()) {
                        g_state.viewedActionIdx = static_cast<int>(g_state.playbackIdx);
                        drawCurrentTrajectories();
                        updateHud();
                    }
                    g_state.playbackIdx++;
                }
            }
            g_state.currentTick++;
        }
        GJBaseGameLayer::processCommands(dt, isHalfTech, isLastTick);
    }
};
