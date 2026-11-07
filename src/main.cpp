#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CheckpointObject.hpp>
#include <Geode/utils/file.hpp>
#include <matjson.hpp>
#include <cmath>
#include <algorithm>
#include <deque>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;

// ===== Hằng số cấu hình Rolling Scan & Trajectory =====
constexpr int MAX_BACKWARD_FRAMES  = 12;  // Số frame tối đa quét lùi trước mỗi Press/Release
constexpr int MAX_FORWARD_FRAMES   = 12;  // Số frame tối đa quét tiến sau mỗi Press/Release
constexpr int SURVIVE_CHECK_FRAMES = 72;  // Số frame hậu kiểm sau thao tác
constexpr int ACTIONS_PER_UI_TICK  = 3;   // Số action quét cuốn chiếu mỗi frame hiển thị
constexpr int PREVIEW_STEPS        = 110; // Độ dài đường dự đoán Ahead Trajectory
constexpr int WHITE_TRAIL_MAX_PTS  = 180; // Độ dài đường quỹ đạo trắng (White Trajectory Arc)
constexpr int MAX_BUCKET           = 10;  // Thống kê từ 1F đến 10F và 11F+

// ===== Phân loại 2 loại Action của Macro =====
enum class ActionType : int {
    Press   = 1, // Type 1: Press
    Release = 2  // Type 2: Release
};

static const char* actionTypeToString(ActionType t) {
    return (t == ActionType::Press) ? "Type 1: Press" : "Type 2: Release";
}

enum class BotStage {
    Record,       // Ghi Macro trong Practice Mode (kèm Silicate Practice Fix)
    RollingScan,  // Quét cuốn chiếu từ đầu đến cuối level -> Lưu file JSON bằng matjson
    Replay        // Đọc file JSON và phát lại Replay + hiển thị Trajectory
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

    // Kết quả phân tích Frame Window
    bool analyzed = false;
    int backwardFrames = 0;
    int forwardFrames = 0;
    int windowSize = 1;

    // Lưu lại chùm quỹ đạo thực tế đo bằng processCommands tại điểm thao tác này
    std::vector<TrajectoryBranch> branches;
};

// ============================================================================
// THUẬT TOÁN PRACTICE FIX & STATE SNAPSHOT CỦA SILICATE (GD 2.2081 SAFE)
// ============================================================================

struct ObjectStateSnapshot {
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
    bool activatedP1 = false;
    bool activatedP2 = false;

    template <typename ObjT>
    static ObjectStateSnapshot capture(ObjT* o) {
        ObjectStateSnapshot s;
        s.obj         = o;
        s.pos         = o->getPosition();
        s.startPos    = o->m_startPosition;
        s.rotX        = o->getRotationX();
        s.rotY        = o->getRotationY();
        s.scaleX      = o->getScaleX();
        s.scaleY      = o->getScaleY();
        s.visible     = o->isVisible();
        s.opacity     = o->getOpacity();
        s.isDisabled  = o->m_isDisabled;
        s.isDisabled2 = o->m_isDisabled2;

        if (auto enh = typeinfo_cast<EnhancedGameObject*>(o)) {
            s.isEnhanced = true;
            if constexpr (requires { enh->m_hasBeenActivated; })   s.activatedP1 = enh->m_hasBeenActivated;
            if constexpr (requires { enh->m_hasBeenActivatedP2; }) s.activatedP2 = enh->m_hasBeenActivatedP2;
        }
        return s;
    }

    void restore() const {
        if (!obj) return;

        // Khôi phục trạng thái chưa kích hoạt cho Orb, Pad, Portal, Trigger (chống chết oan khi chạm lại Orb)
        if (!activatedP1 && !activatedP2) {
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
                if constexpr (requires { enh->m_hasBeenActivated; })   enh->m_hasBeenActivated   = activatedP1;
                if constexpr (requires { enh->m_hasBeenActivatedP2; }) enh->m_hasBeenActivatedP2 = activatedP2;
            }
        }
    }
};

// Cấu trúc lưu toàn bộ biến vật lý của PlayerObject theo đúng Silicate Practice Fix
struct SilicatePlayerFix {
    bool valid = false;
    CCPoint pos{0.f, 0.f};
    CCPoint lastPos{0.f, 0.f};
    CCPoint lastGroundedPos{0.f, 0.f};
    float rotation = 0.f;

    double yVel = 0.0;
    double platXVel = 0.0;
    double gravity = 0.958199;
    double fallSpeed = 0.0;
    float gravityMod = 1.0f;
    float speed = 0.9f;
    float vehicleSize = 1.0f;

    // Trạng thái mặt đất & dốc nghiêng (Slope)
    bool onGround = false;
    bool onGround2 = false;
    bool onGround3 = false;
    bool onGround4 = false;
    bool isOnSlope = false;
    bool wasOnSlope = false;
    float slopeAngle = 0.f;

    // Trạng thái Dash Orb & Ring Jump
    bool dashing = false;
    float dashX = 0.f;
    float dashY = 0.f;
    float dashAngle = 0.f;
    double dashStartTime = 0.0;
    bool ringJumpRelated = false;
    int stateRingJump = 0;

    // Trạng thái Input & Robot
    bool jumpBuffered = false;
    bool wasRobotJump = false;
    bool holdingBtn1 = false;

    // Hướng & Gamemode
    bool upsideDown = false;
    bool goingLeft = false;
    bool sideways = false;
    bool isShip = false;
    bool isUfo = false;
    bool isBall = false;
    bool isWave = false;
    bool isRobot = false;
    bool isSpider = false;
    bool isSwing = false;

    template <typename P>
    void captureFrom(P* p) {
        if (!p) return;
        valid        = true;
        pos          = p->getPosition();
        rotation     = p->getRotation();
        yVel         = p->m_yVelocity;
        platXVel     = p->m_platformerXVelocity;
        gravity      = p->m_gravity;
        gravityMod   = p->m_gravityMod;
        speed        = p->m_playerSpeed;
        vehicleSize  = p->m_vehicleSize;

        onGround     = p->m_isOnGround;
        upsideDown   = p->m_isUpsideDown;
        goingLeft    = p->m_isGoingLeft;
        sideways     = p->m_isSideways;
        dashing      = p->m_isDashing;

        isShip       = p->m_isShip;
        isUfo        = p->m_isBird;
        isBall       = p->m_isBall;
        isWave       = p->m_isDart;
        isRobot      = p->m_isRobot;
        isSpider     = p->m_isSpider;
        isSwing      = p->m_isSwing;

        // Sao lưu các biến chuyên sâu của Silicate Practice Fix (an toàn tuyệt đối với bindings 2.2081)
        if constexpr (requires { p->m_lastPosition; })        lastPos         = p->m_lastPosition;
        if constexpr (requires { p->m_lastGroundedPos; })     lastGroundedPos = p->m_lastGroundedPos;
        if constexpr (requires { p->m_fallSpeed; })           fallSpeed       = p->m_fallSpeed;
        if constexpr (requires { p->m_isOnGround2; })         onGround2       = p->m_isOnGround2;
        if constexpr (requires { p->m_isOnGround3; })         onGround3       = p->m_isOnGround3;
        if constexpr (requires { p->m_isOnGround4; })         onGround4       = p->m_isOnGround4;
        if constexpr (requires { p->m_isOnSlope; })           isOnSlope       = p->m_isOnSlope;
        if constexpr (requires { p->m_wasOnSlope; })          wasOnSlope      = p->m_wasOnSlope;
        if constexpr (requires { p->m_slopeAngle; })          slopeAngle      = p->m_slopeAngle;
        if constexpr (requires { p->m_dashX; })               dashX           = p->m_dashX;
        if constexpr (requires { p->m_dashY; })               dashY           = p->m_dashY;
        if constexpr (requires { p->m_dashAngle; })           dashAngle       = p->m_dashAngle;
        if constexpr (requires { p->m_dashStartTime; })       dashStartTime   = p->m_dashStartTime;
        if constexpr (requires { p->m_ringJumpRelated; })     ringJumpRelated = p->m_ringJumpRelated;
        if constexpr (requires { p->m_stateRingJump; })       stateRingJump   = p->m_stateRingJump;
        if constexpr (requires { p->m_jumpBuffered; })        jumpBuffered    = p->m_jumpBuffered;
        if constexpr (requires { p->m_wasRobotJump; })        wasRobotJump    = p->m_wasRobotJump;
        if constexpr (requires { p->m_holdingButtons[1]; }) {
            auto it = p->m_holdingButtons.find(1);
            if (it != p->m_holdingButtons.end()) holdingBtn1 = it->second;
        }
    }

    template <typename P>
    void restoreTo(P* p) const {
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

        p->m_isOnGround          = onGround;
        p->m_isUpsideDown        = upsideDown;
        p->m_isGoingLeft         = goingLeft;
        p->m_isSideways          = sideways;
        p->m_isDashing           = dashing;

        p->m_isShip              = isShip;
        p->m_isBird              = isUfo;
        p->m_isBall              = isBall;
        p->m_isDart              = isWave;
        p->m_isRobot             = isRobot;
        p->m_isSpider            = isSpider;
        p->m_isSwing             = isSwing;

        if constexpr (requires { p->m_lastPosition; })        p->m_lastPosition    = lastPos;
        if constexpr (requires { p->m_lastGroundedPos; })     p->m_lastGroundedPos = lastGroundedPos;
        if constexpr (requires { p->m_fallSpeed; })           p->m_fallSpeed       = fallSpeed;
        if constexpr (requires { p->m_isOnGround2; })         p->m_isOnGround2     = onGround2;
        if constexpr (requires { p->m_isOnGround3; })         p->m_isOnGround3     = onGround3;
        if constexpr (requires { p->m_isOnGround4; })         p->m_isOnGround4     = onGround4;
        if constexpr (requires { p->m_isOnSlope; })           p->m_isOnSlope       = isOnSlope;
        if constexpr (requires { p->m_wasOnSlope; })          p->m_wasOnSlope      = wasOnSlope;
        if constexpr (requires { p->m_slopeAngle; })          p->m_slopeAngle      = slopeAngle;
        if constexpr (requires { p->m_dashX; })               p->m_dashX           = dashX;
        if constexpr (requires { p->m_dashY; })               p->m_dashY           = dashY;
        if constexpr (requires { p->m_dashAngle; })           p->m_dashAngle       = dashAngle;
        if constexpr (requires { p->m_dashStartTime; })       p->m_dashStartTime   = dashStartTime;
        if constexpr (requires { p->m_ringJumpRelated; })     p->m_ringJumpRelated = ringJumpRelated;
        if constexpr (requires { p->m_stateRingJump; })       p->m_stateRingJump   = stateRingJump;
        if constexpr (requires { p->m_jumpBuffered; })        p->m_jumpBuffered    = jumpBuffered;
        if constexpr (requires { p->m_wasRobotJump; })        p->m_wasRobotJump    = wasRobotJump;
        if constexpr (requires { p->m_holdingButtons[1]; })   p->m_holdingButtons[1] = holdingBtn1;

        // Xóa danh sách Orb đang kẹt nếu người chơi không giữ phím tại thời điểm snapshot
        if (!holdingBtn1) {
            if constexpr (requires { p->m_touchedRings.clear(); }) {
                p->m_touchedRings.clear();
            } else if constexpr (requires { p->m_touchedRings->removeAllObjects(); }) {
                if (p->m_touchedRings) p->m_touchedRings->removeAllObjects();
            }
        }
    }
};

// Gói Snapshot đóng băng toàn bộ PlayLayer tại một Frame cụ thể
struct FullStateSnapshot {
    bool valid = false;
    int frame = 0;
    size_t macroIndex = 0;
    bool holdingP1 = false;
    bool holdingP2 = false;

    Ref<CheckpointObject> engineCkpt = nullptr;
    SilicatePlayerFix p1Fix;
    SilicatePlayerFix p2Fix;
    std::vector<ObjectStateSnapshot> activeObjects;

    static FullStateSnapshot capture(PlayLayer* pl, int curFrame, size_t mIdx, bool hold1, bool hold2, bool createNativeCkpt) {
        FullStateSnapshot s;
        if (!pl || !pl->m_player1) return s;

        s.valid      = true;
        s.frame      = curFrame;
        s.macroIndex = mIdx;
        s.holdingP1  = hold1;
        s.holdingP2  = hold2;

        if (createNativeCkpt) {
            s.engineCkpt = pl->createCheckpoint();
            if (s.engineCkpt) {
                if constexpr (requires { s.engineCkpt->m_physicalCheckpointObject; }) {
                    if (s.engineCkpt->m_physicalCheckpointObject) {
                        s.engineCkpt->m_physicalCheckpointObject->setVisible(false);
                        s.engineCkpt->m_physicalCheckpointObject->removeFromParent();
                    }
                }
            }
        }

        s.p1Fix.captureFrom(pl->m_player1);
        s.p2Fix.captureFrom(pl->m_player2);

        if (pl->m_objects) {
            float px = pl->m_player1->getPositionX();
            float minX = px - 900.f;
            float maxX = px + 3200.f;

            int count = pl->m_objects->count();
            s.activeObjects.reserve(256);
            for (int i = 0; i < count; i++) {
                auto obj = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
                if (!obj) continue;
                float ox = obj->getPositionX();
                if (ox >= minX && ox <= maxX) {
                    s.activeObjects.push_back(ObjectStateSnapshot::capture(obj));
                }
            }
        }
        return s;
    }

    void restore(PlayLayer* pl) const {
        if (!valid || !pl) return;

        if (engineCkpt) {
            pl->loadFromCheckpoint(engineCkpt);
        }

        for (const auto& entry : activeObjects) {
            entry.restore();
        }

        p1Fix.restoreTo(pl->m_player1);
        p2Fix.restoreTo(pl->m_player2);

        pl->handleButton(holdingP1, 1, true);
        if (pl->m_player2) {
            pl->handleButton(holdingP2, 1, false);
        }
    }
};

// ============================================================================
// GEODE DOCS IDIOM: GẮN SNAPSHOT VÀO `CheckpointObject` BẰNG `struct Fields`
// ============================================================================
class $modify(AFCCheckpointObject, CheckpointObject) {
    struct Fields {
        FullStateSnapshot snapshot;
    };
};

// ============================================================================
// BỘ MÔ PHỎNG VẬT LÝ CHUẨN 60HZ BASE FACTOR CHO AHEAD TRAJECTORY PREVIEW
// ============================================================================
struct SilicateTrajectoryPredictor {
    CCPoint pos{0.f, 0.f};
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

    static SilicateTrajectoryPredictor fromPlayer(PlayerObject* p) {
        SilicateTrajectoryPredictor s;
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

    float getHorizontalSpeed() const {
        if (speed <= 0.75f) return 251.16008f; // 0.5x
        if (speed <= 0.95f) return 311.58010f; // 1.0x
        if (speed <= 1.15f) return 387.42014f; // 2.0x
        if (speed <= 1.35f) return 468.00015f; // 3.0x
        return 576.00018f;                     // 4.0x
    }

    // Công thức tích phân khớp với PlayerObject::updateJump & update của GD 2.2081
    void step(float dt, bool holding, bool justPressed) {
        const float stepScale = dt * 60.0f; // Hệ số chuẩn hóa 60Hz của GD
        const float gravDir   = upsideDown ? -1.0f : 1.0f;
        const float miniScale = (vehicleSize < 0.8f) ? 0.8f : 1.0f;

        float dx = getHorizontalSpeed() * dt;
        pos.x += dx;

        if (isWave) {
            float slope = (vehicleSize < 0.8f) ? 2.0f : 1.0f;
            float dir = (holding ? 1.0f : -1.0f) * gravDir;
            pos.y += dx * dir * slope;
            return;
        }

        if (isShip) {
            double accel = holding ? 1.394 : -1.115;
            if (vehicleSize < 0.8f) accel *= 1.176;
            yVel += accel * gravDir * gravityMod * (stepScale * 0.5f);
            double maxUp = (vehicleSize < 0.8f) ? 8.5 : 8.0;
            double maxDown = (vehicleSize < 0.8f) ? -7.2 : -6.4;
            yVel = !upsideDown ? std::clamp(yVel, maxDown, maxUp) : std::clamp(yVel, -maxUp, -maxDown);
        } else if (isUfo) {
            if (justPressed) {
                yVel = ((vehicleSize < 0.8f) ? 7.25 : 7.85) * gravDir;
                onGround = false;
            } else {
                yVel -= 0.70 * gravDir * gravityMod * (stepScale * 0.5f);
            }
            yVel = std::clamp(yVel, -10.8, 10.8);
        } else if (isSwing) {
            if (justPressed) upsideDown = !upsideDown;
            double swingDir = upsideDown ? -1.0 : 1.0;
            yVel -= 0.74 * swingDir * gravityMod * (stepScale * 0.5f);
            yVel = std::clamp(yVel, -9.6, 9.6);
        } else if (isBall) {
            if (justPressed && onGround) {
                upsideDown = !upsideDown;
                onGround = false;
                yVel = -1.55 * (upsideDown ? -1.0 : 1.0);
            }
            double ballGrav = upsideDown ? -0.64 : 0.64;
            yVel -= ballGrav * gravityMod * (stepScale * 0.5f);
            yVel = std::clamp(yVel, -12.0, 12.0);
        } else {
            // Cube / Robot chuẩn GD 2.2 (không nhân 0.9f sai lệch)
            if ((justPressed || holding) && onGround) {
                double jumpVel = 11.180032 * miniScale;
                yVel = jumpVel * gravDir;
                onGround = false;
            }
            yVel -= gravity * gravDir * gravityMod * (stepScale * 0.5f);
            yVel = std::clamp(yVel, -15.0, 15.0);
        }

        // Độ dịch chuyển trục Y chuẩn trong PlayerObject::update: pos.y += yVel * stepScale
        pos.y += static_cast<float>(yVel * stepScale);

        if (!upsideDown && pos.y <= 105.0f) {
            pos.y = 105.0f;
            if (yVel < 0.0) yVel = 0.0;
            onGround = true;
        }
    }
};

static int evaluatePreviewCollision(PlayLayer* pl, SilicateTrajectoryPredictor& sim, bool holding) {
    if (!pl || !pl->m_objects) return 0;

    const float half = 15.0f * sim.vehicleSize; // Hitbox chuẩn 30x30 của GD
    CCRect outerRect(sim.pos.x - half, sim.pos.y - half, half * 2.0f, half * 2.0f);
    CCRect innerRect(sim.pos.x - half * 0.35f, sim.pos.y - half * 0.35f, half * 0.7f, half * 0.7f);

    int count = pl->m_objects->count();
    for (int i = 0; i < count; i++) {
        auto obj = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
        if (!obj || !obj->isVisible()) continue;

        float ox = obj->getPositionX();
        if (std::abs(ox - sim.pos.x) > 85.0f) continue;
        float oy = obj->getPositionY();
        if (std::abs(oy - sim.pos.y) > 85.0f) continue;

        CCRect objRect = obj->getObjectRect();
        if (objRect.size.width <= 0.1f || objRect.size.height <= 0.1f) continue;

        auto type = obj->m_objectType;
        if (type == GameObjectType::Hazard || type == GameObjectType::AnimatedHazard) {
            if (innerRect.intersectsRect(objRect)) return -1;
        } else if (type == GameObjectType::Solid) {
            if (outerRect.intersectsRect(objRect)) {
                float topY = objRect.getMaxY();
                float botY = objRect.getMinY();
                if (!sim.upsideDown && sim.pos.y >= topY - 9.0f && sim.yVel <= 0.0) {
                    sim.pos.y = topY + half;
                    sim.yVel = 0.0;
                    sim.onGround = true;
                    return 1;
                } else if (sim.upsideDown && sim.pos.y <= botY + 9.0f && sim.yVel >= 0.0) {
                    sim.pos.y = botY - half;
                    sim.yVel = 0.0;
                    sim.onGround = true;
                    return 1;
                } else if (innerRect.intersectsRect(objRect)) {
                    return -1;
                }
            }
        } else if (outerRect.intersectsRect(objRect)) {
            // Mô phỏng khi tia dự đoán chạm vào Jump Pad hoặc Gravity Pad
            if (type == GameObjectType::YellowJumpPad) {
                sim.yVel = 15.2 * (sim.upsideDown ? -1.0 : 1.0) * (sim.vehicleSize < 0.8f ? 0.8 : 1.0);
                sim.onGround = false;
            } else if (type == GameObjectType::PinkJumpPad) {
                sim.yVel = 10.0 * (sim.upsideDown ? -1.0 : 1.0) * (sim.vehicleSize < 0.8f ? 0.8 : 1.0);
                sim.onGround = false;
            } else if (type == GameObjectType::RedJumpPad) {
                sim.yVel = 18.8 * (sim.upsideDown ? -1.0 : 1.0) * (sim.vehicleSize < 0.8f ? 0.8 : 1.0);
                sim.onGround = false;
            } else if (type == GameObjectType::GravityPad) {
                sim.upsideDown = !sim.upsideDown;
                sim.onGround = false;
                sim.yVel = -2.5 * (sim.upsideDown ? -1.0 : 1.0);
            } else if (holding && type == GameObjectType::YellowJumpRing) {
                sim.yVel = 11.18 * (sim.upsideDown ? -1.0 : 1.0) * (sim.vehicleSize < 0.8f ? 0.8 : 1.0);
                sim.onGround = false;
            }
        }
    }
    return 0;
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
// GEODE DOCS IDIOM: QUẢN LÝ TRẠNG THÁI TRONG `AFCPlayLayer::Fields` & `matjson`
// ============================================================================
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
        FullStateSnapshot rollingMainSnapshot;
        std::vector<TrajectoryBranch> activeBranches;

        int tally[MAX_BUCKET + 2] = {};
        std::deque<CCPoint> whiteTrail;
        std::string lastPopupText;
        int popupTimer = 0;

        CCNode* hudRoot = nullptr;
        CCDrawNode* worldDrawer = nullptr;
        CCLabelBMFont* tallyLabel = nullptr;
        CCLabelBMFont* statusLabel = nullptr;
        CCLabelBMFont* popupLabel = nullptr;
        CCLabelBMFont* playerPopupLabel = nullptr;
    };

    std::filesystem::path getLevelJsonPath() {
        int id = m_level ? m_level->m_levelID.value() : 0;
        std::string filename = (id > 0)
            ? fmt::format("level_{}.json", id)
            : fmt::format("editor_{}.json", m_level ? std::string(m_level->m_levelName) : "custom");
        return Mod::get()->getSaveDir() / filename;
    }

    void recomputeTally() {
        auto* f = m_fields.self();
        std::fill(std::begin(f->tally), std::end(f->tally), 0);
        for (const auto& act : f->actions) {
            if (act.analyzed && act.windowSize > 0) {
                int b = (act.windowSize > MAX_BUCKET) ? (MAX_BUCKET + 1) : act.windowSize;
                f->tally[b]++;
            }
        }
    }

    // Lưu Macro & Frame Window bằng thư viện `matjson` chính thức của Geode SDK
    void saveMacroToJson() {
        if (!m_level) return;
        auto* f = m_fields.self();
        recomputeTally();

        matjson::Value root = matjson::Value::object();
        root["level_id"]      = m_level->m_levelID.value();
        root["target_fps"]    = f->targetFps;
        root["analyzed"]      = f->analysisCompleted;
        root["total_actions"] = static_cast<int>(f->actions.size());

        matjson::Value tallyObj = matjson::Value::object();
        for (int w = 1; w <= MAX_BUCKET; w++) {
            tallyObj[fmt::format("{}F", w)] = f->tally[w];
        }
        tallyObj[fmt::format("{}+F", MAX_BUCKET + 1)] = f->tally[MAX_BUCKET + 1];
        root["tally"] = std::move(tallyObj);

        matjson::Value actionsArr = matjson::Value::array();
        for (size_t i = 0; i < f->actions.size(); i++) {
            const auto& a = f->actions[i];
            matjson::Value item = matjson::Value::object();
            item["index"]           = static_cast<int>(i);
            item["frame"]           = a.frame;
            item["type"]            = (a.type == ActionType::Press) ? 1 : 2;
            item["type_label"]      = actionTypeToString(a.type);
            item["button"]          = a.button;
            item["player1"]         = a.player1;
            item["x"]               = a.x;
            item["y"]               = a.y;
            item["analyzed"]        = a.analyzed;
            item["backward_frames"] = a.backwardFrames;
            item["forward_frames"]  = a.forwardFrames;
            item["window_size"]     = a.windowSize;
            actionsArr.push(std::move(item));
        }
        root["actions"] = std::move(actionsArr);

        (void)file::writeString(getLevelJsonPath(), root.dump());
    }

    // Đọc dữ liệu từ file JSON bằng `matjson::parse` của Geode SDK
    bool loadMacroFromJson() {
        auto* f = m_fields.self();
        f->actions.clear();
        f->analysisCompleted = false;
        std::fill(std::begin(f->tally), std::end(f->tally), 0);

        auto path = getLevelJsonPath();
        if (!std::filesystem::exists(path)) return false;

        auto strRes = file::readString(path);
        if (!strRes.isOk()) return false;

        auto jsonRes = matjson::parse(strRes.unwrap());
        if (!jsonRes.isOk()) return false;

        matjson::Value root = jsonRes.unwrap();
        f->analysisCompleted = root["analyzed"].asBool().unwrapOr(false);

        if (root.contains("actions") && root["actions"].isArray()) {
            if (auto arrRes = root["actions"].asArray(); arrRes.isOk()) {
                for (const auto& item : arrRes.unwrap()) {
                    MacroAction act;
                    act.index          = static_cast<int>(item["index"].asInt().unwrapOr(0));
                    act.frame          = static_cast<int>(item["frame"].asInt().unwrapOr(0));
                    int typeCode       = static_cast<int>(item["type"].asInt().unwrapOr(1));
                    act.type           = (typeCode == 2) ? ActionType::Release : ActionType::Press;
                    act.button         = static_cast<int>(item["button"].asInt().unwrapOr(1));
                    act.player1        = item["player1"].asBool().unwrapOr(true);
                    act.x              = static_cast<float>(item["x"].asDouble().unwrapOr(0.0));
                    act.y              = static_cast<float>(item["y"].asDouble().unwrapOr(0.0));
                    act.analyzed       = item["analyzed"].asBool().unwrapOr(false);
                    act.backwardFrames = static_cast<int>(item["backward_frames"].asInt().unwrapOr(0));
                    act.forwardFrames  = static_cast<int>(item["forward_frames"].asInt().unwrapOr(0));
                    act.windowSize     = std::max(1, static_cast<int>(item["window_size"].asInt().unwrapOr(1)));
                    f->actions.push_back(std::move(act));
                }
            }
        }

        recomputeTally();
        return !f->actions.empty();
    }

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

        // Bảng đếm 1F..10F+ bên trái (Không có dòng chữ NanCounter)
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

        // 2. Vẽ chùm nhánh Trajectory thực tế đo bằng `processCommands` của thao tác vừa kích hoạt
        for (const auto& branch : f->activeBranches) {
            if (branch.points.size() < 2) continue;
            ccColor4F col;
            float width = 1.2f;
            if (branch.delta == 0) {
                col = {1.0f, 0.92f, 0.15f, 0.95f}; // Vàng: Quỹ đạo gốc của Macro
                width = 1.9f;
            } else if (branch.survived) {
                col = {0.15f, 1.0f, 0.35f, 0.70f}; // Xanh lá: Các frame trước/sau vượt qua được
            } else {
                col = {1.0f, 0.20f, 0.20f, 0.45f}; // Đỏ: Các frame trước/sau chạm gai/tường chết
            }

            for (size_t i = 0; i + 1 < branch.points.size(); i++) {
                f->worldDrawer->drawSegment(branch.points[i], branch.points[i + 1], width, col);
            }
            f->worldDrawer->drawDot(branch.points.back(), branch.survived ? 2.4f : 3.2f, col);
        }

        // 3. Vẽ Ahead Trajectory Preview phía trước nhân vật
        if (f->showAheadTraj && !m_player1->m_isDead) {
            float dt = 1.0f / static_cast<float>(f->targetFps);
            for (int branchType = 0; branchType < 2; branchType++) {
                bool holdBranch = (branchType == 0);
                SilicateTrajectoryPredictor sim = SilicateTrajectoryPredictor::fromPlayer(m_player1);
                CCPoint prevPt = sim.pos;
                ccColor4F previewCol = holdBranch
                    ? ccColor4F{0.20f, 0.88f, 1.0f, 0.75f}
                    : ccColor4F{1.0f, 0.55f, 0.20f, 0.65f};

                for (int step = 0; step < PREVIEW_STEPS; step++) {
                    sim.step(dt, holdBranch, step == 0 && holdBranch);
                    int hit = evaluatePreviewCollision(this, sim, holdBranch);

                    f->worldDrawer->drawSegment(prevPt, sim.pos, 1.3f, previewCol);
                    prevPt = sim.pos;

                    if (hit == -1) {
                        f->worldDrawer->drawDot(sim.pos, 3.6f, {1.0f, 0.1f, 0.1f, 0.95f});
                        break;
                    }
                }
            }
        }
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto* f = m_fields.self();
        syncSettings();

        bool hasJsonMacro = loadMacroFromJson();
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
            loadMacroFromJson();
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
            loadMacroFromJson();
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

    // Lưu Silicate Snapshot trực tiếp vào `AFCCheckpointObject::m_fields` khi tạo Checkpoint
    CheckpointObject* createCheckpoint() {
        auto ckpt = PlayLayer::createCheckpoint();
        auto* f = m_fields.self();
        if (ckpt && f->enabled && !f->isSimulating && f->stage == BotStage::Record) {
            auto* ckptMod = static_cast<AFCCheckpointObject*>(ckpt);
            ckptMod->m_fields->snapshot = FullStateSnapshot::capture(
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
            saveMacroToJson();
            startRollingScan();
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (object == m_anticheatSpike) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }

        auto* f = m_fields.self();
        // Trong lúc mô phỏng Trajectory, chặn hiệu ứng nổ/âm thanh theo đúng cách của Silicate
        if (f->isSimulating) {
            if (player) player->m_isDead = true;
            f->simDead = true;
            return;
        }

        PlayLayer::destroyPlayer(player, object);
    }

    // Chạy thử 1 nhánh độ lệch `delta` từ `rollingMainSnapshot` bằng `GJBaseGameLayer::processCommands`
    TrajectoryBranch testLocalBranchFromSnapshot(size_t actionIdx, int delta) {
        auto* f = m_fields.self();
        TrajectoryBranch branch;
        branch.delta = delta;
        branch.survived = false;

        const auto& targetAct = f->actions[actionIdx];
        int origFrame = targetAct.frame;
        int shiftedFrame = origFrame + delta;

        if (shiftedFrame < f->rollingMainSnapshot.frame) return branch;
        if (actionIdx > 0 && shiftedFrame <= f->actions[actionIdx - 1].frame) return branch;
        if (targetAct.type == ActionType::Release && actionIdx + 1 < f->actions.size()) {
            if (shiftedFrame >= f->actions[actionIdx + 1].frame) return branch;
        }

        int nextReleaseShiftedFrame = -1;
        if (targetAct.type == ActionType::Press && actionIdx + 1 < f->actions.size() &&
            f->actions[actionIdx + 1].type == ActionType::Release) {
            int origRelFrame = f->actions[actionIdx + 1].frame;
            int holdLen = std::max(1, origRelFrame - origFrame);
            if (shiftedFrame >= origRelFrame) {
                nextReleaseShiftedFrame = shiftedFrame + holdLen;
                if (actionIdx + 2 < f->actions.size() &&
                    nextReleaseShiftedFrame >= f->actions[actionIdx + 2].frame) {
                    nextReleaseShiftedFrame = f->actions[actionIdx + 2].frame - 1;
                }
                if (nextReleaseShiftedFrame <= shiftedFrame) return branch;
            }
        }

        f->rollingMainSnapshot.restore(this);

        int simFrame = f->rollingMainSnapshot.frame;
        size_t mIdx = f->rollingMainSnapshot.macroIndex;
        int targetEndFrame = origFrame + SURVIVE_CHECK_FRAMES;
        if (actionIdx + 1 < f->actions.size()) {
            targetEndFrame = std::max(targetEndFrame, f->actions[actionIdx + 1].frame + 35);
        }

        float dt = 1.0f / static_cast<float>(f->targetFps);
        f->simDead = false;
        branch.survived = true;

        while (simFrame <= targetEndFrame) {
            while (mIdx < f->actions.size()) {
                int effectiveFrame = f->actions[mIdx].frame;
                if (mIdx == actionIdx) {
                    effectiveFrame = shiftedFrame;
                } else if (mIdx == actionIdx + 1 && nextReleaseShiftedFrame != -1) {
                    effectiveFrame = nextReleaseShiftedFrame;
                }

                if (effectiveFrame > simFrame) break;
                if (effectiveFrame == simFrame) {
                    bool isDown = (f->actions[mIdx].type == ActionType::Press);
                    this->handleButton(isDown, f->actions[mIdx].button, f->actions[mIdx].player1);
                }
                mIdx++;
            }

            GJBaseGameLayer::processCommands(dt, false, true);

            if (m_player1) {
                branch.points.push_back(m_player1->getPosition());
            }

            if (f->simDead || (m_player1 && m_player1->m_isDead)) {
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

        f->rollingMainSnapshot = FullStateSnapshot::capture(
            this,
            f->rollingMainFrame,
            f->rollingMainMacroIdx,
            f->holdingP1,
            f->holdingP2,
            true
        );

        f->activeBranches.clear();

        // 1. Nhánh gốc (delta = 0)
        f->activeBranches.push_back(testLocalBranchFromSnapshot(actionIdx, 0));

        // 2. Nhánh lùi (Backward Frame Check)
        int backwardValid = 0;
        for (int b = 1; b <= MAX_BACKWARD_FRAMES; b++) {
            auto br = testLocalBranchFromSnapshot(actionIdx, -b);
            bool ok = br.survived;
            f->activeBranches.push_back(std::move(br));
            if (ok) backwardValid++;
            else break;
        }

        // 3. Nhánh tiến (Forward Frame Check)
        int forwardValid = 0;
        for (int fw = 1; fw <= MAX_FORWARD_FRAMES; fw++) {
            auto br = testLocalBranchFromSnapshot(actionIdx, +fw);
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
                    saveMacroToJson();
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
                auto path = getLevelJsonPath();
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
                    f->rollingMainSnapshot = FullStateSnapshot{};
                    f->activeBranches.clear();
                    saveMacroToJson();
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
