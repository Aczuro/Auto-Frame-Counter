#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CheckpointObject.hpp>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <deque>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;

// ===== Tham số cấu hình kiểm tra Trajectory & Frame Window =====
constexpr int MAX_BACKWARD_FRAMES = 12;  // Số frame tối đa lùi về trước cú click/release để kiểm tra
constexpr int MAX_FORWARD_FRAMES  = 12;  // Số frame tối đa tiến về sau cú click/release để kiểm tra
constexpr int MIN_SURVIVE_FRAMES  = 75;  // Số frame tối thiểu nhân vật phải sống sót sau cú thao tác
constexpr int PREVIEW_STEPS       = 110; // Độ dài đường dự đoán quỹ đạo phía trước (Ahead Trajectory)
constexpr int WHITE_TRAIL_MAX_PTS = 180; // Độ dài vệt quỹ đạo trắng đi theo nhân vật
constexpr int MAX_BUCKET          = 10;  // Thống kê từ 1F đến 10F và 11F+

enum class BotStage {
    Record,   // Ghi macro trong Practice Mode từ 0 -> 100%
    Analyze,  // Bot chạy lại + lùi/tiến frame kiểm tra bằng Trajectory
    Replay    // Phát lại Macro + hiển thị dữ liệu Frame Window đã phân tích
};

struct MacroInput {
    int frame = 0;
    bool down = true;
    int button = 1;
    bool player1 = true;
    float x = 0.f;
    float y = 0.f;

    // Kết quả phân tích Frame Window bằng Trajectory
    bool analyzed = false;
    int backwardFrames = 0;
    int forwardFrames = 0;
    int windowSize = 1;
};

struct TrajectoryBranch {
    int delta = 0;
    bool survived = false;
    std::vector<CCPoint> points;
};

struct CheckpointSnapshot {
    int frame = 0;
    size_t inputCount = 0;
    bool holdingP1 = false;
};

struct AFCState {
    bool enabled = true;
    bool showWhiteTrail = true;
    bool showAheadTraj = true;
    int targetFps = 240;

    BotStage stage = BotStage::Analyze;
    bool isSimulating = false;
    bool simDead = false;

    int currentFrame = 0;
    size_t playbackIndex = 0;
    bool holdingP1 = false;
    bool analysisCompleted = false;

    // Dữ liệu Macro và Checkpoint
    std::vector<MacroInput> inputs;
    std::unordered_map<CheckpointObject*, CheckpointSnapshot> checkpoints;

    // Dữ liệu tiến trình Analyze (quét từng thao tác một để không gây đứng hình)
    size_t analyzeActionIdx = 0;
    std::vector<TrajectoryBranch> activeBranches;

    // Thống kê và hiển thị
    int tally[MAX_BUCKET + 2] = {};
    std::deque<CCPoint> whiteTrail;
    std::string lastPopupText;
    int popupTimer = 0;

    // Các Node đồ họa trên PlayLayer
    CCNode* hudRoot = nullptr;
    CCDrawNode* worldDrawer = nullptr;
    CCLabelBMFont* tallyLabel = nullptr;
    CCLabelBMFont* statusLabel = nullptr;
    CCLabelBMFont* popupLabel = nullptr;
    CCLabelBMFont* playerPopupLabel = nullptr;
};

inline AFCState g_afc;

// ===== Hệ thống lưu / tải dữ liệu Macro + Frame Window theo từng màn chơi =====
static std::filesystem::path getLevelDataPath(GJGameLevel* level) {
    int id = level ? level->m_levelID.value() : 0;
    std::string name = (id > 0) ? fmt::format("level_{}.afc", id)
                                : fmt::format("editor_{}.afc", level ? std::string(level->m_levelName) : "unknown");
    return Mod::get()->getSaveDir() / name;
}

static void recomputeTally() {
    std::fill(std::begin(g_afc.tally), std::end(g_afc.tally), 0);
    for (const auto& inp : g_afc.inputs) {
        if (inp.analyzed && inp.windowSize > 0) {
            int b = (inp.windowSize > MAX_BUCKET) ? (MAX_BUCKET + 1) : inp.windowSize;
            g_afc.tally[b]++;
        }
    }
}

static void saveMacroAndAnalysis(GJGameLevel* level) {
    if (!level) return;
    auto path = getLevelDataPath(level);
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return;

    out << (g_afc.analysisCompleted ? 1 : 0) << " " << g_afc.inputs.size() << "\n";
    for (const auto& item : g_afc.inputs) {
        out << item.frame << " "
            << (item.down ? 1 : 0) << " "
            << item.button << " "
            << (item.player1 ? 1 : 0) << " "
            << item.x << " "
            << item.y << " "
            << (item.analyzed ? 1 : 0) << " "
            << item.backwardFrames << " "
            << item.forwardFrames << " "
            << item.windowSize << "\n";
    }
}

static bool loadMacroAndAnalysis(GJGameLevel* level) {
    g_afc.inputs.clear();
    g_afc.analysisCompleted = false;
    std::fill(std::begin(g_afc.tally), std::end(g_afc.tally), 0);

    if (!level) return false;
    auto path = getLevelDataPath(level);
    if (!std::filesystem::exists(path)) return false;

    std::ifstream in(path);
    if (!in.is_open()) return false;

    int completedFlag = 0;
    size_t count = 0;
    if (!(in >> completedFlag >> count)) return false;

    g_afc.analysisCompleted = (completedFlag != 0);
    g_afc.inputs.reserve(count);

    for (size_t i = 0; i < count; i++) {
        MacroInput item;
        int downInt = 1, p1Int = 1, analyzedInt = 0;
        if (!(in >> item.frame >> downInt >> item.button >> p1Int
                 >> item.x >> item.y >> analyzedInt
                 >> item.backwardFrames >> item.forwardFrames >> item.windowSize)) {
            break;
        }
        item.down = (downInt != 0);
        item.player1 = (p1Int != 0);
        item.analyzed = (analyzedInt != 0);
        g_afc.inputs.push_back(item);
    }
    recomputeTally();
    return !g_afc.inputs.empty();
}

// ===== Đọc cấu hình từ Geode Settings =====
static void syncSettingsFromMod() {
    g_afc.enabled       = Mod::get()->getSettingValue<bool>("enable-mod");
    g_afc.showWhiteTrail = Mod::get()->getSettingValue<bool>("show-white-trail");
    g_afc.showAheadTraj  = Mod::get()->getSettingValue<bool>("show-ahead-trajectory");
    g_afc.targetFps      = std::clamp(static_cast<int>(Mod::get()->getSettingValue<int64_t>("target-fps")), 60, 1000);
}

// ===== Hỗ trợ đọc phím tắt tùy chỉnh trên PC =====
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

// ===== Công thức vật lý chuẩn xác của Geometry Dash 2.2 cho Ahead Trajectory =====
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

    // Hằng số vận tốc ngang chuẩn của Geometry Dash (đơn vị/giây) theo từng mốc tốc độ
    float getHorizontalUnitsPerSec() const {
        if (speed <= 0.75f) return 251.16008f; // 0.5x speed
        if (speed <= 0.95f) return 311.58010f; // 1.0x speed
        if (speed <= 1.15f) return 387.42014f; // 2.0x speed
        if (speed <= 1.35f) return 468.00015f; // 3.0x speed
        return 576.00018f;                     // 4.0x speed
    }

    // Bước tích phân vật lý khớp với PlayerObject::update & PlayerObject::updateJump của GD 2.2
    void step(float dt, bool holding, bool justPressed) {
        const float stepScale = dt * 60.0f; // Chuẩn hóa nội bộ của GD (60Hz base factor)
        const float gravDir   = upsideDown ? -1.0f : 1.0f;
        const float miniScale = (vehicleSize < 0.8f) ? 0.8f : 1.0f;

        // 1. Dịch chuyển trục X
        float dx = getHorizontalUnitsPerSec() * dt;
        pos.x += dx;

        // 2. Tính vận tốc trục Y theo đúng từng Gamemode của Geometry Dash
        if (isWave) {
            float waveSlope = (vehicleSize < 0.8f) ? 2.0f : 1.0f;
            float dir = holding ? 1.0f : -1.0f;
            if (upsideDown) dir = -dir;
            yVel = (dx / stepScale) * dir * waveSlope;
            pos.y += static_cast<float>(yVel * stepScale);
            return;
        }

        if (isShip) {
            // Công thức gia tốc của Ship trong GD 2.2
            double accel = holding ? 1.35 : -1.08;
            if (vehicleSize < 0.8f) accel *= 1.18;
            yVel += accel * gravDir * gravityMod * (stepScale * 0.5f);
            double maxUp = (vehicleSize < 0.8f) ? 8.5 : 8.0;
            double maxDown = (vehicleSize < 0.8f) ? -7.2 : -6.4;
            if (!upsideDown) {
                yVel = std::clamp(yVel, maxDown, maxUp);
            } else {
                yVel = std::clamp(yVel, -maxUp, -maxDown);
            }
        } else if (isUfo) {
            if (justPressed) {
                yVel = ((vehicleSize < 0.8f) ? 7.2 : 7.8) * gravDir;
                onGround = false;
            } else {
                yVel -= 0.68 * gravDir * gravityMod * (stepScale * 0.5f);
            }
            yVel = std::clamp(yVel, -10.5, 10.5);
        } else if (isSwing) {
            if (justPressed) {
                upsideDown = !upsideDown;
            }
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
            // Cube / Robot
            if ((justPressed || holding) && onGround) {
                double jumpBoost = 11.180032 * miniScale;
                yVel = jumpBoost * gravDir;
                onGround = false;
            }
            yVel -= (gravity * 0.90) * gravDir * gravityMod * (stepScale * 0.5f);
            yVel = std::clamp(yVel, -15.0, 15.0);
        }

        // Trong GD, độ dịch chuyển Y mỗi step bằng yVel * stepScale * 0.9f
        pos.y += static_cast<float>(yVel * stepScale * 0.9f);

        // Chạm mặt sàn cơ bản (y = 105 là mặt đất mặc định của GD)
        if (!upsideDown && pos.y <= 105.0f) {
            pos.y = 105.0f;
            if (yVel < 0.0) yVel = 0.0;
            onGround = true;
        }
    }
};

// Kiểm tra va chạm nhanh phục vụ vẽ đường dự đoán Ahead Trajectory
static int checkTrajectoryCollision(PlayLayer* pl, const CCPoint& pt, float playerSize, ExactPlayerSim& sim) {
    // Trả về: 0 = bình thường, 1 = chạm đất/khối block, -1 = chạm gai/vật cản chết
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
            if (innerHazardRect.intersectsRect(objRect)) {
                return -1; // Chạm hitbox gai chết
            }
        } else if (type == GameObjectType::Solid) {
            if (playerRect.intersectsRect(objRect)) {
                // Nếu đáp lên mặt trên của block (trọng lực thường) hoặc mặt dưới (trọng lực ngược)
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
                    return -1; // Đâm trực diện vào cạnh bên của khối Solid
                }
            }
        }
    }
    return 0;
}

// ===== Cập nhật giao diện HUD (Đã xóa hoàn toàn chữ NanCounter) =====
static void refreshHudText() {
    if (!g_afc.hudRoot) return;
    g_afc.hudRoot->setVisible(g_afc.enabled);
    if (g_afc.worldDrawer) g_afc.worldDrawer->setVisible(g_afc.enabled);
    if (!g_afc.enabled) return;

    // 1. Bảng thống kê Frame Window bên trái (Không có dòng chữ NanCounter)
    if (g_afc.tallyLabel) {
        std::string s;
        for (int w = 1; w <= MAX_BUCKET; w++) {
            if (!s.empty()) s += "\n";
            s += fmt::format("{}F: {}", w, g_afc.tally[w]);
        }
        s += fmt::format("\n{}+F: {}", MAX_BUCKET + 1, g_afc.tally[MAX_BUCKET + 1]);
        g_afc.tallyLabel->setString(s.c_str());
    }

    // 2. Dòng trạng thái nhỏ gọn
    if (g_afc.statusLabel) {
        std::string modeStr;
        if (g_afc.stage == BotStage::Record) {
            modeStr = fmt::format("[RECORD] Frame: {} | Actions: {}", g_afc.currentFrame, g_afc.inputs.size());
        } else if (g_afc.stage == BotStage::Analyze) {
            modeStr = fmt::format(
                "[ANALYZE] {}/{} | Frame: {}",
                g_afc.analyzeActionIdx,
                g_afc.inputs.size(),
                g_afc.currentFrame
            );
        } else {
            modeStr = fmt::format("[REPLAY] Frame: {} | Actions: {}", g_afc.currentFrame, g_afc.inputs.size());
        }
        g_afc.statusLabel->setString(modeStr.c_str());
    }

    // 3. Hiển thị số Frame Window vừa đo được
    if (g_afc.popupLabel) {
        if (g_afc.popupTimer > 0 && !g_afc.lastPopupText.empty()) {
            g_afc.popupLabel->setString(g_afc.lastPopupText.c_str());
            g_afc.popupLabel->setVisible(true);
        } else {
            g_afc.popupLabel->setVisible(false);
        }
    }
}

// ===== Vẽ White Trajectory Arc, Ahead Trajectory & Các nhánh Frame Window =====
static void renderWorldTrajectories(PlayLayer* pl) {
    if (!g_afc.worldDrawer || !pl || !pl->m_player1) return;
    g_afc.worldDrawer->clear();
    if (!g_afc.enabled) return;

    // 1. Vẽ đường quỹ đạo màu trắng đi theo nhân vật (White Trajectory Arc)
    if (g_afc.showWhiteTrail && g_afc.whiteTrail.size() >= 2) {
        ccColor4F whiteCol = {1.0f, 1.0f, 1.0f, 0.85f};
        for (size_t i = 0; i + 1 < g_afc.whiteTrail.size(); i++) {
            g_afc.worldDrawer->drawSegment(g_afc.whiteTrail[i], g_afc.whiteTrail[i + 1], 1.6f, whiteCol);
        }
    }

    // 2. Vẽ các nhánh Trajectory Backward/Forward của cú Click/Release vừa kiểm tra
    for (const auto& branch : g_afc.activeBranches) {
        if (branch.points.size() < 2) continue;
        ccColor4F col;
        float width = 1.2f;
        if (branch.delta == 0) {
            col = {1.0f, 0.92f, 0.15f, 0.95f}; // Vàng: Quỹ đạo frame gốc trong Macro
            width = 1.9f;
        } else if (branch.survived) {
            col = {0.15f, 1.0f, 0.35f, 0.70f}; // Xanh lá: Các frame trước/sau khả thi
        } else {
            col = {1.0f, 0.20f, 0.20f, 0.45f}; // Đỏ: Các frame trước/sau bị va chạm chết
        }

        for (size_t i = 0; i + 1 < branch.points.size(); i++) {
            g_afc.worldDrawer->drawSegment(branch.points[i], branch.points[i + 1], width, col);
        }
        g_afc.worldDrawer->drawDot(branch.points.back(), branch.survived ? 2.4f : 3.2f, col);
    }

    // 3. Vẽ đường dự đoán quỹ đạo bay phía trước nhân vật (Ahead Trajectory Preview)
    if (g_afc.showAheadTraj && !pl->m_player1->m_isDead) {
        float dt = 1.0f / static_cast<float>(g_afc.targetFps);

        // Vẽ 2 nhánh dự đoán: Nhánh 1 = Giữ chuột (Xanh lơ), Nhánh 2 = Thả chuột (Hồng cam)
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
                    // Vẽ dấu chấm đỏ tại điểm dự đoán va chạm chết
                    g_afc.worldDrawer->drawDot(sim.pos, 3.6f, {1.0f, 0.1f, 0.1f, 0.95f});
                    break;
                }
            }
        }
    }
}

// Tạo bản sao chuỗi Macro nhưng dịch chuyển thời điểm của thao tác `idx` đi `delta` frame (Backward < 0, Forward > 0)
static std::vector<MacroInput> makeShiftedMacro(size_t idx, int delta) {
    auto copy = g_afc.inputs;
    int origFrame = copy[idx].frame;
    int shiftedFrame = std::max(0, origFrame + delta);

    if (idx > 0 && shiftedFrame <= copy[idx - 1].frame) {
        shiftedFrame = copy[idx - 1].frame + 1;
    }
    int realDelta = shiftedFrame - origFrame;
    copy[idx].frame = shiftedFrame;

    // Nếu thao tác đang xét là Click (down) và dịch sang phải chạm vào Release (up) kế tiếp,
    // dịch luôn Release kế tiếp để giữ nguyên độ dài giữ chuột (hold duration)
    if (copy[idx].down && idx + 1 < copy.size() && !copy[idx + 1].down) {
        if (copy[idx + 1].frame <= shiftedFrame) {
            copy[idx + 1].frame = std::max(shiftedFrame + 1, copy[idx + 1].frame + realDelta);
        }
    }
    return copy;
}

class $modify(AFCPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g_afc = AFCState{};
        syncSettingsFromMod();

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        bool hasSavedData = loadMacroAndAnalysis(level);
        std::string modeSetting = Mod::get()->getSettingValue<std::string>("work-mode");

        if (modeSetting == "Record Macro (Practice)") {
            g_afc.stage = BotStage::Record;
        } else if (modeSetting == "Replay Saved Data Only") {
            g_afc.stage = BotStage::Replay;
        } else {
            // Auto (Analyze + Replay): Nếu đã có Macro mà chưa Analyze xong thì chạy Analyze,
            // nếu đã Analyze xong thì phát Replay, nếu chưa có dữ liệu thì vào chế độ Record
            if (!hasSavedData) {
                g_afc.stage = BotStage::Record;
            } else if (!g_afc.analysisCompleted) {
                g_afc.stage = BotStage::Analyze;
            } else {
                g_afc.stage = BotStage::Replay;
            }
        }

        auto ws = CCDirector::get()->getWinSize();

        // Lớp vẽ Trajectory gắn trực tiếp vào m_objectLayer để di chuyển khớp 100% với camera màn chơi
        g_afc.worldDrawer = CCDrawNode::create();
        if (m_objectLayer) {
            m_objectLayer->addChild(g_afc.worldDrawer, 1500);
        }

        // Nhãn số Frame bay theo nhân vật trong thế giới game
        g_afc.playerPopupLabel = CCLabelBMFont::create("", "bigFont.fnt");
        g_afc.playerPopupLabel->setScale(0.42f);
        g_afc.playerPopupLabel->setVisible(false);
        if (m_objectLayer) {
            m_objectLayer->addChild(g_afc.playerPopupLabel, 1600);
        }

        // Container HUD trên màn hình
        g_afc.hudRoot = CCNode::create();
        this->addChild(g_afc.hudRoot, 1000);

        // Bảng đếm 1F..10F+ ở góc trái trên (Không có chữ NanCounter)
        g_afc.tallyLabel = CCLabelBMFont::create("", "bigFont.fnt");
        g_afc.tallyLabel->setScale(0.28f);
        g_afc.tallyLabel->setOpacity(215);
        g_afc.tallyLabel->setAnchorPoint({0.f, 1.f});
        g_afc.tallyLabel->setPosition({8.f, ws.height - 8.f});
        g_afc.hudRoot->addChild(g_afc.tallyLabel);

        // Nhãn hiển thị cửa sổ Frame ở giữa phía trên
        g_afc.popupLabel = CCLabelBMFont::create("", "bigFont.fnt");
        g_afc.popupLabel->setScale(0.55f);
        g_afc.popupLabel->setOpacity(235);
        g_afc.popupLabel->setAnchorPoint({0.5f, 1.f});
        g_afc.popupLabel->setPosition({ws.width * 0.5f, ws.height - 10.f});
        g_afc.hudRoot->addChild(g_afc.popupLabel);

        // Nhãn trạng thái nhỏ ở cạnh dưới màn hình
        g_afc.statusLabel = CCLabelBMFont::create("", "chatFont.fnt");
        g_afc.statusLabel->setScale(0.55f);
        g_afc.statusLabel->setOpacity(190);
        g_afc.statusLabel->setAnchorPoint({0.5f, 0.f});
        g_afc.statusLabel->setPosition({ws.width * 0.5f, 6.f});
        g_afc.hudRoot->addChild(g_afc.statusLabel);

        // Cụm nút bấm chuyển nhanh chế độ ở góc dưới phải (hỗ trợ cả chuột lẫn màn hình cảm ứng)
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
        g_afc.inputs.clear();
        g_afc.checkpoints.clear();
        g_afc.activeBranches.clear();
        g_afc.analysisCompleted = false;
        std::fill(std::begin(g_afc.tally), std::end(g_afc.tally), 0);
        this->resetLevel();
        refreshHudText();
    }

    void onSwitchAnalyze(CCObject*) {
        if (g_afc.inputs.empty()) {
            loadMacroAndAnalysis(m_level);
        }
        if (g_afc.inputs.empty()) {
            FLAlertLayer::create(
                "Auto Frame Counter",
                "Chua co du lieu Macro! Hay bat Practice Mode va ghi Macro (REC) truoc.",
                "OK"
            )->show();
            return;
        }
        g_afc.stage = BotStage::Analyze;
        g_afc.analyzeActionIdx = 0;
        g_afc.analysisCompleted = false;
        std::fill(std::begin(g_afc.tally), std::end(g_afc.tally), 0);
        for (auto& inp : g_afc.inputs) inp.analyzed = false;
        this->resetLevel();
        refreshHudText();
    }

    void onSwitchReplay(CCObject*) {
        if (g_afc.inputs.empty()) {
            loadMacroAndAnalysis(m_level);
        }
        g_afc.stage = BotStage::Replay;
        this->resetLevel();
        refreshHudText();
    }

    // Lưu trạng thái Macro tại từng Checkpoint trong Practice Mode
    CheckpointObject* createCheckpoint() {
        auto ckpt = PlayLayer::createCheckpoint();
        if (ckpt && g_afc.enabled && g_afc.stage == BotStage::Record) {
            g_afc.checkpoints[ckpt] = CheckpointSnapshot{
                g_afc.currentFrame,
                g_afc.inputs.size(),
                g_afc.holdingP1
            };
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
                auto it = g_afc.checkpoints.find(lastCkpt);
                if (it != g_afc.checkpoints.end()) {
                    g_afc.currentFrame = it->second.frame;
                    if (g_afc.inputs.size() > it->second.inputCount) {
                        g_afc.inputs.resize(it->second.inputCount);
                    }
                    g_afc.holdingP1 = it->second.holdingP1;
                    if (g_afc.holdingP1) {
                        this->handleButton(true, 1, true);
                    }
                    refreshHudText();
                    return;
                }
            }
            // Nếu reset về 0% (không có checkpoint)
            g_afc.currentFrame = 0;
            g_afc.inputs.clear();
            g_afc.holdingP1 = false;
            refreshHudText();
        } else if (!g_afc.isSimulating) {
            g_afc.currentFrame = 0;
            g_afc.playbackIndex = 0;
            g_afc.holdingP1 = false;
        }
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        if (!g_afc.enabled || g_afc.isSimulating) return;

        // Khi người chơi hoàn thành 100% trong chế độ Record, tự động lưu Macro
        // và nếu đang để chế độ Auto thì chuyển sang Analyze
        if (g_afc.stage == BotStage::Record && !g_afc.inputs.empty()) {
            saveMacroAndAnalysis(m_level);
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

    // Chạy mô phỏng vật lý thực của game cho một độ lệch frame `delta` (Backward hoặc Forward)
    TrajectoryBranch simulateShiftedTrajectory(size_t actionIdx, int delta) {
        TrajectoryBranch branch;
        branch.delta = delta;
        branch.survived = true;

        auto shiftedMacro = makeShiftedMacro(actionIdx, delta);
        int baseFrame = g_afc.inputs[actionIdx].frame;

        // Xác định mốc frame cần sống sót vượt qua
        int targetEndFrame = baseFrame + MIN_SURVIVE_FRAMES;
        if (actionIdx + 1 < g_afc.inputs.size()) {
            targetEndFrame = std::max(targetEndFrame, g_afc.inputs[actionIdx + 1].frame + 35);
        }
        int recordStartFrame = std::max(0, baseFrame - MAX_BACKWARD_FRAMES - 4);

        bool savedPractice = m_isPracticeMode;
        m_isPracticeMode = false;

        g_afc.isSimulating = true;
        g_afc.simDead = false;
        this->resetLevel();

        g_afc.currentFrame = 0;
        size_t mIdx = 0;
        float dt = 1.0f / static_cast<float>(g_afc.targetFps);

        while (g_afc.currentFrame <= targetEndFrame) {
            while (mIdx < shiftedMacro.size() && shiftedMacro[mIdx].frame == g_afc.currentFrame) {
                this->handleButton(
                    shiftedMacro[mIdx].down,
                    shiftedMacro[mIdx].button,
                    shiftedMacro[mIdx].player1
                );
                mIdx++;
            }

            GJBaseGameLayer::processCommands(dt, false, true);

            if (g_afc.currentFrame >= recordStartFrame && m_player1) {
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

            g_afc.currentFrame++;
        }

        this->handleButton(false, 1, true);
        g_afc.isSimulating = false;
        m_isPracticeMode = savedPractice;

        return branch;
    }

    // Thực hiện Backward Check + Forward Check cho thao tác thứ `actionIdx`
    void analyzeSingleMacroAction(size_t actionIdx) {
        if (actionIdx >= g_afc.inputs.size()) return;

        g_afc.activeBranches.clear();
        int origFrame = g_afc.inputs[actionIdx].frame;
        int prevActionFrame = (actionIdx > 0) ? g_afc.inputs[actionIdx - 1].frame : -1;
        int nextActionFrame = (actionIdx + 1 < g_afc.inputs.size())
                              ? g_afc.inputs[actionIdx + 1].frame
                              : (origFrame + MAX_FORWARD_FRAMES + 10);

        // 1. Mô phỏng tại đúng frame gốc của Macro (delta = 0)
        g_afc.activeBranches.push_back(simulateShiftedTrajectory(actionIdx, 0));

        // 2. Backward Frame Check: Lùi từng frame về phía trước thời điểm bấm của Macro
        int backwardValid = 0;
        for (int b = 1; b <= MAX_BACKWARD_FRAMES; b++) {
            if (origFrame - b <= prevActionFrame) break;
            auto br = simulateShiftedTrajectory(actionIdx, -b);
            bool ok = br.survived;
            g_afc.activeBranches.push_back(std::move(br));
            if (ok) {
                backwardValid++;
            } else {
                break; // Dừng đếm khi chạm ranh giới frame chết phía trước
            }
        }

        // 3. Forward Frame Check: Tiến từng frame về phía sau thời điểm bấm của Macro
        int forwardValid = 0;
        for (int f = 1; f <= MAX_FORWARD_FRAMES; f++) {
            if (origFrame + f >= nextActionFrame && !g_afc.inputs[actionIdx].down) break;
            auto br = simulateShiftedTrajectory(actionIdx, +f);
            bool ok = br.survived;
            g_afc.activeBranches.push_back(std::move(br));
            if (ok) {
                forwardValid++;
            } else {
                break; // Dừng đếm khi chạm ranh giới frame chết phía sau
            }
        }

        // Công thức: Tổng số Frame của Click/Release = 1 (Macro gốc) + Backward khả thi + Forward khả thi
        int totalWindow = 1 + backwardValid + forwardValid;

        auto& act = g_afc.inputs[actionIdx];
        act.analyzed = true;
        act.backwardFrames = backwardValid;
        act.forwardFrames = forwardValid;
        act.windowSize = totalWindow;

        int bucket = (totalWindow > MAX_BUCKET) ? (MAX_BUCKET + 1) : totalWindow;
        g_afc.tally[bucket]++;
    }

    void triggerActionFeedback(const MacroInput& act) {
        g_afc.lastPopupText = fmt::format(
            "{}F ({}: 1 + {}B + {}A)",
            act.windowSize,
            act.down ? "CLICK" : "REL",
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
                    saveMacroAndAnalysis(m_level);
                    this->onSwitchAnalyze(nullptr);
                } else if (g_afc.stage == BotStage::Analyze) {
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
                auto path = getLevelDataPath(m_level);
                if (std::filesystem::exists(path)) {
                    std::filesystem::remove(path);
                }
                g_afc.inputs.clear();
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

        // Nếu đang ở chế độ Analyze và còn thao tác chưa phân tích:
        // phân tích từng thao tác một rồi chuyển sang phát lại mượt mà
        if (g_afc.stage == BotStage::Analyze) {
            if (g_afc.analyzeActionIdx < g_afc.inputs.size()) {
                analyzeSingleMacroAction(g_afc.analyzeActionIdx);
                g_afc.analyzeActionIdx++;
                refreshHudText();

                if (g_afc.analyzeActionIdx >= g_afc.inputs.size()) {
                    g_afc.analysisCompleted = true;
                    saveMacroAndAnalysis(m_level);
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
            if (isPlayer1) g_afc.holdingP1 = down;
            CCPoint pos = pl->m_player1 ? pl->m_player1->getPosition() : CCPoint{0.f, 0.f};
            g_afc.inputs.push_back(MacroInput{
                g_afc.currentFrame,
                down,
                button,
                isPlayer1,
                pos.x,
                pos.y,
                false, 0, 0, 1
            });
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = static_cast<AFCPlayLayer*>(PlayLayer::get());
        if (pl && g_afc.enabled && !g_afc.isSimulating) {
            // Phát lại chính xác từng frame trong chế độ Replay
            if (g_afc.stage == BotStage::Replay) {
                while (g_afc.playbackIndex < g_afc.inputs.size() &&
                       g_afc.inputs[g_afc.playbackIndex].frame == g_afc.currentFrame) {
                    const auto& act = g_afc.inputs[g_afc.playbackIndex];
                    GJBaseGameLayer::handleButton(act.down, act.button, act.player1);
                    if (act.analyzed) {
                        pl->triggerActionFeedback(act);
                    }
                    g_afc.playbackIndex++;
                }
            }

            // Lưu tọa độ thật của nhân vật mỗi tick để vẽ đường White Trajectory Arc
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
