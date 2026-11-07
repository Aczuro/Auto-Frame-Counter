#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

using namespace geode::prelude;

// ===== Chỉnh các số này cho hợp với cú nhảy cần đo =====
constexpr int  MAX_OFFSET    = 60;
constexpr int  HOLD_TICKS    = 4;
constexpr int  SURVIVE_TICKS = 120;   // sống đủ bấy nhiêu tick (tính từ lúc bấm) thì tính là "qua"
constexpr int  MAX_BUCKET    = 10;
constexpr bool SHOW_TALLY    = true;
constexpr bool SHOW_FRAME    = true;

struct TestState {
    bool running = false;
    bool trialActive = false;
    bool needReset = false;
    int tick = 0;
    int shownTick = -1;
    std::vector<bool> results;
    int tally[MAX_BUCKET + 2] = {};

    CCLabelBMFont* big = nullptr;
    CCLabelBMFont* info = nullptr;
    CCLabelBMFont* tallyLabel = nullptr;
    CCLabelBMFont* frameLabel = nullptr;
};
inline TestState g_test;

static CCLabelBMFont* makeLabel(CCNode* parent, float scale, CCPoint anchor, CCPoint pos) {
    auto label = CCLabelBMFont::create("", "bigFont.fnt");
    label->setScale(scale);
    label->setOpacity(200);
    label->setAnchorPoint(anchor);
    label->setPosition(pos);
    parent->addChild(label, 1000);
    return label;
}

static void findWindow(int& start, int& len) {
    start = -1;
    len = 0;
    int curStart = 0, curLen = 0;
    for (int i = 0; i < static_cast<int>(g_test.results.size()); i++) {
        if (g_test.results[i]) {
            if (curLen == 0) curStart = i;
            curLen++;
            if (curLen > len) { len = curLen; start = curStart; }
        } else {
            curLen = 0;
        }
    }
}

static void refreshHud() {
    int start, len;
    findWindow(start, len);

    if (g_test.big) {
        std::string s = len > 0 ? fmt::format("{}", len) : "-";
        g_test.big->setString(s.c_str());
    }

    if (g_test.info) {
        std::string s;
        if (len > 0) s += fmt::format("tick {}-{}\n", start, start + len - 1);
        s += fmt::format("tested {}/{}", g_test.results.size(), MAX_OFFSET + 1);
        for (size_t i = 0; i < g_test.results.size(); i++) {
            if (i % 30 == 0) s += "\n";
            s += g_test.results[i] ? 'O' : '.';
        }
        g_test.info->setString(s.c_str());
    }

    if (g_test.tallyLabel) {
        std::string s = "FRAME WINDOW COUNTER";
        for (int w = 1; w <= MAX_BUCKET; w++) {
            s += fmt::format("\n{}f: {}", w, g_test.tally[w]);
        }
        s += fmt::format("\n{}+: {}", MAX_BUCKET + 1, g_test.tally[MAX_BUCKET + 1]);
        g_test.tallyLabel->setString(s.c_str());
    }
}

static void finishTest() {
    g_test.running = false;
    g_test.trialActive = false;

    int start, len;
    findWindow(start, len);
    if (len > 0) g_test.tally[len > MAX_BUCKET ? MAX_BUCKET + 1 : len]++;
    refreshHud();
    log::info("Frame window = {} (tick {} -> {})", len, start, start + len - 1);
}

class $modify(WindowPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g_test = TestState{};
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto ws = CCDirector::get()->getWinSize();

        g_test.big = makeLabel(this, 1.0f, {0.5f, 1.f}, {ws.width / 2, ws.height - 8.f});
        g_test.info = makeLabel(this, 0.3f, {0.5f, 1.f}, {ws.width / 2, ws.height - 55.f});
        if (SHOW_TALLY) g_test.tallyLabel = makeLabel(this, 0.3f, {0.f, 1.f}, {6.f, ws.height - 6.f});
        if (SHOW_FRAME) g_test.frameLabel = makeLabel(this, 0.45f, {0.5f, 0.f}, {ws.width / 2, 6.f});
        refreshHud();

        auto btn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Test"), this, menu_selector(WindowPlayLayer::onToggleTest)
        );
        auto menu = CCMenu::create();
        menu->addChild(btn);
        menu->setPosition({ws.width - 45.f, 25.f});
        this->addChild(menu, 1000);

        return true;
    }

    void onToggleTest(CCObject*) {
        if (g_test.running) {
            g_test.running = false;
            g_test.trialActive = false;
            refreshHud();
            return;
        }
        if (!m_isPracticeMode) {
            FLAlertLayer::create(
                "Frame window",
                "Turn on Practice Mode and place a checkpoint right before the jump first!",
                "OK"
            )->show();
            return;
        }
        g_test.results.clear();
        g_test.running = true;
        g_test.needReset = true;
        refreshHud();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_test.tick = 0;
        if (!g_test.running) return;

        if (static_cast<int>(g_test.results.size()) > MAX_OFFSET) {
            finishTest();
            return;
        }
        g_test.trialActive = true;
        refreshHud();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_test.running && g_test.trialActive) {
            g_test.results.push_back(false);
            g_test.trialActive = false;
            refreshHud();
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (g_test.frameLabel && g_test.tick != g_test.shownTick) {
            g_test.shownTick = g_test.tick;
            g_test.frameLabel->setString(fmt::format("Frame: {}", g_test.tick).c_str());
        }
        if (g_test.needReset) {
            g_test.needReset = false;
            this->resetLevel();
        }
    }
};

class $modify(WindowGameLayer, GJBaseGameLayer) {
    // GD 2.2081: processCommands(float, bool, bool)
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (PlayLayer::get()) {
            if (g_test.running && g_test.trialActive) {
                int k = static_cast<int>(g_test.results.size());

                if (g_test.tick == k) {
                    this->handleButton(true, 1, true);
                }
                if (g_test.tick == k + HOLD_TICKS) {
                    this->handleButton(false, 1, true);
                }
                if (g_test.tick >= k + SURVIVE_TICKS) {
                    g_test.results.push_back(true);
                    g_test.trialActive = false;
                    g_test.needReset = true;
                }
            }
            g_test.tick++;
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};
