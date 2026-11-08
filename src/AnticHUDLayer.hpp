#pragma once
#include "MacroTypes.hpp"
#include "AdvancedFrameCounterPopup.hpp"
#include <Geode/Geode.hpp>
#include <deque>
#include <vector>

using namespace geode::prelude;

// ============================================================================
// GIAO DIỆN HUD HIỂN THỊ TRONG MÀN CHƠI (ANTIC-STYLE OVERLAY LAYER)
// Kế thừa trực tiếp từ cocos2d::CCLayer để chèn vào PlayLayer
// ============================================================================
class AnticHUDLayer : public cocos2d::CCLayer {
public:
    static AnticHUDLayer* create();

    bool init() override;

    // Cập nhật thông số hiển thị thời gian thực trên góc màn hình & tại toạ độ nhân vật
    void updateHUD(
        PlayLayer* pl,
        MacroExtension activeExt,
        int currentFrame,
        int feasibleWindowFrames,
        int backwardFrames,
        int forwardFrames,
        int totalMacroActions,
        const int windowTally[MAX_BUCKET]
    );

    // Tạo nhãn nổi (Floating Frame Badge) ngay tại vị trí nhân vật khi vừa thực hiện Click/Release
    void spawnPlayerPositionBadge(PlayLayer* pl, int feasibleWindowFrames, int backwardFrames, int forwardFrames);

    // Bảng màu động chuẩn Antic's Frame Counter theo độ khó của Frame Window:
    // - 1 đến 2 frames: Đỏ (Extreme / Frame Perfect)
    // - 3 đến 5 frames: Vàng (Hard / Medium Window)
    // - > 5 frames    : Xanh lá (Lenient / Safe Window)
    static cocos2d::ccColor3B getDifficultyColor(int feasibleFrames) noexcept;

private:
    void applyHudCornerPosition();

private:
    cocos2d::extension::CCScale9Sprite* m_bgPanel = nullptr;
    CCLabelBMFont* m_activeReplayLabel = nullptr;
    CCLabelBMFont* m_frameStatusLabel  = nullptr;
    CCLabelBMFont* m_tallySummaryLabel = nullptr;
    CCLabelBMFont* m_playerFollowLabel = nullptr;
};
