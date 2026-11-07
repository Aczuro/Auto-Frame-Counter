#pragma once
#include "MacroTypes.hpp"
#include "StateSnapshot.hpp"
#include <vector>

using namespace geode::prelude;

// Giới hạn tối đa số frame khi tua từ Snapshot gốc đến điểm kiểm tra
// Ngăn chặn hoàn toàn nguy cơ vòng lặp vô hạn (Infinite Loop Guard)
constexpr int MAX_CHECKPOINT_FRAMES = 240;

// Bước thời gian vật lý cố định chuẩn 240Hz của Geometry Dash 2.2
constexpr float GD_FIXED_TICK_DT = 1.0f / 240.0f;

enum class TrajectoryPointType {
    Normal,
    Death
};

struct TrajectoryPoint {
    CCPoint pos;
    TrajectoryPointType type = TrajectoryPointType::Normal;
};

struct FrameWindowResult {
    int backwardFrames = 0;
    int forwardFrames = 0;
    int windowSize = 1;
    bool valid = false;
};

class TrajectorySim {
public:
    // Chặn đệ quy hook và tắt hiệu ứng chết khi đang chạy Headless Simulation
    static bool isSimulating() noexcept;

    // Kiểm tra xem nếu thực hiện hành động `targetAction` tại `candidateFrame`
    // (thay vì `targetAction.frame` gốc), nhân vật có sống sót qua `surviveCheckFrames` tiếp theo hay không.
    static bool simulateSurviveAtFrame(
        PlayLayer* pl,
        const FullPlayLayerSnapshot& baseSnap,
        const std::vector<MacroAction>& allActions,
        size_t targetActionIdx,
        int candidateFrame,
        int surviveCheckFrames = SURVIVE_CHECK_FRAMES
    );

    // Thực thi thuật toán Quét Lùi (Backward Scan) và Quét Tiến (Forward Scan) an toàn
    // xung quanh frame gốc của hành động `targetActionIdx` với giới hạn MAX_CHECKPOINT_FRAMES.
    static FrameWindowResult analyzeActionWindow(
        PlayLayer* pl,
        const FullPlayLayerSnapshot& baseSnap,
        const std::vector<MacroAction>& allActions,
        size_t targetActionIdx,
        int maxBackward = MAX_BACKWARD_FRAMES,
        int maxForward = MAX_FORWARD_FRAMES,
        int surviveCheckFrames = SURVIVE_CHECK_FRAMES
    );

    // Dự đoán quỹ đạo bay (Trajectory Preview) để hiển thị đường xanh/đỏ trên HUD
    static std::vector<TrajectoryPoint> previewTrajectory(
        PlayLayer* pl,
        const FullPlayLayerSnapshot& baseSnap,
        const std::vector<MacroAction>& allActions,
        size_t startActionIdx,
        int steps = PREVIEW_STEPS
    );

private:
    static inline bool s_inHeadlessSimulation = false;

    struct ScopedSimulationGuard {
        ScopedSimulationGuard() noexcept  { TrajectorySim::s_inHeadlessSimulation = true; }
        ~ScopedSimulationGuard() noexcept { TrajectorySim::s_inHeadlessSimulation = false; }
    };

    static bool isPlayerDead(PlayLayer* pl) noexcept;
    static void dispatchInput(PlayLayer* pl, ActionType type, int button, bool isPlayer1);
};
