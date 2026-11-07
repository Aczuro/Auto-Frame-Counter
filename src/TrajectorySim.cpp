#include "TrajectorySim.hpp"

// Helper C++20 gọi trực tiếp hàm vật lý & va chạm gốc của GD trên Dummy PlayerObject
template <typename PL, typename P>
static void stepRealEngineForDummy(PL* pl, P* dummy, float dt, bool holding, bool firstStep) {
    float stepDt = dt * 60.0f; // GD internal 60Hz time scale

    if (firstStep) {
        if (holding) {
            if constexpr (requires { dummy->pushButton(PlayerButton::Jump); }) {
                dummy->pushButton(PlayerButton::Jump);
            }
        } else {
            if constexpr (requires { dummy->releaseButton(PlayerButton::Jump); }) {
                dummy->releaseButton(PlayerButton::Jump);
            }
        }
    }

    if constexpr (requires { dummy->m_collisionLogTop; }) {
        if (dummy->m_collisionLogTop) dummy->m_collisionLogTop->removeAllObjects();
        if (dummy->m_collisionLogBottom) dummy->m_collisionLogBottom->removeAllObjects();
        if (dummy->m_collisionLogLeft) dummy->m_collisionLogLeft->removeAllObjects();
        if (dummy->m_collisionLogRight) dummy->m_collisionLogRight->removeAllObjects();
    }

    // 1. Gọi hàm tích phân vật lý gốc của PlayerObject
    dummy->update(stepDt);

    // 2. Gọi hàm kiểm tra va chạm gốc của PlayLayer / GJBaseGameLayer (xử lý dốc, gai, tường, pad, portal)
    if constexpr (requires { pl->checkCollisions(dummy, stepDt, false); }) {
        pl->checkCollisions(dummy, stepDt, false);
    } else if constexpr (requires { static_cast<GJBaseGameLayer*>(pl)->checkCollisions(dummy, stepDt, false); }) {
        static_cast<GJBaseGameLayer*>(pl)->checkCollisions(dummy, stepDt, false);
    }

    // 3. Cập nhật trạng thái đặc biệt & góc xoay
    if constexpr (requires { dummy->updateSpecial(stepDt); }) {
        dummy->updateSpecial(stepDt);
    }
}

namespace TrajectorySimulator {

    PlayerObject* createDummyPlayer(PlayLayer* pl) {
        if (!pl || !pl->m_objectLayer) return nullptr;
        auto dummy = PlayerObject::create(1, 1, pl, pl->m_objectLayer, true);
        if (!dummy) return nullptr;
        dummy->setVisible(false);
        dummy->setOpacity(0);
        pl->m_objectLayer->addChild(dummy, -100);
        return dummy;
    }

    void drawAheadTrajectoryWithDummy(
        PlayLayer* pl,
        PlayerObject* dummy,
        CCDrawNode* drawer,
        int targetFps,
        bool& isSimulatingFlag,
        bool& simDeadFlag
    ) {
        if (!pl || !pl->m_player1 || !dummy || !drawer || pl->m_player1->m_isDead) return;

        // Sao lưu trạng thái các Orb/Pad/Portal gần nhân vật để Dummy Player không ăn mất Orb của người chơi thật
        std::vector<ObjectStateSnapshot> nearbyObjects;
        if (pl->m_objects) {
            float px = pl->m_player1->getPositionX();
            int count = pl->m_objects->count();
            nearbyObjects.reserve(128);
            for (int i = 0; i < count; i++) {
                auto obj = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
                if (!obj) continue;
                float ox = obj->getPositionX();
                if (ox >= px - 120.f && ox <= px + 1400.f) {
                    nearbyObjects.push_back(ObjectStateSnapshot::capture(obj));
                }
            }
        }

        SilicatePlayerState realP1State;
        realP1State.captureFrom(pl->m_player1);

        float dt = 1.0f / static_cast<float>(targetFps);
        isSimulatingFlag = true;

        // Mô phỏng 2 nhánh bằng chính Dummy PlayerObject: Nhánh 0 = Nhấn giữ (Hold), Nhánh 1 = Nhả (Release)
        for (int branchIdx = 0; branchIdx < 2; branchIdx++) {
            bool holdBranch = (branchIdx == 0);

            realP1State.restoreTo(dummy);
            dummy->setVisible(false);
            simDeadFlag = false;

            CCPoint prevPos = dummy->getPosition();
            ccColor4F lineCol = holdBranch
                ? ccColor4F{0.20f, 0.90f, 1.0f, 0.78f}
                : ccColor4F{1.0f, 0.55f, 0.20f, 0.68f};

            for (int step = 0; step < PREVIEW_STEPS; step++) {
                stepRealEngineForDummy(pl, dummy, dt, holdBranch, step == 0);

                CCPoint curPos = dummy->getPosition();
                drawer->drawSegment(prevPos, curPos, 1.35f, lineCol);
                prevPos = curPos;

                if (simDeadFlag || dummy->m_isDead) {
                    drawer->drawDot(curPos, 3.6f, {1.0f, 0.12f, 0.12f, 0.95f});
                    break;
                }
            }

            // Hoàn trả trạng thái các Orb/Portal sau mỗi nhánh thử
            for (const auto& objSnap : nearbyObjects) {
                objSnap.restore();
            }
        }

        dummy->setVisible(false);
        isSimulatingFlag = false;
        simDeadFlag = false;
    }

    TrajectoryBranch simulateShiftedBranch(
        PlayLayer* pl,
        const FullPlayLayerSnapshot& baseSnapshot,
        const std::vector<MacroAction>& actions,
        size_t actionIdx,
        int delta,
        int targetFps,
        bool& simDeadFlag
    ) {
        TrajectoryBranch branch;
        branch.delta = delta;
        branch.survived = false;

        if (!pl || actionIdx >= actions.size()) return branch;

        const auto& targetAct = actions[actionIdx];
        int origFrame = targetAct.frame;
        int shiftedFrame = origFrame + delta;

        if (shiftedFrame < baseSnapshot.frame) return branch;
        if (actionIdx > 0 && shiftedFrame <= actions[actionIdx - 1].frame) return branch;
        if (targetAct.type == ActionType::Release && actionIdx + 1 < actions.size()) {
            if (shiftedFrame >= actions[actionIdx + 1].frame) return branch;
        }

        // Nếu Type 1: Press dịch sang phải chạm vào Type 2: Release kế tiếp, dịch Release theo để bảo toàn độ dài giữ phím
        int nextReleaseShiftedFrame = -1;
        if (targetAct.type == ActionType::Press && actionIdx + 1 < actions.size() &&
            actions[actionIdx + 1].type == ActionType::Release) {
            int origRelFrame = actions[actionIdx + 1].frame;
            int holdLen = std::max(1, origRelFrame - origFrame);
            if (shiftedFrame >= origRelFrame) {
                nextReleaseShiftedFrame = shiftedFrame + holdLen;
                if (actionIdx + 2 < actions.size() &&
                    nextReleaseShiftedFrame >= actions[actionIdx + 2].frame) {
                    nextReleaseShiftedFrame = actions[actionIdx + 2].frame - 1;
                }
                if (nextReleaseShiftedFrame <= shiftedFrame) return branch;
            }
        }

        // Khôi phục toàn bộ trạng thái gốc (Player + Orb/Portal + Trigger)
        baseSnapshot.restore(pl);

        int simFrame = baseSnapshot.frame;
        size_t mIdx = baseSnapshot.macroIndex;
        int targetEndFrame = origFrame + SURVIVE_CHECK_FRAMES;
        if (actionIdx + 1 < actions.size()) {
            targetEndFrame = std::max(targetEndFrame, actions[actionIdx + 1].frame + 35);
        }

        float dt = 1.0f / static_cast<float>(targetFps);
        simDeadFlag = false;
        branch.survived = true;

        while (simFrame <= targetEndFrame) {
            while (mIdx < actions.size()) {
                int effectiveFrame = actions[mIdx].frame;
                if (mIdx == actionIdx) {
                    effectiveFrame = shiftedFrame;
                } else if (mIdx == actionIdx + 1 && nextReleaseShiftedFrame != -1) {
                    effectiveFrame = nextReleaseShiftedFrame;
                }

                if (effectiveFrame > simFrame) break;
                if (effectiveFrame == simFrame) {
                    bool isDown = (actions[mIdx].type == ActionType::Press);
                    pl->handleButton(isDown, actions[mIdx].button, actions[mIdx].player1);
                }
                mIdx++;
            }

            static_cast<GJBaseGameLayer*>(pl)->processCommands(dt, false, true);

            if (pl->m_player1) {
                branch.points.push_back(pl->m_player1->getPosition());
            }

            if (simDeadFlag || (pl->m_player1 && pl->m_player1->m_isDead)) {
                branch.survived = false;
                break;
            }
            if (pl->m_hasCompletedLevel) {
                branch.survived = true;
                break;
            }

            simFrame++;
        }

        return branch;
    }

}
