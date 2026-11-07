#pragma once
#include <Geode/Geode.hpp>
#include <vector>

using namespace geode::prelude;

// Lưu trạng thái của từng GameObject / EnhancedGameObject (Orb, Pad, Portal, Trigger)
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

    static ObjectStateSnapshot capture(GameObject* o);
    void restore() const;
};

// Lưu toàn bộ biến trạng thái vật lý của PlayerObject theo kiến trúc Silicate PracticeFix
struct SilicatePlayerState {
    bool valid = false;
    CCPoint pos{0.f, 0.f};
    CCPoint lastPos{0.f, 0.f};
    CCPoint lastGroundedPos{0.f, 0.f};
    float rotation = 0.f;
    float rotationSpeed = 0.f;

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

    void captureFrom(PlayerObject* p);
    void restoreTo(PlayerObject* p) const;
};

// Gói đóng băng toàn diện PlayLayer tại 1 frame
struct FullPlayLayerSnapshot {
    bool valid = false;
    int frame = 0;
    size_t macroIndex = 0;
    bool holdingP1 = false;
    bool holdingP2 = false;

    Ref<CheckpointObject> engineCkpt = nullptr;
    SilicatePlayerState p1State;
    SilicatePlayerState p2State;
    std::vector<ObjectStateSnapshot> activeObjects;

    static FullPlayLayerSnapshot capture(
        PlayLayer* pl,
        int curFrame,
        size_t mIdx,
        bool hold1,
        bool hold2,
        bool createNativeCkpt
    );
    void restore(PlayLayer* pl) const;
};
