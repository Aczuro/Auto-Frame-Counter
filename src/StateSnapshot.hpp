#pragma once
#include <Geode/Geode.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <vector>

using namespace geode::prelude;

namespace afc::detail {

inline double getPlayerYVelocity(PlayerObject* p) {
    return p ? p->m_yVelocity : 0.0;
}

inline void setPlayerYVelocity(PlayerObject* p, double val) {
    if (p) p->m_yVelocity = val;
}

inline double getPlayerXVelocity(PlayerObject* p) {
    return p ? p->m_platformerXVelocity : 0.0;
}

inline void setPlayerXVelocity(PlayerObject* p, double val) {
    if (p) p->m_platformerXVelocity = val;
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

using SilicatePlayerState = GameStateSnapshot;

// Gói đóng băng toàn diện PlayLayer + CheckpointObject + 2 PlayerObject tại 1 frame vật lý cố định
struct FullPlayLayerSnapshot {
    bool valid = false;
    int frame = 0;
    size_t macroIndex = 0;
    bool holdingP1 = false;
    bool holdingP2 = false;

    bool isDualMode = false;

    Ref<CheckpointObject> engineCkpt = nullptr;
    GameStateSnapshot p1State;
    GameStateSnapshot p2State;

    std::vector<ObjectStateSnapshot> touchedObjects;

    void clear();
    bool capture(PlayLayer* pl, int exactPhysicsFrame, size_t mIdx, bool hP1, bool hP2);
    bool restore(PlayLayer* pl) const;
};
