#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CheckpointObject.hpp>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <deque>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;

// ===== Cấu hình Rolling Scan & Trajectory =====
constexpr int MAX_BACKWARD_FRAMES   = 12;  // Số frame tối đa quét lùi trước mỗi Press/Release
constexpr int MAX_FORWARD_FRAMES    = 12;  // Số frame tối đa quét tiến sau mỗi Press/Release
constexpr int SURVIVE_CHECK_FRAMES  = 72;  // Số frame nhân vật phải sống sót sau thao tác
constexpr int ACTIONS_PER_UI_TICK   = 3;   // Số action quét cuốn chiếu mỗi frame hình ảnh (tránh treo game)
constexpr int PREVIEW_STEPS         = 110; // Độ dài đường dự đoán Ahead Trajectory
constexpr int WHITE_TRAIL_MAX_PTS   = 180; // Độ dài đường quỹ đạo trắng (White Trajectory Arc)
constexpr int MAX_BUCKET            = 10;  // Thống kê từ 1F đến 10F và 11F+

// ===== Phân loại rõ 2 loại Action của Macro =====
enum class ActionType : int {
    Press   = 1, // Type 1: Press (Nhấn chuột / phím)
    Release = 2  // Type 2: Release (Nhả chuột / phím)
};

static const char* actionTypeToString(ActionType t) {
    return (t == ActionType::Press) ? "Type 1: Press" : "Type 2: Release";
}

enum class BotStage {
    Record,       // Ghi Macro trong Practice Mode (có Save/Load State chống desync)
    RollingScan,  // Quét cuốn chiếu tĩnh từ đầu đến cuối level -> xuất file JSON
    Replay        // Đọc dữ liệu từ file JSON và phát lại Replay + hiển thị Frame Window
};

struct TrajectoryBranch {
    int delta = 0;
    bool survived = false;
    std::vector<CCPoint> points;
};

struct MacroAction {
    int index = 0;
    int frame = 0;
    ActionType type = ActionType::Press; // 1 = Press, 2 = Release
    int button = 1;
    bool player1 = true;
    float x = 0.f;
    float y = 0.f;

    // Kết quả sau khi Rolling Scan
    bool analyzed = false;
    int backwardFrames = 0;
    int forwardFrames = 0;
    int windowSize = 1;
};

// ============================================================================
// HỆ THỐNG SAVE / LOAD STATE 3 TẦNG CHỐNG DESYNC (PLAYER + ORB/PORTAL + TRIGGER)
// ============================================================================

// Helper C++20 kiểm tra biến thành viên an toàn tại thời điểm biên dịch
template <typename T>
static void readEnhancedFlags(T* obj, bool& act1, bool& act2) {
    if constexpr (requires { obj->m_hasBeenActivated; }) {
        act1 = obj->m_hasBeenActivated;
    }
    if constexpr (requires { obj->m_hasBeenActivatedP2; }) {
        act2 = obj->m_hasBeenActivatedP2;
    }
}

template <typename T>
static void writeEnhancedFlags(T* obj, bool act1, bool act2) {
    if constexpr (requires { obj->m_hasBeenActivated; }) {
        obj->m_hasBeenActivated = act1;
    }
    if constexpr (requires { obj->m_hasBeenActivatedP2; }) {
        obj->m_hasBeenActivatedP2 = act2;
    }
}

template <typename P>
static void clearPlayerRingTouchState(P* p) {
    if constexpr (requires { p->m_touchedRings.clear(); }) {
        p->m_touchedRings.clear();
    } else if constexpr (requires { p->m_touchedRings->removeAllObjects(); }) {
        if (p->m_touchedRings) p->m_touchedRings->removeAllObjects();
    }
    if constexpr (requires { p->m_jumpBuffered = false; }) {
        p->m_jumpBuffered = false;
    }
}

// Tầng 2: Snapshot trạng thái từng GameObject / Orb / Portal / Pad / Trigger
struct ObjectStateEntry {
    GameObject* obj = nullptr;
    CCPoint pos{0.f, 0.f};
    CCPoint startPos{0.f, 0.f};
    float rotX = 0.f;
    float rotY = 0.f;
    float scaleX = 1.f;
    float scaleY = 1.f;
    bool visible = true;
    GLubyte opacity = 255;
    bool isDisabled = false;
    bool isDisabled2 = false;
    bool isEnhanced = false;
    bool hasBeenActivatedP1 = false;
    bool hasBeenActivatedP2 = false;

    static ObjectStateEntry capture(GameObject* o) {
        ObjectStateEntry e;
        e.obj         = o;
        e.pos         = o->getPosition();
        e.startPos    = o->m_startPosition;
        e.rotX        = o->getRotationX();
        e.rotY        = o->getRotationY();
        e.scaleX      = o->getScaleX();
        e.scaleY      = o->getScaleY();
        e.visible     = o->isVisible();
        e.opacity     = o->getOpacity();
        e.isDisabled  = o->m_isDisabled;
        e.isDisabled2 = o->m_isDisabled2;

        if (auto enh = typeinfo_cast<EnhancedGameObject*>(o)) {
            e.isEnhanced = true;
            readEnhancedFlags(enh, e.hasBeenActivatedP1, e.hasBeenActivatedP2);
        }
        return e;
    }

    void restore() const {
        if (!obj) return;

        // Nếu vật thể (Orb, Pad, Portal, Trigger, Coin) chưa kích hoạt ở frame gốc
        // nhưng bị kích hoạt trong nhánh thử nghiệm -> gọi resetObject() để xóa sạch trạng thái kích hoạt!
        if (!hasBeenActivatedP1 && !hasBeenActivatedP2) {
            obj->resetObject();
        }

        obj->setPosition(pos);
        obj->m_startPosition = startPos;
        obj->setRotationX(rotX);
        obj->setRotationY(rotY);
        obj->setScaleX(scaleX);
        obj->setScaleY(scaleY);
        obj->setVisible(visible);
        obj->setOpacity(opacity);
        obj->m_isDisabled  = isDisabled;
        obj->m_isDisabled2 = isDisabled2;

        if (isEnhanced) {
            if (auto enh = typeinfo_cast<EnhancedGameObject*>(obj)) {
                writeEnhancedFlags(enh, hasBeenActivatedP1, hasBeenActivatedP2);
            }
        }
    }
};

// Tầng 3: Snapshot toàn diện cho PlayerObject (P1 & P2)
struct PlayerExactSnapshot {
    bool valid = false;
    CCPoint pos{0.f, 0.f};
    float rotation = 0.f;
    double yVel = 0.0;
    double platXVel = 0.0;
    double gravity = 0.958199;
    float gravityMod = 1.0f;
    float speed = 0.9f;
    float vehicleSize = 1.0f;

    bool upsideDown = false;
    bool onGround = false;
    bool goingLeft = false;
    bool sideways = false;
    bool dashing = false;
    bool holding = false;
    bool holding2 = false;
    bool hasJustHeld = false;
    bool hasJustHeld2 = false;
    bool canRobotJump = false;

    bool isShip = false;
    bool isUfo = false;
    bool isBall = false;
    bool isWave = false;
    bool isRobot = false;
    bool isSpider = false;
    bool isSwing = false;

    static PlayerExactSnapshot capture(PlayerObject* p) {
        PlayerExactSnapshot s;
        if (!p) return s;
        s.valid        = true;
        s.pos          = p->getPosition();
        s.rotation     = p->getRotation();
        s.yVel         = p->m_yVelocity;
        s.platXVel     = p->m_platformerXVelocity;
        s.gravity      = p->m_gravity;
        s.gravityMod   = p->m_gravityMod;
        s.speed        = p->m_playerSpeed;
        s.vehicleSize  = p->m_vehicleSize;

        s.upsideDown   = p->m_isUpsideDown;
        s.onGround     = p->m_isOnGround;
        s.goingLeft    = p->m_isGoingLeft;
        s.sideways     = p->m_isSideways;
        s.dashing      = p->m_isDashing;
        s.holding      = p->m_isHolding;
        s.holding2     = p->m_isHolding2;
        s.hasJustHeld  = p->m_hasJustHeld;
        s.hasJustHeld2 = p->m_hasJustHeld2;
        s.canRobotJump = p->m_canRobotJump;

        s.isShip       = p->m_isShip;
        s.isUfo        = p->m_isBird;
        s.isBall       = p->m_isBall;
        s.isWave       = p->m_isDart;
        s.isRobot      = p->m_isRobot;
        s.isSpider     = p->m_isSpider;
        s.isSwing      = p->m_isSwing;
        return s;
    }

    void restore(PlayerObject* p) const {
        if (!valid || !p) return;
        p->m_isDead              = false;
        p->setPosition(pos);
        p->m_position            = pos;
        p->setRotation(rotation);
        p->m_yVelocity           = yVel;
        p->m_platformerXVelocity = platXVel;
        p->m_gravity             = gravity;
        p->m_gravityMod          = gravityMod;
        p->m_playerSpeed         = speed;
        p->m_vehicleSize         = vehicleSize;

        p->m_isUpsideDown        = upsideDown;
        p->m_isOnGround          = onGround;
        p->m_isGoingLeft         = goingLeft;
        p->m_isSideways          = sideways;
        p->m_isDashing           = dashing;
        p->m_isHolding           = holding;
        p->m_isHolding2          = holding2;
        p->m_hasJustHeld         = hasJustHeld;
        p->m_hasJustHeld2        = hasJustHeld2;
        p->m_canRobotJump        = canRobotJump;

        p->m_isShip              = isShip;
        p->m_isBird              = isUfo;
        p->m_isBall              = isBall;
        p->m_isDart              = isWave;
        p->m_isRobot             = isRobot;
        p->m_isSpider            = isSpider;
        p->m_isSwing             = isSwing;

        if (!holding) {
            clearPlayerRingTouchState(p);
        }
    }
};

// Gói Snapshot hoàn chỉnh của PlayLayer tại một Frame cụ thể
struct PlayLayerStateSnapshot {
    bool valid = false;
    int frame = 0;
    size_t macroInputCount = 0;
    bool holdingP1 = false;
    bool holdingP2 = false;

    Ref<CheckpointObject> nativeCkpt = nullptr; // Tầng 1: GJGameState, Trigger đang chạy, Camera, Portal
    PlayerExactSnapshot p1Snap;                 // Tầng 3: Player 1
    PlayerExactSnapshot p2Snap;                 // Tầng 3: Player 2
    std::vector<ObjectStateEntry> objectsSnap;  // Tầng 2: Toàn bộ Orb, Pad, Portal, Trigger quanh nhân vật

    static PlayLayerStateSnapshot capture(PlayLayer* pl, int curFrame, size_t inputCount, bool hold1, bool hold2) {
        PlayLayerStateSnapshot snap;
        if (!pl || !pl->m_player1) return snap;

        snap.valid           = true;
        snap.frame           = curFrame;
        snap.macroInputCount = inputCount;
        snap.holdingP1       = hold1;
        snap.holdingP2       = hold2;

        // 1. Tạo Checkpoint gốc của GD để lưu m_gameState (Trigger đang chạy dở, dịch chuyển, tốc độ, camera)
        snap.nativeCkpt = pl->createCheckpoint();
        if (snap.nativeCkpt) {
            if constexpr (requires { snap.nativeCkpt->m_physicalCheckpointObject; }) {
                if (snap.nativeCkpt->m_physicalCheckpointObject) {
                    snap.nativeCkpt->m_physicalCheckpointObject->setVisible(false);
                    snap.nativeCkpt->m_physicalCheckpointObject->removeFromParent();
                }
            }
        }

        // 2. Lưu trạng thái chi tiết của Player 1 & Player 2
        snap.p1Snap = PlayerExactSnapshot::capture(pl->m_player1);
        snap.p2Snap = PlayerExactSnapshot::capture(pl->m_player2);

        // 3. Lưu trạng thái toàn bộ các Object trong vùng ảnh hưởng (Orb, Portal, Pad, Trigger, Moving Block)
        if (pl->m_objects) {
            float px = pl->m_player1->getPositionX();
            float minX = px - 900.f;
            float maxX = px + 3200.f;

            int count = pl->m_objects->count();
            snap.objectsSnap.reserve(256);
            for (int i = 0; i < count; i++) {
                auto obj = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
                if (!obj) continue;
                float ox = obj->getPositionX();
                if (ox >= minX && ox <= maxX) {
                    snap.objectsSnap.push_back(ObjectStateEntry::capture(obj));
                }
            }
        }

        return snap;
    }

    void restore(PlayLayer* pl) const {
        if (!valid || !pl) return;

        // 1. Khôi phục m_gameState, các Trigger đang kích hoạt dở và cổng dịch chuyển qua Checkpoint gốc
        if (nativeCkpt) {
            pl->loadFromCheckpoint(nativeCkpt);
        }

        // 2. Khôi phục toàn bộ Orb, Pad, Portal, Trigger & Moving Objects về đúng trạng thái tại frame gốc
        for (const auto& entry : objectsSnap) {
            entry.restore();
        }

        // 3. Khôi phục chính xác vị trí, vận tốc, trạng thái giữ chuột và vòng Orb của Player 1 & Player 2
        p1Snap.restore(pl->m_player1);
        p2Snap.restore(pl->m_player2);
    }
};

// ===== Trạng thái toàn cục của Mod =====
struct AFCState {
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

    // Dữ liệu Macro và Snapshot
    std::vector<MacroAction> actions;
    std::unordered_map<CheckpointObject*, PlayLayerStateSnapshot> practiceSnapshots;

    // Trạng thái Quét cuốn chiếu (Rolling Scan)
    bool rollingScanInitialized = false;
    size_t rollingActionIdx = 0;
    int rollingMainFrame = 0;
    size_t rollingMainMacroIdx = 0;
    PlayLayerStateSnapshot rollingMainSnapshot;
    std::vector<TrajectoryBranch> activeBranches;

    // Thống kê và hiển thị
    int tally[MAX_BUCKET + 2] = {};
    std::deque<CCPoint> whiteTrail;
    std::string lastPopupText;
    int popupTimer = 0;

    // Node đồ họa
    CCNode* hudRoot = nullptr;
    CCDrawNode* worldDrawer = nullptr;
    CCLabelBMFont* tallyLabel = nullptr;
    CCLabelBMFont* statusLabel = nullptr;
    CCLabelBMFont* popupLabel = nullptr;
    CCLabelBMFont* playerPopupLabel = nullptr;
};

inline AFCState g_afc;

// ============================================================================
// HỆ THỐNG LƯU & ĐỌC FILE JSON (`level_<id>.json`)
// ============================================================================

static std::filesystem::path getLevelJsonPath(GJGameLevel* level) {
    int id = level ? level->m_levelID.value() : 0;
    std::string filename = (id > 0)
        ? fmt::format("level_{}.json", id)
        : fmt::format("editor_{}.json", level ? std::string(level->m_levelName) : "custom");
    return Mod::get()->getSaveDir() / filename;
}

static void recomputeTally() {
    std::fill(std::begin(g_afc.tally), std::end(g_afc.tally), 0);
    for (const auto& act : g_afc.actions) {
        if (act.analyzed && act.windowSize > 0) {
            int b = (act.windowSize > MAX_BUCKET) ? (MAX_BUCKET + 1) : act.windowSize;
            g_afc.tally[b]++;
        }
    }
}

static void saveMacroToJson(GJGameLevel* level) {
    if (!level) return;
    recomputeTally();

    auto path = getLevelJsonPath(level);
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return;

    int levelId = level->m_levelID.value();
    out << "{\n";
    out << "  \"level_id\": " << levelId << ",\n";
    out << "  \"target_fps\": " << g_afc.targetFps << ",\n";
    out << "  \"analyzed\": " << (g_afc.analysisCompleted ? "true" : "false") << ",\n";
    out << "  \"total_actions\": " << g_afc.actions.size() << ",\n";

    // Ghi thống kê Tally
    out << "  \"tally\": {\n";
    for (int w = 1; w <= MAX_BUCKET; w++) {
        out << "    \"" << w << "F\": " << g_afc.tally[w] << ",\n";
    }
    out << "    \"" << (MAX_BUCKET + 1) << "+F\": " << g_afc.tally[MAX_BUCKET + 1] << "\n";
    out << "  },\n";

    // Ghi danh sách các Action (Phân loại rõ Type 1: Press và Type 2: Release)
    out << "  \"actions\": [\n";
    for (size_t i = 0; i < g_afc.actions.size(); i++) {
        const auto& a = g_afc.actions[i];
        int typeCode = (a.type == ActionType::Press) ? 1 : 2;
        out << "    {\n";
        out << "      \"index\": " << i << ",\n";
        out << "      \"frame\": " << a.frame << ",\n";
        out << "      \"type\": " << typeCode << ",\n";
        out << "      \"type_label\": \"" << actionTypeToString(a.type) << "\",\n";
        out << "      \"button\": " << a.button << ",\n";
        out << "      \"player1\": " << (a.player1 ? "true" : "false") << ",\n";
        out << "      \"x\": " << std::fixed << std::setprecision(2) << a.x << ",\n";
        out << "      \"y\": " << std::fixed << std::setprecision(2) << a.y << ",\n";
        out << "      \"analyzed\": " << (a.analyzed ? "true" : "false") << ",\n";
        out << "      \"backward_frames\": " << a.backwardFrames << ",\n";
        out << "      \"forward_frames\": " << a.forwardFrames << ",\n";
        out << "      \"window_size\": " << a.windowSize << "\n";
        out << "    }" << (i + 1 < g_afc.actions.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

// Hàm đọc giá trị số/boolean từ một dòng hoặc khối JSON
static std::string extractJsonToken(const std::string& block, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = block.find(pattern);
    if (pos == std::string::npos) return "";
    pos = block.find(':', pos + pattern.size());
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < block.size() && (block[pos] == ' ' || block[pos] == '\t' || block[pos] == '\n' || block[pos] == '\r')) {
        pos++;
    }
    size_t end = pos;
    while (end < block.size() && block[end] != ',' && block[end] != '}' && block[end] != '\n' && block[end] != '\r') {
        end++;
    }
    return block.substr(pos, end - pos);
}

static bool loadMacroFromJson(GJGameLevel* level) {
    g_afc.actions.clear();
    g_afc.analysisCompleted = false;
    std::fill(std::begin(g_afc.tally), std::end(g_afc.tally), 0);

    if (!level) return false;
    auto path = getLevelJsonPath(level);
    if (!std::filesystem::exists(path)) return false;

    std::ifstream in(path);
    if (!in.is_open()) return false;

    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string content = buffer.str();
    if (content.empty()) return false;

    std::string topAnalyzed = extractJsonToken(content, "analyzed");
    g_afc.analysisCompleted = (topAnalyzed.find("true") != std::string::npos);

    size_t actionsPos = content.find("\"actions\"");
    if (actionsPos == std::string::npos) return false;
    size_t arrayStart = content.find('[', actionsPos);
    if (arrayStart == std::string::npos) return false;

    size_t cursor = arrayStart + 1;
    while (true) {
        size_t objStart = content.find('{', cursor);
        if (objStart == std::string::npos) break;
        size_t objEnd = content.find('}', objStart);
        if (objEnd == std::string::npos) break;

        std::string itemBlock = content.substr(objStart, objEnd - objStart + 1);
        MacroAction act;
        act.index          = std::atoi(extractJsonToken(itemBlock, "index").c_str());
        act.frame          = std::atoi(extractJsonToken(itemBlock, "frame").c_str());
        int typeInt        = std::atoi(extractJsonToken(itemBlock, "type").c_str());
        act.type           = (typeInt == 2) ? ActionType::Release : ActionType::Press;
        act.button         = std::atoi(extractJsonToken(itemBlock, "button").c_str());
        if (act.button == 0) act.button = 1;
        act.player1        = (extractJsonToken(itemBlock, "player1").find("false") == std::string::npos);
        act.x              = static_cast<float>(std::atof(extractJsonToken(itemBlock, "x").c_str()));
        act.y              = static_cast<float>(std::atof(extractJsonToken(itemBlock, "y").c_str()));
        act.analyzed       = (extractJsonToken(itemBlock, "analyzed").find("true") != std::string::npos);
        act.backwardFrames = std::atoi(extractJsonToken(itemBlock, "backward_frames").c_str());
        act.forwardFrames  = std::atoi(extractJsonToken(itemBlock, "forward_frames").c_str());
        act.windowSize     = std::max(1, std::atoi(extractJsonToken(itemBlock, "window_size").c_str()));

        g_afc.actions.push_back(act);
        cursor = objEnd + 1;
    }

    recomputeTally();
    return !g_afc.actions.empty();
}

// ===== Đồng bộ cài đặt từ Geode Settings =====
static void syncSettingsFromMod() {
    g_afc.enabled        = Mod::get()->getSettingValue<bool>("enable-mod");
    g_afc.showWhiteTrail = Mod::get()->getSettingValue<bool>("show-white-trail");
    g_afc.showAheadTraj  = Mod::get()->getSettingValue<bool>("show-ahead-trajectory");
    g_afc.targetFps      = std::clamp(static_cast<int>(Mod::get()->getSettingValue<int64_t>("target-fps")), 60, 1000);
}

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

// ============================================================================
// BỘ DỰ ĐOÁN QUỸ ĐẠO PHÍA TRƯỚC (AHEAD TRAJECTORY PREVIEW)
// ============================================================================
struct ExactPlayerSim {
    CCPoint pos;
    double yVel = 0.0;
    double gravity = 0.958199;
    float gravityMod = 1.0f;
    float speed = 0.9f;
    float vehicleSize = 1.0f;
    bool upsideDown = false;
    bool onGround = false;

    bool isShip = false;
    bool isUfo = false;
    bool isBall = false;
    bool isWave = false;
    bool isRobot = false;
    bool isSpider = false;
    bool isSwing = false;

    static ExactPlayerSim fromPlayer(PlayerObject* p) {
        ExactPlayerSim s;
        if (!p) return s;
        s.pos         = p->getPosition();
        s.yVel        = p->m_yVelocity;
        s.gravity     = p->m_gravity > 0.01 ? p->m_gravity : 0.958199;
        s.gravityMod  = p->m_gravityMod != 0.f ? p->m_gravityMod : 1.0f;
        s.speed       = p->m_playerSpeed;
        s.vehicleSize = p->m_vehicleSize > 0.1f ? p->m_vehicleSize : 1.0f;
        s.upsideDown  = p->m_isUpsideDown;
        s.onGround    = p->m_isOnGround;
        s.isShip      = p->m_isShip;
        s.isUfo       = p->m_isBird;
        s.isBall      = p->m_isBall;
        s.isWave      = p->m_isDart;
        s.isRobot     = p->m_isRobot;
        s.isSpider    = p->m_isSpider;
        s.isSwing     = p->m_isSwing;
        return s;
    }

    float getHorizontalUnitsPerSec() const {
        if (speed <= 0.75f) return 251.16008f;
        if (speed <= 0.95f) return 311.58010f;
        if (speed <= 1.15f) return 387.42014f;
        if (speed <= 1.35f) return 468.00015f;
        return 576.00018f;
    }

    void step(float dt, bool holding, bool justPressed) {
        const float stepScale = dt * 60.0f;
        const float gravDir   = upsideDown ? -1.0f : 1.0f;
        const float miniScale = (vehicleSize < 0.8f) ? 0.8f : 1.0f;

        float dx = getHorizontalUnitsPerSec() * dt;
        pos.x += dx;

        if (isWave) {
            float waveSlope = (vehicleSize < 0.8f) ? 2.0f : 1.0f;
            float dir = holding ? 1.0f : -1.0f;
            if (upsideDown) dir = -dir;
            yVel = (dx / stepScale) * dir * waveSlope;
            pos.y += static_cast<float>(yVel * stepScale);
            return;
        }

        if (isShip) {
            double accel = holding ? 1.35 : -1.08;
            if (vehicleSize < 0.8f) accel *= 1.18;
            yVel += accel * gravDir * gravityMod * (stepScale * 0.5f);
            double maxUp = (vehicleSize < 0.8f) ? 8.5 : 8.0;
            double maxDown = (vehicleSize < 0.8f) ? -7.2 : -6.4;
            yVel = !upsideDown ? std::clamp(yVel, maxDown, maxUp) : std::clamp(yVel, -maxUp, -maxDown);
        } else if (isUfo) {
            if (justPressed) {
                yVel = ((vehicleSize < 0.8f) ? 7.2 : 7.8) * gravDir;
                onGround = false;
            } else {
                yVel -= 0.68 * gravDir * gravityMod * (stepScale * 0.5f);
            }
            yVel = std::clamp(yVel, -10.5, 10.5);
        } else if (isSwing) {
            if (justPressed) upsideDown = !upsideDown;
            double swingDir = upsideDown ? -1.0 : 1.0;
            yVel -= 0.72 * swingDir * gravityMod * (stepScale * 0.5f);
            yVel = std::clamp(yVel, -9.5, 9.5);
        } else if (isBall) {
            if (justPressed && onGround) {
                upsideDown = !upsideDown;
                onGround = false;
                yVel = -1.5 * (upsideDown ? -1.0 : 1.0);
            }
            double ballGrav = upsideDown ? -0.62 : 0.62;
            yVel -= ballGrav * gravityMod * (stepScale * 0.5f);
            yVel = std::clamp(yVel, -12.0, 12.0);
        } else {
            if ((justPressed || holding) && onGround) {
                double jumpBoost = 11.180032 * miniScale;
                yVel = jumpBoost * gravDir;
                onGround = false;
            }
            yVel -= (gravity * 0.90) * gravDir * gravityMod * (stepScale * 0.5f);
            yVel = std::clamp(yVel, -15.0, 15.0);
        }

        pos.y += static_cast<float>(yVel * stepScale * 0.9f);

        if (!upsideDown && pos.y <= 105.0f) {
            pos.y = 105.0f;
            if (yVel < 0.0) yVel = 0.0;
            onGround = true;
        }
    }
};

static int checkTrajectoryCollision(PlayLayer* pl, const CCPoint& pt, float playerSize, ExactPlayerSim& sim) {
    if (!pl || !pl->m_objects) return 0;

    const float half = 14.0f * playerSize;
    CCRect playerRect(pt.x - half, pt.y - half, half * 2.0f, half * 2.0f);
    CCRect innerHazardRect(pt.x - half * 0.45f, pt.y - half * 0.45f, half * 0.9f, half * 0.9f);

    int count = pl->m_objects->count();
    for (int i = 0; i < count; i++) {
        auto obj = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
        if (!obj || !obj->isVisible()) continue;

        float ox = obj->getPositionX();
        if (std::abs(ox - pt.x) > 90.0f) continue;
        float oy = obj->getPositionY();
        if (std::abs(oy - pt.y) > 90.0f) continue;

        CCRect objRect = obj->getObjectRect();
        if (objRect.size.width <= 0.1f || objRect.size.height <= 0.1f) continue;

        auto type = obj->m_objectType;
        if (type == GameObjectType::Hazard || type == GameObjectType::AnimatedHazard) {
            if (innerHazardRect.intersectsRect(objRect)) return -1;
        } else if (type == GameObjectType::Solid) {
            if (playerRect.intersectsRect(objRect)) {
                float topY = objRect.getMaxY();
                float botY = objRect.getMinY();
                if (!sim.upsideDown && sim.pos.y >= topY - 8.0f && sim.yVel <= 0.0) {
                    sim.pos.y = topY + half;
                    sim.yVel = 0.0;
                    sim.onGround = true;
                    return 1;
                } else if (sim.upsideDown && sim.pos.y <= botY + 8.0f && sim.yVel >= 0.0) {
                    sim.pos.y = botY - half;
                    sim.yVel = 0.0;
                    sim.onGround = true;
                    return 1;
                } else if (std::abs(pt.y - oy) < half * 0.85f) {
                    return -1;
                }
            }
        }
    }
    return 0;
}

// ===== Cập nhật HUD hiển thị =====
static void refreshHudText() {
    if (!g_afc.hudRoot) return;
    g_afc.hudRoot->setVisible(g_afc.enabled);
    if (g_afc.worldDrawer) g_afc.worldDrawer->setVisible(g_afc.enabled);
    if (!g_afc.enabled) return;

    // 1. Bảng thống kê 1F..10F+ bên trái (Không có chữ NanCounter)
    if (g_afc.tallyLabel) {
        std::string s;
        for (int w = 1; w <= MAX_BUCKET; w++) {
            if (!s.empty()) s += "\n";
            s += fmt::format("{}F: {}", w, g_afc.tally[w]);
        }
        s += fmt::format("\n{}+F: {}", MAX_BUCKET + 1, g_afc.tally[MAX_BUCKET + 1]);
        g_afc.tallyLabel->setString(s.c_str());
    }

    // 2. Dòng trạng thái phía dưới
    if (g_afc.statusLabel) {
        std::string modeStr;
        if (g_afc.stage == BotStage::Record) {
            modeStr = fmt::format(
                "[RECORD] Frame: {} | Actions: {} (Press/Release)",
                g_afc.currentFrame,
                g_afc.actions.size()
            );
        } else if (g_afc.stage == BotStage::RollingScan) {
            int pct = g_afc.actions.empty()
                ? 0
                : static_cast<int>((g_afc.rollingActionIdx * 100) / g_afc.actions.size());
            modeStr = fmt::format(
                "[ROLLING SCAN -> JSON] {}/{} ({}%) | Frame: {}",
                g_afc.rollingActionIdx,
                g_afc.actions.size(),
                pct,
                g_afc.rollingMainFrame
            );
        } else {
            modeStr = fmt::format(
                "[REPLAY JSON] Frame: {} | Actions: {}",
                g_afc.currentFrame,
                g_afc.actions.size()
            );
        }
        g_afc.statusLabel->setString(modeStr.c_str());
    }

    // 3. Hiển thị thông tin chi tiết Frame Window của cú Press / Release vừa chạy qua
    if (g_afc.popupLabel) {
        if (g_afc.popupTimer > 0 && !g_afc.lastPopupText.empty()) {
            g_afc.popupLabel->setString(g_afc.lastPopupText.c_str());
            g_afc.popupLabel->setVisible(true);
        } else {
            g_afc.popupLabel->setVisible(false);
        }
    }
}

static void renderWorldTrajectories(PlayLayer* pl) {
    if (!g_afc.worldDrawer || !pl || !pl->m_player1) return;
    g_afc.worldDrawer->clear();
    if (!g_afc.enabled) return;

    // 1. Vẽ White Trajectory Arc đi theo nhân vật
    if (g_afc.showWhiteTrail && g_afc.whiteTrail.size() >= 2) {
        ccColor4F whiteCol = {1.0f, 1.0f, 1.0f, 0.85f};
        for (size_t i = 0; i + 1 < g_afc.whiteTrail.size(); i++) {
            g_afc.worldDrawer->drawSegment(g_afc.whiteTrail[i], g_afc.whiteTrail[i + 1], 1.6f, whiteCol);
        }
    }

    // 2. Vẽ các nhánh Trajectory Backward/Forward khi đang Rolling Scan
    for (const auto& branch : g_afc.activeBranches) {
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
            g_afc.worldDrawer->drawSegment(branch.points[i], branch.points[i + 1], width, col);
        }
        g_afc.worldDrawer->drawDot(branch.points.back(), branch.survived ? 2.4f : 3.2f, col);
    }

    // 3. Vẽ Ahead Trajectory Preview phía trước nhân vật
    if (g_afc.showAheadTraj && !pl->m_player1->m_isDead) {
        float dt = 1.0f / static_cast<float>(g_afc.targetFps);
        for (int branchType = 0; branchType < 2; branchType++) {
            bool holdBranch = (branchType == 0);
            ExactPlayerSim sim = ExactPlayerSim::fromPlayer(pl->m_player1);
            CCPoint prevPt = sim.pos;
            ccColor4F previewCol = holdBranch
                ? ccColor4F{0.20f, 0.88f, 1.0f, 0.75f}
                : ccColor4F{1.0f, 0.55f, 0.20f, 0.65f};

            for (int step = 0; step < PREVIEW_STEPS; step++) {
                sim.step(dt, holdBranch, step == 0 && holdBranch);
                int hit = checkTrajectoryCollision(pl, sim.pos, sim.vehicleSize, sim);

                g_afc.worldDrawer->drawSegment(prevPt, sim.pos, 1.3f, previewCol);
                prevPt = sim.pos;

                if (hit == -1) {
                    g_afc.worldDrawer->drawDot(sim.pos, 3.6f, {1.0f, 0.1f, 0.1f, 0.95f});
                    break;
                }
            }
        }
    }
}

class $modify(AFCPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g_afc = AFCState{};
        syncSettingsFromMod();

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        // Tự động kiểm tra file JSON khi mở màn chơi
        bool hasJsonMacro = loadMacroFromJson(level);
        std::string modeSetting = Mod::get()->getSettingValue<std::string>("work-mode");

        if (modeSetting == "Record Macro (Practice)") {
            g_afc.stage = BotStage::Record;
        } else if (modeSetting == "Replay Saved Data Only") {
            g_afc.stage = BotStage::Replay;
        } else {
            // Auto: Nếu đã có file JSON đã quét xong -> chạy Replay ngay lập tức!
            // Nếu có Macro trong JSON nhưng chưa quét xong -> chạy Quét cuốn chiếu (Rolling Scan)
            // Nếu chưa có file JSON -> vào chế độ Record
            if (hasJsonMacro && g_afc.analysisCompleted) {
                g_afc.stage = BotStage::Replay;
            } else if (hasJsonMacro && !g_afc.analysisCompleted) {
                g_afc.stage = BotStage::RollingScan;
            } else {
                g_afc.stage = BotStage::Record;
            }
        }

        auto ws = CCDirector::get()->getWinSize();

        g_afc.worldDrawer = CCDrawNode::create();
        if (m_objectLayer) {
            m_objectLayer->addChild(g_afc.worldDrawer, 1500);
        }

        g_afc.playerPopupLabel = CCLabelBMFont::create("", "bigFont.fnt");
        g_afc.playerPopupLabel->setScale(0.42f);
        g_afc.playerPopupLabel->setVisible(false);
        if (m_objectLayer) {
            m_objectLayer->addChild(g_afc.playerPopupLabel, 1600);
        }

        g_afc.hudRoot = CCNode::create();
        this->addChild(g_afc.hudRoot, 1000);

        g_afc.tallyLabel = CCLabelBMFont::create("", "bigFont.fnt");
        g_afc.tallyLabel->setScale(0.28f);
        g_afc.tallyLabel->setOpacity(215);
        g_afc.tallyLabel->setAnchorPoint({0.f, 1.f});
        g_afc.tallyLabel->setPosition({8.f, ws.height - 8.f});
        g_afc.hudRoot->addChild(g_afc.tallyLabel);

        g_afc.popupLabel = CCLabelBMFont::create("", "bigFont.fnt");
        g_afc.popupLabel->setScale(0.50f);
        g_afc.popupLabel->setOpacity(235);
        g_afc.popupLabel->setAnchorPoint({0.5f, 1.f});
        g_afc.popupLabel->setPosition({ws.width * 0.5f, ws.height - 10.f});
        g_afc.hudRoot->addChild(g_afc.popupLabel);

        g_afc.statusLabel = CCLabelBMFont::create("", "chatFont.fnt");
        g_afc.statusLabel->setScale(0.55f);
        g_afc.statusLabel->setOpacity(190);
        g_afc.statusLabel->setAnchorPoint({0.5f, 0.f});
        g_afc.statusLabel->setPosition({ws.width * 0.5f, 6.f});
        g_afc.hudRoot->addChild(g_afc.statusLabel);

        auto menu = CCMenu::create();
        menu->setPosition({ws.width - 88.f, 22.f});
        g_afc.hudRoot->addChild(menu, 10);

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
        g_afc.stage = BotStage::Record;
        g_afc.actions.clear();
        g_afc.practiceSnapshots.clear();
        g_afc.activeBranches.clear();
        g_afc.analysisCompleted = false;
        g_afc.rollingScanInitialized = false;
        std::fill(std::begin(g_afc.tally), std::end(g_afc.tally), 0);
        this->resetLevel();
        refreshHudText();
    }

    void onSwitchAnalyze(CCObject*) {
        if (g_afc.actions.empty()) {
            loadMacroFromJson(m_level);
        }
        if (g_afc.actions.empty()) {
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
        if (g_afc.actions.empty()) {
            loadMacroFromJson(m_level);
        }
        g_afc.stage = BotStage::Replay;
        g_afc.rollingScanInitialized = false;
        this->resetLevel();
        refreshHudText();
    }

    // Khởi tạo Quét cuốn chiếu (Rolling Scan) từ đầu level
    void startRollingScan() {
        g_afc.stage = BotStage::RollingScan;
        g_afc.rollingScanInitialized = false;
        g_afc.rollingActionIdx = 0;
        g_afc.rollingMainFrame = 0;
        g_afc.rollingMainMacroIdx = 0;
        g_afc.analysisCompleted = false;
        std::fill(std::begin(g_afc.tally), std::end(g_afc.tally), 0);
        for (auto& act : g_afc.actions) act.analyzed = false;
        refreshHudText();
    }

    // Khi tạo Checkpoint trong Practice Mode lúc Record, lưu Deep Snapshot để tránh desync Orb/Portal
    CheckpointObject* createCheckpoint() {
        auto ckpt = PlayLayer::createCheckpoint();
        if (ckpt && g_afc.enabled && !g_afc.isSimulating && g_afc.stage == BotStage::Record) {
            PlayLayerStateSnapshot snap;
            snap.valid           = true;
            snap.frame           = g_afc.currentFrame;
            snap.macroInputCount = g_afc.actions.size();
            snap.holdingP1       = g_afc.holdingP1;
            snap.holdingP2       = g_afc.holdingP2;
            snap.p1Snap          = PlayerExactSnapshot::capture(m_player1);
            snap.p2Snap          = PlayerExactSnapshot::capture(m_player2);

            if (m_objects && m_player1) {
                float px = m_player1->getPositionX();
                int count = m_objects->count();
                for (int i = 0; i < count; i++) {
                    auto obj = static_cast<GameObject*>(m_objects->objectAtIndex(i));
                    if (!obj) continue;
                    if (obj->getPositionX() >= px - 800.f && obj->getPositionX() <= px + 2500.f) {
                        snap.objectsSnap.push_back(ObjectStateEntry::capture(obj));
                    }
                }
            }
            g_afc.practiceSnapshots[ckpt] = std::move(snap);
        }
        return ckpt;
    }

    void resetLevel() {
        this->handleButton(false, 1, true);
        PlayLayer::resetLevel();

        if (!g_afc.isSimulating) {
            g_afc.whiteTrail.clear();
        }

        if (g_afc.enabled && g_afc.stage == BotStage::Record && !g_afc.isSimulating) {
            if (m_isPracticeMode && m_checkpointArray && m_checkpointArray->count() > 0) {
                auto lastCkpt = static_cast<CheckpointObject*>(m_checkpointArray->lastObject());
                auto it = g_afc.practiceSnapshots.find(lastCkpt);
                if (it != g_afc.practiceSnapshots.end()) {
                    const auto& snap = it->second;
                    g_afc.currentFrame = snap.frame;
                    if (g_afc.actions.size() > snap.macroInputCount) {
                        g_afc.actions.resize(snap.macroInputCount);
                    }
                    // Khôi phục toàn bộ trạng thái Orb, Portal, Trigger & Player tại Checkpoint
                    for (const auto& objEntry : snap.objectsSnap) {
                        objEntry.restore();
                    }
                    snap.p1Snap.restore(m_player1);
                    snap.p2Snap.restore(m_player2);
                    g_afc.holdingP1 = snap.holdingP1;
                    g_afc.holdingP2 = snap.holdingP2;
                    if (g_afc.holdingP1) {
                        this->handleButton(true, 1, true);
                    }
                    refreshHudText();
                    return;
                }
            }
            g_afc.currentFrame = 0;
            g_afc.actions.clear();
            g_afc.holdingP1 = false;
            g_afc.holdingP2 = false;
            refreshHudText();
        } else if (!g_afc.isSimulating) {
            g_afc.currentFrame = 0;
            g_afc.playbackIndex = 0;
            g_afc.holdingP1 = false;
            g_afc.holdingP2 = false;
        }
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        if (!g_afc.enabled || g_afc.isSimulating) return;

        // Hoàn thành 100% trong chế độ Record -> Lưu file JSON thô rồi tự động chạy Rolling Scan
        if (g_afc.stage == BotStage::Record && !g_afc.actions.empty()) {
            g_afc.analysisCompleted = false;
            saveMacroToJson(m_level);
            startRollingScan();
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (object == m_anticheatSpike) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }
        PlayLayer::destroyPlayer(player, object);

        if (g_afc.isSimulating && player && player->m_isDead) {
            g_afc.simDead = true;
        }
    }

    // ========================================================================
    // CƠ CHẾ QUÉT CUỐN CHIẾU (ROLLING SCAN) VỚI SAVE / LOAD STATE TẠI CHỖ
    // ========================================================================

    // Chạy thử 1 nhánh độ lệch `delta` bắt đầu trực tiếp từ `rollingMainSnapshot` (không cần chạy lại từ 0%)
    TrajectoryBranch testLocalBranchFromSnapshot(size_t actionIdx, int delta) {
        TrajectoryBranch branch;
        branch.delta = delta;
        branch.survived = false;

        const auto& targetAct = g_afc.actions[actionIdx];
        int origFrame = targetAct.frame;
        int shiftedFrame = origFrame + delta;

        // Kiểm tra ràng buộc thứ tự nhân quả giữa Type 1 (Press) và Type 2 (Release)
        if (shiftedFrame < g_afc.rollingMainSnapshot.frame) {
            return branch;
        }
        if (actionIdx > 0 && shiftedFrame <= g_afc.actions[actionIdx - 1].frame) {
            return branch;
        }
        if (targetAct.type == ActionType::Release && actionIdx + 1 < g_afc.actions.size()) {
            if (shiftedFrame >= g_afc.actions[actionIdx + 1].frame) {
                return branch;
            }
        }

        // Nếu thao tác là Type 1: Press và bị dịch tiến chạm vào Type 2: Release kế tiếp của nó,
        // dịch lùi nhẹ Release kế tiếp để bảo toàn thời lượng giữ phím tối thiểu
        int nextReleaseShiftedFrame = -1;
        if (targetAct.type == ActionType::Press && actionIdx + 1 < g_afc.actions.size() &&
            g_afc.actions[actionIdx + 1].type == ActionType::Release) {
            int origRelFrame = g_afc.actions[actionIdx + 1].frame;
            int holdLen = std::max(1, origRelFrame - origFrame);
            if (shiftedFrame >= origRelFrame) {
                nextReleaseShiftedFrame = shiftedFrame + holdLen;
                if (actionIdx + 2 < g_afc.actions.size() &&
                    nextReleaseShiftedFrame >= g_afc.actions[actionIdx + 2].frame) {
                    nextReleaseShiftedFrame = g_afc.actions[actionIdx + 2].frame - 1;
                }
                if (nextReleaseShiftedFrame <= shiftedFrame) {
                    return branch;
                }
            }
        }

        // Khôi phục chính xác trạng thái gốc trước thao tác (Orb, Portal, Trigger, Vị trí, Vận tốc)
        g_afc.rollingMainSnapshot.restore(this);

        int simFrame = g_afc.rollingMainSnapshot.frame;
        size_t mIdx = g_afc.rollingMainSnapshot.macroInputCount;
        int targetEndFrame = origFrame + SURVIVE_CHECK_FRAMES;
        if (actionIdx + 1 < g_afc.actions.size()) {
            targetEndFrame = std::max(targetEndFrame, g_afc.actions[actionIdx + 1].frame + 35);
        }

        float dt = 1.0f / static_cast<float>(g_afc.targetFps);
        g_afc.simDead = false;
        branch.survived = true;

        while (simFrame <= targetEndFrame) {
            // Thực thi các thao tác Macro trong phạm vi nhánh thử nghiệm
            while (mIdx < g_afc.actions.size()) {
                int effectiveFrame = g_afc.actions[mIdx].frame;
                if (mIdx == actionIdx) {
                    effectiveFrame = shiftedFrame;
                } else if (mIdx == actionIdx + 1 && nextReleaseShiftedFrame != -1) {
                    effectiveFrame = nextReleaseShiftedFrame;
                }

                if (effectiveFrame > simFrame) break;
                if (effectiveFrame == simFrame) {
                    bool isDown = (g_afc.actions[mIdx].type == ActionType::Press);
                    this->handleButton(isDown, g_afc.actions[mIdx].button, g_afc.actions[mIdx].player1);
                }
                mIdx++;
            }

            GJBaseGameLayer::processCommands(dt, false, true);

            if (m_player1) {
                branch.points.push_back(m_player1->getPosition());
            }

            if (g_afc.simDead || (m_player1 && m_player1->m_isDead)) {
                branch.survived = false;
                break;
            }
            if (m_hasCompletedLevel) {
                branch.survived = true;
                break;
            }

            simFrame++;
        }

        return branch;
    }

    // Bước quét cuốn chiếu cho Action thứ `actionIdx`
    void stepRollingScanForAction(size_t actionIdx) {
        if (actionIdx >= g_afc.actions.size()) return;

        bool savedPractice = m_isPracticeMode;
        m_isPracticeMode = false;
        g_afc.isSimulating = true;

        float dt = 1.0f / static_cast<float>(g_afc.targetFps);

        // Khởi tạo Main Timeline ở frame 0 nếu mới bắt đầu quét cuốn chiếu
        if (!g_afc.rollingScanInitialized) {
            this->resetLevel();
            g_afc.rollingMainFrame = 0;
            g_afc.rollingMainMacroIdx = 0;
            g_afc.holdingP1 = false;
            g_afc.holdingP2 = false;
            g_afc.rollingScanInitialized = true;
        } else {
            // Khôi phục lại Main Timeline từ Snapshot trước đó để cuốn chiếu tiếp
            g_afc.rollingMainSnapshot.restore(this);
            g_afc.rollingMainFrame = g_afc.rollingMainSnapshot.frame;
            g_afc.rollingMainMacroIdx = g_afc.rollingMainSnapshot.macroInputCount;
            g_afc.holdingP1 = g_afc.rollingMainSnapshot.holdingP1;
            g_afc.holdingP2 = g_afc.rollingMainSnapshot.holdingP2;
        }

        // Xác định mốc frame đóng băng trạng thái ngay trước phạm vi quét Backward của Action hiện tại
        int actionFrame = g_afc.actions[actionIdx].frame;
        int desiredSnapshotFrame = std::max(0, actionFrame - MAX_BACKWARD_FRAMES - 2);
        if (actionIdx > 0) {
            desiredSnapshotFrame = std::max(desiredSnapshotFrame, g_afc.actions[actionIdx - 1].frame + 1);
        }
        desiredSnapshotFrame = std::min(desiredSnapshotFrame, actionFrame);

        // 1. Cuốn chiếu Main Timeline tiến tới `desiredSnapshotFrame` (tối đa tới ngay trước `actionIdx`)
        while (g_afc.rollingMainFrame < desiredSnapshotFrame) {
            while (g_afc.rollingMainMacroIdx < actionIdx &&
                   g_afc.actions[g_afc.rollingMainMacroIdx].frame == g_afc.rollingMainFrame) {
                const auto& mAct = g_afc.actions[g_afc.rollingMainMacroIdx];
                bool isDown = (mAct.type == ActionType::Press);
                if (mAct.player1) g_afc.holdingP1 = isDown;
                else g_afc.holdingP2 = isDown;
                this->handleButton(isDown, mAct.button, mAct.player1);
                g_afc.rollingMainMacroIdx++;
            }

            GJBaseGameLayer::processCommands(dt, false, true);
            g_afc.rollingMainFrame++;
        }

        // Đảm bảo mọi thao tác trước `actionIdx` có cùng frame đã được thực thi trên Main Timeline
        while (g_afc.rollingMainMacroIdx < actionIdx &&
               g_afc.actions[g_afc.rollingMainMacroIdx].frame <= g_afc.rollingMainFrame) {
            const auto& mAct = g_afc.actions[g_afc.rollingMainMacroIdx];
            bool isDown = (mAct.type == ActionType::Press);
            if (mAct.player1) g_afc.holdingP1 = isDown;
            else g_afc.holdingP2 = isDown;
            this->handleButton(isDown, mAct.button, mAct.player1);
            g_afc.rollingMainMacroIdx++;
        }

        // 2. SAVE STATE: Đóng băng toàn bộ PlayLayer (Player, Velocity, Orb, Portal, Trigger) tại `rollingMainFrame`
        g_afc.rollingMainSnapshot = PlayLayerStateSnapshot::capture(
            this,
            g_afc.rollingMainFrame,
            g_afc.rollingMainMacroIdx,
            g_afc.holdingP1,
            g_afc.holdingP2
        );

        g_afc.activeBranches.clear();

        // 3. Kiểm tra nhánh gốc (delta = 0)
        g_afc.activeBranches.push_back(testLocalBranchFromSnapshot(actionIdx, 0));

        // 4. Quét lùi (Backward Frame Check: delta = -1, -2, ..., -MAX_BACKWARD_FRAMES)
        int backwardValid = 0;
        for (int b = 1; b <= MAX_BACKWARD_FRAMES; b++) {
            auto br = testLocalBranchFromSnapshot(actionIdx, -b);
            bool ok = br.survived;
            g_afc.activeBranches.push_back(std::move(br));
            if (ok) {
                backwardValid++;
            } else {
                break;
            }
        }

        // 5. Quét tiến (Forward Frame Check: delta = +1, +2, ..., +MAX_FORWARD_FRAMES)
        int forwardValid = 0;
        for (int f = 1; f <= MAX_FORWARD_FRAMES; f++) {
            auto br = testLocalBranchFromSnapshot(actionIdx, +f);
            bool ok = br.survived;
            g_afc.activeBranches.push_back(std::move(br));
            if (ok) {
                forwardValid++;
            } else {
                break;
            }
        }

        // 6. LOAD STATE: Khôi phục lại Main Timeline về đúng trạng thái đóng băng để chuẩn bị cho Action kế tiếp
        g_afc.rollingMainSnapshot.restore(this);

        // Công thức tính Frame Window: 1 (frame gốc) + số frame khả thi trước + số frame khả thi sau
        int totalWindow = 1 + backwardValid + forwardValid;
        auto& act = g_afc.actions[actionIdx];
        act.index          = static_cast<int>(actionIdx);
        act.analyzed       = true;
        act.backwardFrames = backwardValid;
        act.forwardFrames  = forwardValid;
        act.windowSize     = totalWindow;

        int bucket = (totalWindow > MAX_BUCKET) ? (MAX_BUCKET + 1) : totalWindow;
        g_afc.tally[bucket]++;

        g_afc.isSimulating = false;
        m_isPracticeMode = savedPractice;
    }

    void triggerActionFeedback(const MacroAction& act) {
        const char* shortType = (act.type == ActionType::Press) ? "Type 1: PRESS" : "Type 2: RELEASE";
        g_afc.lastPopupText = fmt::format(
            "{}F [{} | 1 + {}B + {}A]",
            act.windowSize,
            shortType,
            act.backwardFrames,
            act.forwardFrames
        );
        g_afc.popupTimer = 95;

        if (g_afc.playerPopupLabel && m_player1) {
            g_afc.playerPopupLabel->setString(fmt::format("{}F", act.windowSize).c_str());
            g_afc.playerPopupLabel->setPosition({
                m_player1->getPositionX(),
                m_player1->getPositionY() + 28.f
            });
            g_afc.playerPopupLabel->setVisible(true);
        }

        if (Mod::get()->getSettingValue<bool>("enable-sound")) {
            FMODAudioEngine::sharedEngine()->playEffect("crystal01.ogg");
        }
    }

    void pollCustomShortcutKeys() {
#ifdef GEODE_IS_WINDOWS
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
            g_afc.enabled = !g_afc.enabled;
            Mod::get()->setSettingValue<bool>("enable-mod", g_afc.enabled);
            refreshHudText();
        }
        if (g_afc.enabled) {
            if (curMode && !prevMode) {
                if (g_afc.stage == BotStage::Record) {
                    saveMacroToJson(m_level);
                    this->onSwitchAnalyze(nullptr);
                } else if (g_afc.stage == BotStage::RollingScan) {
                    this->onSwitchReplay(nullptr);
                } else {
                    this->onSwitchRecord(nullptr);
                }
            }
            if (curTraj && !prevTraj) {
                g_afc.showAheadTraj = !g_afc.showAheadTraj;
                Mod::get()->setSettingValue<bool>("show-ahead-trajectory", g_afc.showAheadTraj);
            }
            if (curReset && !prevReset) {
                auto path = getLevelJsonPath(m_level);
                if (std::filesystem::exists(path)) {
                    std::filesystem::remove(path);
                }
                g_afc.actions.clear();
                g_afc.activeBranches.clear();
                std::fill(std::begin(g_afc.tally), std::end(g_afc.tally), 0);
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
        if (g_afc.isSimulating) return;

        pollCustomShortcutKeys();
        if (!g_afc.enabled) return;

        // Thực thi Quét cuốn chiếu (Rolling Scan) theo lô mỗi frame để vừa cực nhanh vừa hiển thị mượt
        if (g_afc.stage == BotStage::RollingScan) {
            if (g_afc.rollingActionIdx < g_afc.actions.size()) {
                for (int step = 0; step < ACTIONS_PER_UI_TICK && g_afc.rollingActionIdx < g_afc.actions.size(); step++) {
                    stepRollingScanForAction(g_afc.rollingActionIdx);
                    g_afc.rollingActionIdx++;
                }
                renderWorldTrajectories(this);
                refreshHudText();

                // Ngay khi quét xong toàn bộ level -> Xuất ra file JSON và chuyển sang Replay
                if (g_afc.rollingActionIdx >= g_afc.actions.size()) {
                    g_afc.analysisCompleted = true;
                    g_afc.rollingMainSnapshot = PlayLayerStateSnapshot{};
                    g_afc.activeBranches.clear();
                    saveMacroToJson(m_level);
                    g_afc.stage = BotStage::Replay;
                    this->resetLevel();
                }
                return;
            } else {
                g_afc.stage = BotStage::Replay;
                this->resetLevel();
            }
        }

        if (g_afc.popupTimer > 0) {
            g_afc.popupTimer--;
            if (g_afc.popupTimer == 0 && g_afc.playerPopupLabel) {
                g_afc.playerPopupLabel->setVisible(false);
            }
        }

        renderWorldTrajectories(this);
        refreshHudText();
    }
};

class $modify(AFCGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        if (pl && g_afc.enabled && !g_afc.isSimulating && g_afc.stage == BotStage::Record) {
            // Chống ghi trùng trạng thái nếu phím đang được giữ sẵn
            bool alreadyHolding = isPlayer1 ? g_afc.holdingP1 : g_afc.holdingP2;
            if (down != alreadyHolding) {
                if (isPlayer1) g_afc.holdingP1 = down;
                else g_afc.holdingP2 = down;

                CCPoint pos = pl->m_player1 ? pl->m_player1->getPosition() : CCPoint{0.f, 0.f};
                MacroAction act;
                act.index          = static_cast<int>(g_afc.actions.size());
                act.frame          = g_afc.currentFrame;
                act.type           = down ? ActionType::Press : ActionType::Release; // Type 1 vs Type 2
                act.button         = button;
                act.player1        = isPlayer1;
                act.x              = pos.x;
                act.y              = pos.y;
                act.analyzed       = false;
                act.backwardFrames = 0;
                act.forwardFrames  = 0;
                act.windowSize     = 1;

                g_afc.actions.push_back(act);
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = static_cast<AFCPlayLayer*>(PlayLayer::get());
        if (pl && g_afc.enabled && !g_afc.isSimulating && g_afc.stage != BotStage::RollingScan) {
            // Phát lại dữ liệu từ file JSON trong chế độ Replay
            if (g_afc.stage == BotStage::Replay) {
                while (g_afc.playbackIndex < g_afc.actions.size() &&
                       g_afc.actions[g_afc.playbackIndex].frame == g_afc.currentFrame) {
                    const auto& act = g_afc.actions[g_afc.playbackIndex];
                    bool isDown = (act.type == ActionType::Press);
                    GJBaseGameLayer::handleButton(isDown, act.button, act.player1);
                    if (act.analyzed) {
                        pl->triggerActionFeedback(act);
                    }
                    g_afc.playbackIndex++;
                }
            }

            if (pl->m_player1 && !pl->m_player1->m_isDead) {
                g_afc.whiteTrail.push_back(pl->m_player1->getPosition());
                if (g_afc.whiteTrail.size() > WHITE_TRAIL_MAX_PTS) {
                    g_afc.whiteTrail.pop_front();
                }
            }

            g_afc.currentFrame++;
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};
