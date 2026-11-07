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
        readEnhancedActivation(enh, s.hasBeenActivated, s.hasBeenActivatedP2);
    }
    return s;
}

void ObjectStateSnapshot::restore() const {
    if (!obj) return;
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

    if (auto enh = typeinfo_cast<EnhancedGameObject*>(obj)) {
        writeEnhancedActivation(enh, hasBeenActivated, hasBeenActivatedP2);
    }
}

void GameStateSnapshot::captureFrom(PlayerObject* p) {
    if (!p) return;

    // 1. Vị trí X/Y & lịch sử vị trí khung hình trước
    pos             = p->m_position;
    prevPos         = p->m_previousPosition;
    realPos         = p->getPosition();
    lastPos         = p->m_lastPosition;

    // 2. Vận tốc m_yVelocity, m_xVelocity & tốc độ di chuyển ngang
    m_yVelocity     = afc::detail::getPlayerYVelocity(p);
    m_xVelocity     = afc::detail::getPlayerXVelocity(p);
    if constexpr (requires { p->m_playerSpeed; }) {
        playerSpeed = p->m_playerSpeed;
    }
    rotation        = p->getRotation();
    if constexpr (requires { p->m_rotationSpeed; }) {
        rotationSpeed = p->m_rotationSpeed;
    }

    // 3. Trọng lực, kích thước (Mini/Big) & trạng thái Ngửa/Sấp (m_isUpsideDown)
    if constexpr (requires { p->m_gravity; }) {
        gravity = p->m_gravity;
    }
    if constexpr (requires { p->m_gravityMod; }) {
        gravityMod = p->m_gravityMod;
    }
    if constexpr (requires { p->m_vehicleSize; }) {
        vehicleSize = p->m_vehicleSize;
    }
    spriteScaleX    = p->getScaleX();
    spriteScaleY    = p->getScaleY();
    m_isUpsideDown  = p->m_isUpsideDown;

    // 4. Trạng thái sống/chết & tiếp đất
    m_isDead        = p->m_isDead;
    isGrounded      = p->m_isGrounded;
    isJumping       = p->m_isJumping;
    if constexpr (requires { p->m_isHolding; }) {
        isHolding   = p->m_isHolding;
    }
    isSliding       = p->m_isSliding;
    isDashing       = p->m_isDashing;
    if constexpr (requires { p->m_fallSpeed; }) {
        fallSpeed   = p->m_fallSpeed;
    }

    // 5. Trạng thái Slope
    isOnSlope       = p->m_isOnSlope;
    if constexpr (requires { p->m_wasOnSlope; }) {
        wasOnSlope  = p->m_wasOnSlope;
    }
    slopeAngle      = p->m_slopeAngle;
    if constexpr (requires { p->m_slopeStartTime; }) {
        slopeStartTime = p->m_slopeStartTime;
    }

    // 6. Trạng thái Dash Orb & Ring Jump
    dashing         = p->m_dashing;
    dashX           = p->m_dashX;
    dashY           = p->m_dashY;
    dashAngle       = p->m_dashAngle;
    dashStartTime   = p->m_dashStartTime;
    if constexpr (requires { p->m_ringJumpRelated; }) {
        ringJumpRelated = p->m_ringJumpRelated;
    }
    if constexpr (requires { p->m_stateRingJump; }) {
        stateRingJump = p->m_stateRingJump;
    }

    // 7. Trạng thái Input Buffer & Robot Jump
    jumpBuffered    = p->m_jumpBuffered;
    wasRobotJump    = p->m_wasRobotJump;
    if constexpr (requires { p->m_robotJumpTime; }) {
        robotJumpTime = p->m_robotJumpTime;
    }
    if constexpr (requires { p->m_holdingRight; }) {
        holdingRight = p->m_holdingRight;
    }
    if constexpr (requires { p->m_holdingLeft; }) {
        holdingLeft  = p->m_holdingLeft;
    }

    // 8. Hướng di chuyển & Gamemode
    goingLeft       = p->m_isGoingLeft;
    sideways        = p->m_isSideways;
    isShip          = p->m_isShip;
    isUfo           = p->m_isBird;
    isBall          = p->m_isBall;
    isWave          = p->m_isDart;
    isRobot         = p->m_isRobot;
    isSpider        = p->m_isSpider;
    isSwing         = p->m_isSwing;
}

void GameStateSnapshot::restoreTo(PlayerObject* p) const {
    if (!p) return;

    // 1. Khôi phục vị trí X/Y
    p->m_position         = pos;
    p->m_previousPosition = prevPos;
    p->setPosition(realPos);
    p->m_lastPosition     = lastPos;

    // 2. Khôi phục vận tốc m_yVelocity, m_xVelocity & tốc độ
    afc::detail::setPlayerYVelocity(p, m_yVelocity);
    afc::detail::setPlayerXVelocity(p, m_xVelocity);
    if constexpr (requires { p->m_playerSpeed; }) {
        p->m_playerSpeed = playerSpeed;
    }
    p->setRotation(rotation);
    if constexpr (requires { p->m_rotationSpeed; }) {
        p->m_rotationSpeed = rotationSpeed;
    }

    // 3. Khôi phục trọng lực, kích thước & trạng thái Ngửa/Sấp (m_isUpsideDown)
    if constexpr (requires { p->m_gravity; }) {
        p->m_gravity = gravity;
    }
    if constexpr (requires { p->m_gravityMod; }) {
        p->m_gravityMod = gravityMod;
    }
    if constexpr (requires { p->m_vehicleSize; }) {
        p->m_vehicleSize = vehicleSize;
    }
    p->setScaleX(spriteScaleX);
    p->setScaleY(spriteScaleY);
    p->m_isUpsideDown     = m_isUpsideDown;
    p->flipGravity(m_isUpsideDown, true);

    // 4. Khôi phục cờ sống/chết & tiếp đất
    p->m_isDead           = m_isDead;
    p->m_isGrounded       = isGrounded;
    p->m_isJumping        = isJumping;
    if constexpr (requires { p->m_isHolding; }) {
        p->m_isHolding    = isHolding;
    }
    p->m_isSliding        = isSliding;
    p->m_isDashing        = isDashing;
    if constexpr (requires { p->m_fallSpeed; }) {
        p->m_fallSpeed    = fallSpeed;
    }

    // 5. Khôi phục trạng thái Slope
    p->m_isOnSlope        = isOnSlope;
    if constexpr (requires { p->m_wasOnSlope; }) {
        p->m_wasOnSlope   = wasOnSlope;
    }
    p->m_slopeAngle       = slopeAngle;
    if constexpr (requires { p->m_slopeStartTime; }) {
        p->m_slopeStartTime = slopeStartTime;
    }

    // 6. Khôi phục Dash Orb & Ring Jump
    p->m_dashing          = dashing;
    p->m_dashX            = dashX;
    p->m_dashY            = dashY;
    p->m_dashAngle        = dashAngle;
    p->m_dashStartTime    = dashStartTime;
    if constexpr (requires { p->m_ringJumpRelated; }) {
        p->m_ringJumpRelated = ringJumpRelated;
    }
    if constexpr (requires { p->m_stateRingJump; }) {
        p->m_stateRingJump = stateRingJump;
    }

    // 7. Khôi phục Input Buffer & Robot Jump
    p->m_jumpBuffered     = jumpBuffered;
    p->m_wasRobotJump     = wasRobotJump;
    if constexpr (requires { p->m_robotJumpTime; }) {
        p->m_robotJumpTime = robotJumpTime;
    }
    if constexpr (requires { p->m_holdingRight; }) {
        p->m_holdingRight = holdingRight;
    }
    if constexpr (requires { p->m_holdingLeft; }) {
        p->m_holdingLeft  = holdingLeft;
    }

    // 8. Khôi phục hướng & Gamemode
    p->m_isGoingLeft      = goingLeft;
    p->m_isSideways       = sideways;
    p->m_isShip           = isShip;
    p->m_isBird           = isUfo;
    p->m_isBall           = isBall;
    p->m_isDart           = isWave;
    p->m_isRobot          = isRobot;
    p->m_isSpider         = isSpider;
    p->m_isSwing          = isSwing;
}

void FullPlayLayerSnapshot::clear() {
    valid = false;
    frame = 0;
    macroIndex = 0;
    holdingP1 = false;
    holdingP2 = false;
    engineCkpt = nullptr;
    touchedObjects.clear();
}

bool FullPlayLayerSnapshot::capture(PlayLayer* pl, int exactPhysicsFrame, size_t mIdx, bool hP1, bool hP2) {
    if (!pl) return false;
    clear();

    // Lưu Checkpoint gốc của Engine kèm GJGameState
    engineCkpt = pl->createCheckpoint();
    if (!engineCkpt) return false;

    frame          = exactPhysicsFrame;
    macroIndex     = mIdx;
    holdingP1      = hP1;
    holdingP2      = hP2;
    savedGameState = pl->m_gameState;
    isDualMode     = pl->m_gameState.m_isDualMode;

    if (pl->m_player1) p1State.captureFrom(pl->m_player1);
    if (pl->m_player2) p2State.captureFrom(pl->m_player2);

    if (pl->m_objects) {
        CCARRAY_FOREACH_DATA_TYPE(pl->m_objects, GameObject*, obj) {
            if (!obj) continue;
            bool enhAct = false;
            if (auto enh = typeinfo_cast<EnhancedGameObject*>(obj)) {
                readEnhancedActivation(enh, enhAct, enhAct);
                bool a1 = false, a2 = false;
                readEnhancedActivation(enh, a1, a2);
                enhAct = a1 || a2;
            }
            if (obj->m_isDisabled || obj->m_isDisabled2 || enhAct) {
                touchedObjects.push_back(ObjectStateSnapshot::capture(obj));
            }
        }
    }

    valid = true;
    return true;
}

bool FullPlayLayerSnapshot::restore(PlayLayer* pl) const {
    if (!valid || !pl || !engineCkpt) return false;

    // 1. Nạp lại Checkpoint gốc của Engine
    pl->loadFromCheckpoint(engineCkpt);

    // 2. Ghi đè lại toàn bộ trạng thái GJGameState để tránh lệch Camera / Portal / Dual
    pl->m_gameState = savedGameState;

    // 3. Khôi phục chính xác trạng thái Player 1 & Player 2 từ GameStateSnapshot
    if (pl->m_player1) {
        p1State.restoreTo(pl->m_player1);
    }
    if (pl->m_player2) {
        p2State.restoreTo(pl->m_player2);
    }

    // 4. Khôi phục các Orbs/Pads/Triggers đã kích hoạt trước thời điểm Snapshot
    for (const auto& objSnap : touchedObjects) {
        objSnap.restore();
    }

    // 5. Đồng bộ hoá trạng thái giữ nút (Hold State) với Snapshot
    if (pl->m_player1) {
        if (holdingP1) pl->pushButton(1, true);
        else           pl->releaseButton(1, true);
        // Ghi đè lại lần nữa để pushButton/releaseButton không làm phát sinh vận tốc nhảy giả
        p1State.restoreTo(pl->m_player1);
    }
    if (pl->m_player2 && isDualMode) {
        if (holdingP2) pl->pushButton(1, false);
        else           pl->releaseButton(1, false);
        p2State.restoreTo(pl->m_player2);
    }

    return true;
}
