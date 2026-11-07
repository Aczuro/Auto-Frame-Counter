#include "StateSnapshot.hpp"

template <typename T>
static void readEnhancedActivation(T* enh, bool& act1, bool& act2) {
    if constexpr (requires { enh->m_hasBeenActivated; })   act1 = enh->m_hasBeenActivated;
    if constexpr (requires { enh->m_hasBeenActivatedP2; }) act2 = enh->m_hasBeenActivatedP2;
}

template <typename T>
static void writeEnhancedActivation(T* enh, bool act1, bool act2) {
    if constexpr (requires { enh->m_hasBeenActivated; })   enh->m_hasBeenActivated   = act1;
    if constexpr (requires { enh->m_hasBeenActivatedP2; }) enh->m_hasBeenActivatedP2 = act2;
}

ObjectStateSnapshot ObjectStateSnapshot::capture(GameObject* o) {
    ObjectStateSnapshot s;
    if (!o) return s;
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
        readEnhancedActivation(enh, s.activatedP1, s.activatedP2);
    }
    return s;
}

void ObjectStateSnapshot::restore() const {
    if (!obj) return;

    // Nếu Orb/Pad/Portal chưa được kích hoạt tại frame gốc nhưng bị kích hoạt ở nhánh thử nghiệm,
    // gọi resetObject() để trả lại trạng thái nguyên vẹn (nhân vật bay tới Orb vẫn nhảy bình thường)
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
            writeEnhancedActivation(enh, activatedP1, activatedP2);
        }
    }
}

template <typename P>
static void captureExtraPlayerFields(P* p, SilicatePlayerState& s) {
    if constexpr (requires { p->m_lastPosition; })        s.lastPos         = p->m_lastPosition;
    if constexpr (requires { p->m_lastGroundedPos; })     s.lastGroundedPos = p->m_lastGroundedPos;
    if constexpr (requires { p->m_rotationSpeed; })       s.rotationSpeed   = p->m_rotationSpeed;
    if constexpr (requires { p->m_fallSpeed; })           s.fallSpeed       = p->m_fallSpeed;
    if constexpr (requires { p->m_isOnGround2; })         s.onGround2       = p->m_isOnGround2;
    if constexpr (requires { p->m_isOnGround3; })         s.onGround3       = p->m_isOnGround3;
    if constexpr (requires { p->m_isOnGround4; })         s.onGround4       = p->m_isOnGround4;
    if constexpr (requires { p->m_isOnSlope; })           s.isOnSlope       = p->m_isOnSlope;
    if constexpr (requires { p->m_wasOnSlope; })          s.wasOnSlope      = p->m_wasOnSlope;
    if constexpr (requires { p->m_slopeAngle; })          s.slopeAngle      = p->m_slopeAngle;
    if constexpr (requires { p->m_dashX; })               s.dashX           = p->m_dashX;
    if constexpr (requires { p->m_dashY; })               s.dashY           = p->m_dashY;
    if constexpr (requires { p->m_dashAngle; })           s.dashAngle       = p->m_dashAngle;
    if constexpr (requires { p->m_dashStartTime; })       s.dashStartTime   = p->m_dashStartTime;
    if constexpr (requires { p->m_ringJumpRelated; })     s.ringJumpRelated = p->m_ringJumpRelated;
    if constexpr (requires { p->m_stateRingJump; })       s.stateRingJump   = p->m_stateRingJump;
    if constexpr (requires { p->m_jumpBuffered; })        s.jumpBuffered    = p->m_jumpBuffered;
    if constexpr (requires { p->m_wasRobotJump; })        s.wasRobotJump    = p->m_wasRobotJump;
    if constexpr (requires { p->m_holdingButtons[1]; }) {
        auto it = p->m_holdingButtons.find(1);
        if (it != p->m_holdingButtons.end()) s.holdingBtn1 = it->second;
    }
}

template <typename P>
static void restoreExtraPlayerFields(P* p, const SilicatePlayerState& s) {
    if constexpr (requires { p->m_lastPosition; })        p->m_lastPosition    = s.lastPos;
    if constexpr (requires { p->m_lastGroundedPos; })     p->m_lastGroundedPos = s.lastGroundedPos;
    if constexpr (requires { p->m_rotationSpeed; })       p->m_rotationSpeed   = s.rotationSpeed;
    if constexpr (requires { p->m_fallSpeed; })           p->m_fallSpeed       = s.fallSpeed;
    if constexpr (requires { p->m_isOnGround2; })         p->m_isOnGround2     = s.onGround2;
    if constexpr (requires { p->m_isOnGround3; })         p->m_isOnGround3     = s.onGround3;
    if constexpr (requires { p->m_isOnGround4; })         p->m_isOnGround4     = s.onGround4;
    if constexpr (requires { p->m_isOnSlope; })           p->m_isOnSlope       = s.isOnSlope;
    if constexpr (requires { p->m_wasOnSlope; })          p->m_wasOnSlope      = s.wasOnSlope;
    if constexpr (requires { p->m_slopeAngle; })          p->m_slopeAngle      = s.slopeAngle;
    if constexpr (requires { p->m_dashX; })               p->m_dashX           = s.dashX;
    if constexpr (requires { p->m_dashY; })               p->m_dashY           = s.dashY;
    if constexpr (requires { p->m_dashAngle; })           p->m_dashAngle       = s.dashAngle;
    if constexpr (requires { p->m_dashStartTime; })       p->m_dashStartTime   = s.dashStartTime;
    if constexpr (requires { p->m_ringJumpRelated; })     p->m_ringJumpRelated = s.ringJumpRelated;
    if constexpr (requires { p->m_stateRingJump; })       p->m_stateRingJump   = s.stateRingJump;
    if constexpr (requires { p->m_jumpBuffered; })        p->m_jumpBuffered    = s.jumpBuffered;
    if constexpr (requires { p->m_wasRobotJump; })        p->m_wasRobotJump    = s.wasRobotJump;
    if constexpr (requires { p->m_holdingButtons[1]; })   p->m_holdingButtons[1] = s.holdingBtn1;

    if (!s.holdingBtn1) {
        if constexpr (requires { p->m_touchedRings.clear(); }) {
            p->m_touchedRings.clear();
        } else if constexpr (requires { p->m_touchedRings->removeAllObjects(); }) {
            if (p->m_touchedRings) p->m_touchedRings->removeAllObjects();
        }
    }
}

void SilicatePlayerState::captureFrom(PlayerObject* p) {
    if (!p) return;
    valid       = true;
    pos         = p->getPosition();
    rotation    = p->getRotation();
    yVel        = p->m_yVelocity;
    platXVel    = p->m_platformerXVelocity;
    gravity     = p->m_gravity;
    gravityMod  = p->m_gravityMod;
    speed       = p->m_playerSpeed;
    vehicleSize = p->m_vehicleSize;

    onGround    = p->m_isOnGround;
    upsideDown  = p->m_isUpsideDown;
    goingLeft   = p->m_isGoingLeft;
    sideways    = p->m_isSideways;
    dashing     = p->m_isDashing;

    isShip      = p->m_isShip;
    isUfo       = p->m_isBird;
    isBall      = p->m_isBall;
    isWave      = p->m_isDart;
    isRobot     = p->m_isRobot;
    isSpider    = p->m_isSpider;
    isSwing     = p->m_isSwing;

    captureExtraPlayerFields(p, *this);
}

void SilicatePlayerState::restoreTo(PlayerObject* p) const {
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

    restoreExtraPlayerFields(p, *this);
}

template <typename CkptT>
static void hidePhysicalCheckpointSprite(CkptT* ckpt) {
    if constexpr (requires { ckpt->m_physicalCheckpointObject; }) {
        if (ckpt->m_physicalCheckpointObject) {
            ckpt->m_physicalCheckpointObject->setVisible(false);
            ckpt->m_physicalCheckpointObject->removeFromParent();
        }
    }
}

FullPlayLayerSnapshot FullPlayLayerSnapshot::capture(
    PlayLayer* pl,
    int curFrame,
    size_t mIdx,
    bool hold1,
    bool hold2,
    bool createNativeCkpt
) {
    FullPlayLayerSnapshot s;
    if (!pl || !pl->m_player1) return s;

    s.valid      = true;
    s.frame      = curFrame;
    s.macroIndex = mIdx;
    s.holdingP1  = hold1;
    s.holdingP2  = hold2;

    if (createNativeCkpt) {
        s.engineCkpt = pl->createCheckpoint();
        if (s.engineCkpt) {
            hidePhysicalCheckpointSprite(s.engineCkpt.data());
        }
    }

    s.p1State.captureFrom(pl->m_player1);
    s.p2State.captureFrom(pl->m_player2);

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

void FullPlayLayerSnapshot::restore(PlayLayer* pl) const {
    if (!valid || !pl) return;

    if (engineCkpt) {
        pl->loadFromCheckpoint(engineCkpt);
    }

    for (const auto& entry : activeObjects) {
        entry.restore();
    }

    p1State.restoreTo(pl->m_player1);
    p2State.restoreTo(pl->m_player2);

    pl->handleButton(holdingP1, 1, true);
    if (pl->m_player2) {
        pl->handleButton(holdingP2, 1, false);
    }
}
