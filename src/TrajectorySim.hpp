#pragma once
#include "MacroTypes.hpp"
#include "StateSnapshot.hpp"

namespace TrajectorySimulator {
    // Khởi tạo Dummy PlayerObject ẩn trong PlayLayer (theo thuật toán ShowTrajectory của Eclipse/Maxnut)
    PlayerObject* createDummyPlayer(PlayLayer* pl);

    // Vẽ đường dự đoán Ahead Trajectory (Hold & Release) bằng chính hàm vật lý của GD trên Dummy PlayerObject
    void drawAheadTrajectoryWithDummy(
        PlayLayer* pl,
        PlayerObject* dummy,
        CCDrawNode* drawer,
        int targetFps,
        bool& isSimulatingFlag,
        bool& simDeadFlag
    );

    // Chạy thử 1 nhánh độ lệch frame (delta) trong quá trình Quét cuốn chiếu (Rolling Scan)
    TrajectoryBranch simulateShiftedBranch(
        PlayLayer* pl,
        const FullPlayLayerSnapshot& baseSnapshot,
        const std::vector<MacroAction>& actions,
        size_t actionIdx,
        int delta,
        int targetFps,
        bool& simDeadFlag
    );
}
