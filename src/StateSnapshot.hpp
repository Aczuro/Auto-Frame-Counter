#pragma once
#include <Geode/Geode.hpp>
#include <vector>
#include <type_traits>

using namespace geode::prelude;

namespace afc::detail {
    // C++20 concept-based SFINAE adapters to remain compatible across Geode bindings
    // (e.g., m_yVelocity vs m_yAccel, m_xVelocity vs m_xAccel / m_platformerXVelocity)
    inline double getPlayerYVelocity(PlayerObject* p) {
        if (!p) return 0.0;
        if constexpr (requires { p->m_yVelocity; }) {
            return static_cast<double>(p->m_yVelocity);
        } else if constexpr (requires { p->m_yAccel; }) {
            return static_cast<double>(p->m_yAccel);
        }
        return 0.0;
    }

    inline void setPlayerYVelocity(PlayerObject* p, double val) {
        if (!p) return;
        if constexpr (requires { p->m_yVelocity; }) {
            p->m_yVelocity = val;
        }
        if constexpr (requires { p->m_yAccel; }) {
            p->m_yAccel = val;
        }
    }

    inline double getPlayerXVelocity(PlayerObject* p) {
        if (!p) return 0.0;
        if constexpr (requires { p->m_xVelocity; }) {
            return static_cast<double>(p->m_xVelocity);
        } else if constexpr (requires { p->m_platformerXVelocity; }) {
            return static_cast<double>(p->m_platformerXVelocity);
        } else if constexpr (requires { p->m_xAccel; }) {
            return static_cast<double>(p->m_xAccel);
        }
        return 0.0;
    }

    inline void setPlayerXVelocity(PlayerObject* p, double val) {
        if (!p) return;
        if constexpr (requires { p->m_xVelocity; }) {
            p->m_xVelocity = val;
        }
        if constexpr (requires { p->m_platformerXVelocity; }) {
            p->m_platformerXVelocity = val;
        }
        if constexpr (requires { p->m_xAccel; }) {
            p->m_xAccel = val;
        }
    }
} // namespace afc::detail

// Struct lưu thông tin trạng thái của các GameObject tương tác (Orbs, Pads, Portals, Triggers)
struct ObjectStateSnapshot {
    GameObject* obj = nullptr;
    CCPoint pos;
    CCPoint startPos;
    float rotX = 0.f;
    float rotY = 0.f;
    float scaleX = 1.f;
    float scaleY = 1.f;
    bool visible = true;
    GLubyte opacity = 255;
    bool isDisabled = false;
    bool isDisabled2 = false;
    bool hasBeenActivated = false;
    bool hasBeenActivatedP2 = false;

    static ObjectStateSnapshot capture(GameObject* o);
    void restore() const;
};

// Cấu trúc GameStateSnapshot toàn diện đóng băng trạng thái vật lý của PlayerObject
// Tham chiếu mô hình State Management của Silicate / yBot / Echo
struct GameStateSnapshot {
    // Vị trí không gian & toạ độ nội bộ
    CCPoint pos{0.f, 0.f};
    CCPoint prevPos{0.f, 0.f};
    CCPoint realPos{0.f, 0.f};
    CCPoint lastPos{0.f, 0.f};

    // Vận tốc & gia tốc vật lý (m_yVelocity, m_xVelocity, m_playerSpeed)
    double m_yVelocity = 0.0;
    double m_xVelocity = 0.0;
    float playerSpeed = 0.9f;
    float rotation = 0.f;
    float rotationSpeed = 0.f;

    // Trọng lực, kích thước (Mini/Normal) và trạng thái Ngửa/Sấp (m_isUpsideDown)
    double gravity = 0.958199;
    float gravityMod = 1.0f;
    float vehicleSize = 1.0f;
    float spriteScaleX = 1.0f;
    float spriteScaleY = 1.0f;
    bool m_isUpsideDown = false;

    // Trạng thái sinh tử & tiếp đất
    bool m_isDead = false;
    bool isGrounded = false;
    bool isJumping = false;
    bool isHolding = false;
    bool isSliding = false;
    bool isDashing = false;
    double fallSpeed = 0.0;

    // Trạng thái mặt phẳng nghiêng (Slope Physics)
    bool isOnSlope = false;
    bool wasOnSlope = false;
    float slopeAngle = 0.f;
    double slopeStartTime = 0.0;

    // Trạng thái Dash Orb & Ring Jump
    bool dashing = false;
    float dashX = 0.f;
    float dashY = 0.f;
    float dashAngle = 0.f;
    double dashStartTime = 0.0;
    bool ringJumpRelated = false;
    int stateRingJump = 0;

    // Trạng thái Input Buffer & Robot/UFO/Spider
    bool jumpBuffered = false;
    bool wasRobotJump = false;
    double robotJumpTime = 0.0;
    bool holdingRight = false;
    bool holdingLeft = false;

    // Hướng di chuyển & Gamemode hiện tại
    bool goingLeft = false;
    bool sideways = false;
    bool isShip = false;
    bool isUfo = false;
    bool isBall = false;
    bool isWave = false;
    bool isRobot = false;
    bool isSpider = false;
    bool isSwing = false;

    void captureFrom(PlayerObject* p);
    void restoreTo(PlayerObject* p) const;
};

// Alias giữ tương thích ngược với mã nguồn cũ
using SilicatePlayerState = GameStateSnapshot;

// Gói đóng băng toàn diện PlayLayer + GJGameState + 2 PlayerObject tại 1 frame vật lý cố định
struct FullPlayLayerSnapshot {
    bool valid = false;
    int frame = 0;
    size_t macroIndex = 0;
    bool holdingP1 = false;
    bool holdingP2 = false;

    // Trạng thái tổng thể của GJBaseGameLayer::m_gameState (Camera, Dual, TimeWarp, Seed)
    GJGameState savedGameState{};
    bool isDualMode = false;

    Ref<CheckpointObject> engineCkpt = nullptr;
    GameStateSnapshot p1State;
    GameStateSnapshot p2State;

    std::vector<ObjectStateSnapshot> touchedObjects;

    void clear();
    bool capture(PlayLayer* pl, int exactPhysicsFrame, size_t mIdx, bool hP1, bool hP2);
    bool restore(PlayLayer* pl) const;
};
