#include "TrajectorySim.hpp"
#include <algorithm>

bool TrajectorySim::isSimulating() noexcept {
    return s_inHeadlessSimulation;
}

bool TrajectorySim::isPlayerDead(PlayLayer* pl) noexcept {
    if (!pl) return true;
    if (pl->m_player1 && pl->m_player1->m_isDead) return true;
    if (pl->m_gameState.m_isDualMode && pl->m_player2 && pl->m_player2->m_isDead) return true;
    return false;
}

void TrajectorySim::dispatchInput(PlayLayer* pl, ActionType type, int button, bool isPlayer1) {
    if (!pl) return;
    // Tham chiếu yBot / Echo: Gọi handleButton trực tiếp trên GJBaseGameLayer
    // để đồng bộ trạng thái nút vật lý cả cho Player 1 và Player 2
    const bool isPush = (type == ActionType::Press);
    pl->handleButton(isPush, button, isPlayer1);
}

bool TrajectorySim::simulateSurviveAtFrame(
    PlayLayer* pl,
    const FullPlayLayerSnapshot& baseSnap,
    const std::vector<MacroAction>& allActions,
    size_t targetActionIdx,
    int candidateFrame,
    int surviveCheckFrames
) {
    if (!pl || !baseSnap.valid || targetActionIdx >= allActions.size()) {
        return false;
    }

    // Không thể giả lập lùi về trước thời điểm của Snapshot gốc
    if (candidateFrame < baseSnap.frame) {
        return false;
    }

    // Khoá bảo vệ chống vòng lặp vô hạn (Max Checkpoint Frames Guard)
    const int fastForwardSteps = candidateFrame - baseSnap.frame;
    if (fastForwardSteps > MAX_CHECKPOINT_FRAMES) {
        return false;
    }

    ScopedSimulationGuard simGuard;

    // 1. Khôi phục toàn diện trạng thái vật lý về Snapshot gốc
    if (!baseSnap.restore(pl)) {
        return false;
    }

    const MacroAction& targetAct = allActions[targetActionIdx];
    int simFrame = baseSnap.frame;
    const int endSimFrame = candidateFrame + std::clamp(surviveCheckFrames, 1, MAX_CHECKPOINT_FRAMES);

    // 2. Vòng lặp kín Headless Simulation: Tiến từng bước vật lý cố định (1/240s)
    int safetyCounter = 0;
    const int maxTotalIterations = MAX_CHECKPOINT_FRAMES * 2;

    while (simFrame < endSimFrame) {
        if (++safetyCounter > maxTotalIterations) {
            // Ngắt khẩn cấp nếu vượt ngưỡng an toàn để tránh treo Main Thread
            baseSnap.restore(pl);
            return false;
        }

        // 2a. Phát các input phụ (nếu có) và input đang được kiểm tra tại đúng `candidateFrame`
        for (size_t i = baseSnap.macroIndex; i < allActions.size(); ++i) {
            if (i == targetActionIdx) continue; // Bỏ qua timing gốc của action đang xét
            const auto& otherAct = allActions[i];
            if (otherAct.frame < baseSnap.frame) continue;
            if (otherAct.frame > simFrame) break;

            if (otherAct.frame == simFrame) {
                dispatchInput(pl, otherAct.type, otherAct.button, otherAct.player1);
            }
        }

        // Kích hoạt hành động mục tiêu tại đúng frame giả định `candidateFrame`
        if (simFrame == candidateFrame) {
            dispatchInput(pl, targetAct.type, targetAct.button, targetAct.player1);
        }

        // 2b. Tiến đúng 1 tick vật lý cố định (Fixed Timestep = 1/240s)
        pl->GJBaseGameLayer::update(GD_FIXED_TICK_DT);
        ++simFrame;

        // 2c. Kiểm tra điều kiện sống sót sau mỗi tick vật lý
        if (isPlayerDead(pl)) {
            baseSnap.restore(pl);
            return false;
        }
    }

    // 3. Khôi phục trả lại nguyên vẹn trạng thái thực cho luồng game chính
    baseSnap.restore(pl);
    return true;
}

FrameWindowResult TrajectorySim::analyzeActionWindow(
    PlayLayer* pl,
    const FullPlayLayerSnapshot& baseSnap,
    const std::vector<MacroAction>& allActions,
    size_t targetActionIdx,
    int maxBackward,
    int maxForward,
    int surviveCheckFrames
) {
    FrameWindowResult result{};
    if (!pl || !baseSnap.valid || targetActionIdx >= allActions.size()) {
        return result;
    }

    const int recordedFrame = allActions[targetActionIdx].frame;

    // Ràng buộc biên dưới (Prev Action) và biên trên (Next Action) để không chồng chéo input
    int minAllowedFrame = std::max(baseSnap.frame, recordedFrame - maxBackward);
    if (targetActionIdx > 0) {
        minAllowedFrame = std::max(minAllowedFrame, allActions[targetActionIdx - 1].frame);
    }

    int maxAllowedFrame = recordedFrame + maxForward;
    if (targetActionIdx + 1 < allActions.size()) {
        maxAllowedFrame = std::min(maxAllowedFrame, allActions[targetActionIdx + 1].frame);
    }

    // Đảm bảo khoảng cách từ baseSnap.frame tới maxAllowedFrame không vượt quá MAX_CHECKPOINT_FRAMES
    maxAllowedFrame = std::min(maxAllowedFrame, baseSnap.frame + MAX_CHECKPOINT_FRAMES);

    // 1. PASS 1 - QUÉT LÙI (Backward Scan): Từ (recordedFrame - 1) lùi về minAllowedFrame
    int backwardSurviveCount = 0;
    for (int testFrame = recordedFrame - 1; testFrame >= minAllowedFrame; --testFrame) {
        const bool survived = simulateSurviveAtFrame(
            pl, baseSnap, allActions, targetActionIdx, testFrame, surviveCheckFrames
        );
        if (survived) {
            ++backwardSurviveCount;
        } else {
            // Ngay khi chạm ranh giới chết ở phía sớm (Early Death Boundary), dừng quét lùi
            break;
        }
    }

    // 2. PASS 2 - QUÉT TIẾN (Forward Scan): Từ (recordedFrame + 1) tiến tới maxAllowedFrame
    int forwardSurviveCount = 0;
    for (int testFrame = recordedFrame + 1; testFrame <= maxAllowedFrame; ++testFrame) {
        const bool survived = simulateSurviveAtFrame(
            pl, baseSnap, allActions, targetActionIdx, testFrame, surviveCheckFrames
        );
        if (survived) {
            ++forwardSurviveCount;
        } else {
            // Ngay khi chạm ranh giới chết ở phía trễ (Late Death Boundary), dừng quét tiến
            break;
        }
    }

    result.backwardFrames = backwardSurviveCount;
    result.forwardFrames  = forwardSurviveCount;
    result.windowSize     = 1 + backwardSurviveCount + forwardSurviveCount;
    result.valid          = true;
    return result;
}

std::vector<TrajectoryPoint> TrajectorySim::previewTrajectory(
    PlayLayer* pl,
    const FullPlayLayerSnapshot& baseSnap,
    const std::vector<MacroAction>& allActions,
    size_t startActionIdx,
    int steps
) {
    std::vector<TrajectoryPoint> points;
    if (!pl || !baseSnap.valid || steps <= 0) {
        return points;
    }

    const int clampedSteps = std::min(steps, MAX_CHECKPOINT_FRAMES);
    points.reserve(static_cast<size_t>(clampedSteps));

    ScopedSimulationGuard simGuard;

    if (!baseSnap.restore(pl)) {
        return points;
    }

    int simFrame = baseSnap.frame;
    size_t nextIdx = startActionIdx;

    for (int i = 0; i < clampedSteps; ++i) {
        // Phát tiếp các macro action nằm trên đường dự đoán quỹ đạo
        while (nextIdx < allActions.size() && allActions[nextIdx].frame <= simFrame) {
            if (allActions[nextIdx].frame == simFrame) {
                dispatchInput(pl, allActions[nextIdx].type, allActions[nextIdx].button, allActions[nextIdx].player1);
            }
            ++nextIdx;
        }

        pl->GJBaseGameLayer::update(GD_FIXED_TICK_DT);
        ++simFrame;

        TrajectoryPoint pt;
        if (pl->m_player1) {
            pt.pos = pl->m_player1->getPosition();
            if (isPlayerDead(pl)) {
                pt.type = TrajectoryPointType::Death;
                points.push_back(pt);
                break;
            }
        }
        pt.type = TrajectoryPointType::Normal;
        points.push_back(pt);
    }

    baseSnap.restore(pl);
    return points;
}
