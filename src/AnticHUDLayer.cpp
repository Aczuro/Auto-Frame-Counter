#include "AnticHUDLayer.hpp"
#include <sstream>

AnticHUDLayer* AnticHUDLayer::create() {
    auto* ret = new AnticHUDLayer();
    if (ret && ret->init()) {
        ret->autorelease();
        return ret;
    }
    CC_SAFE_DELETE(ret);
    return nullptr;
}

cocos2d::ccColor3B AnticHUDLayer::getDifficultyColor(int feasibleFrames) noexcept {
    if (feasibleFrames <= 2) {
        // 1 - 2 frames: Đỏ tươi (Frame Perfect / Tight Window)
        return ccc3(255, 65, 65);
    }
    if (feasibleFrames <= 5) {
        // 3 - 5 frames: Vàng cam (Medium / Tight Timing)
        return ccc3(255, 215, 55);
    }
    // > 5 frames: Xanh lá (Safe / Wide Window)
    return ccc3(75, 255, 115);
}

bool AnticHUDLayer::init() {
    if (!CCLayer::init()) {
        return false;
    }

    // 1. Khung nền mờ (Semi-transparent HUD Background) ở góc màn hình
    m_bgPanel = cocos2d::extension::CCScale9Sprite::create("square02b_001.png", {0.f, 0.f, 80.f, 80.f});
    if (m_bgPanel) {
        m_bgPanel->setContentSize({210.f, 58.f});
        m_bgPanel->setColor({0, 0, 0});
        const int bgOpacity = static_cast<int>(Mod::get()->getSettingValue<int64_t>("bg-opacity"));
        m_bgPanel->setOpacity(static_cast<GLubyte>(std::clamp(bgOpacity, 0, 255)));
        this->addChild(m_bgPanel, 0);
    }

    // 2. Nhãn hiển thị loại đuôi file đang chạy (Ví dụ: "Active Replay: [.echo]")
    m_activeReplayLabel = CCLabelBMFont::create("Active Replay: [.internal]", "bigFont.fnt");
    m_activeReplayLabel->setScale(0.36f);
    this->addChild(m_activeReplayLabel, 1);

    // 3. Nhãn hiển thị Frame hiện tại / Tổng số Frame khả thi (Đổi màu động Đỏ/Vàng/Xanh)
    m_frameStatusLabel = CCLabelBMFont::create("Frame: 0 | Window: -- F", "bigFont.fnt");
    m_frameStatusLabel->setScale(0.40f);
    this->addChild(m_frameStatusLabel, 1);

    // 4. Nhãn thống kê phân bố 1F / 2F / 3F / 4F+
    m_tallySummaryLabel = CCLabelBMFont::create("1F: 0 | 2F: 0 | 3F: 0 | 4F+: 0", "chatFont.fnt");
    m_tallySummaryLabel->setScale(0.55f);
    m_tallySummaryLabel->setColor({210, 220, 235});
    this->addChild(m_tallySummaryLabel, 1);

    // 5. Nhãn bám theo toạ độ nhân vật (Display Frame Windows on Player Position)
    m_playerFollowLabel = CCLabelBMFont::create("", "bigFont.fnt");
    m_playerFollowLabel->setScale(0.35f);
    m_playerFollowLabel->setVisible(false);
    this->addChild(m_playerFollowLabel, 2);

    applyHudCornerPosition();
    return true;
}

void AnticHUDLayer::applyHudCornerPosition() {
    const auto winSize = CCDirector::sharedDirector()->getWinSize();
    const std::string posMode = Mod::get()->getSettingValue<std::string>("hud-position");
    const float userScale = static_cast<float>(Mod::get()->getSettingValue<double>("font-size"));
    const int bgOpacity = static_cast<int>(Mod::get()->getSettingValue<int64_t>("bg-opacity"));

    if (m_bgPanel) {
        m_bgPanel->setOpacity(static_cast<GLubyte>(std::clamp(bgOpacity, 0, 255)));
    }

    constexpr float marginX = 12.f;
    constexpr float marginY = 12.f;
    const float boxW = 210.f;
    const float boxH = 58.f;

    CCPoint panelCenter;
    if (posMode == "Top-Left") {
        panelCenter = ccp(marginX + boxW * 0.5f, winSize.height - marginY - boxH * 0.5f);
    } else if (posMode == "Bottom-Left") {
        panelCenter = ccp(marginX + boxW * 0.5f, marginY + boxH * 0.5f);
    } else if (posMode == "Bottom-Right") {
        panelCenter = ccp(winSize.width - marginX - boxW * 0.5f, marginY + boxH * 0.5f);
    } else {
        // Mặc định: Top-Right
        panelCenter = ccp(winSize.width - marginX - boxW * 0.5f, winSize.height - marginY - boxH * 0.5f);
    }

    if (m_bgPanel) {
        m_bgPanel->setPosition(panelCenter);
    }

    const float scaleFactor = std::clamp(userScale / 0.45f, 0.5f, 1.8f);
    if (m_activeReplayLabel) {
        m_activeReplayLabel->setScale(0.34f * scaleFactor);
        m_activeReplayLabel->setPosition(ccp(panelCenter.x, panelCenter.y + 17.f));
    }
    if (m_frameStatusLabel) {
        m_frameStatusLabel->setScale(0.38f * scaleFactor);
        m_frameStatusLabel->setPosition(ccp(panelCenter.x, panelCenter.y));
    }
    if (m_tallySummaryLabel) {
        m_tallySummaryLabel->setScale(0.55f * scaleFactor);
        m_tallySummaryLabel->setPosition(ccp(panelCenter.x, panelCenter.y - 17.f));
    }
}

void AnticHUDLayer::updateHUD(
    PlayLayer* pl,
    MacroExtension activeExt,
    int currentFrame,
    int feasibleWindowFrames,
    int backwardFrames,
    int forwardFrames,
    int totalMacroActions,
    const int windowTally[MAX_BUCKET]
) {
    if (!Mod::get()->getSettingValue<bool>("enable-mod")) {
        this->setVisible(false);
        return;
    }
    this->setVisible(true);
    applyHudCornerPosition();

    // 1. Cập nhật nhãn loại đuôi file đang chạy (Ví dụ: Active Replay: [.echo])
    if (m_activeReplayLabel) {
        std::string extBanner = fmt::format(
            "Active Replay: [{}] ({} inputs)",
            macroExtensionToString(activeExt),
            totalMacroActions
        );
        m_activeReplayLabel->setString(extBanner.c_str());
    }

    // 2. Cập nhật nhãn Frame hiện tại / Tổng số Frame khả thi & đổi màu động
    if (m_frameStatusLabel) {
        const std::string dispMode = Mod::get()->getSettingValue<std::string>("display-mode");
        std::string frameText;

        if (feasibleWindowFrames > 0) {
            if (dispMode == "Only Delta (Diff)") {
                frameText = fmt::format("F: {} | [-{}f / +{}f]", currentFrame, backwardFrames, forwardFrames);
            } else if (dispMode == "Only Total") {
                frameText = fmt::format("Frame: {} / Feasible: {}F", currentFrame, feasibleWindowFrames);
            } else {
                frameText = fmt::format(
                    "Frame: {} / {}F [-{} / +{}]",
                    currentFrame,
                    feasibleWindowFrames,
                    backwardFrames,
                    forwardFrames
                );
            }
            // Áp dụng cơ chế đổi màu chữ động: 1-2 (Đỏ), 3-5 (Vàng), >5 (Xanh lá)
            m_frameStatusLabel->setColor(getDifficultyColor(feasibleWindowFrames));
        } else {
            frameText = fmt::format("Frame: {} / Feasible: --", currentFrame);
            m_frameStatusLabel->setColor(Mod::get()->getSettingValue<ccColor3B>("text-color"));
        }

        m_frameStatusLabel->setString(frameText.c_str());
    }

    // 3. Cập nhật thống kê phân bố độ khó
    if (m_tallySummaryLabel && windowTally) {
        int totalAnalyzed = 0;
        for (int i = 1; i < MAX_BUCKET; ++i) {
            totalAnalyzed += windowTally[i];
        }
        const int fourPlus = std::max(0, totalAnalyzed - windowTally[1] - windowTally[2] - windowTally[3]);
        std::string tallyStr = fmt::format(
            "1F: {} | 2F: {} | 3F: {} | 4F+: {}",
            windowTally[1],
            windowTally[2],
            windowTally[3],
            fourPlus
        );
        m_tallySummaryLabel->setString(tallyStr.c_str());
    }

    // 4. Cập nhật nhãn bám theo vị trí nhân vật (nếu bật Checkbox "Display Frame Windows on Player Position")
    if (m_playerFollowLabel) {
        if (AdvancedFrameCounterPopup::isDisplayOnPlayerPosEnabled() &&
            pl && pl->m_player1 && pl->m_objectLayer && feasibleWindowFrames > 0) {
            // Chuyển đổi toạ độ Thế giới (m_objectLayer) sang toạ độ Màn hình (HUD Layer)
            const CCPoint worldPos = pl->m_player1->getPosition();
            const CCPoint screenPos = pl->m_objectLayer->convertToWorldSpace(worldPos);
            const CCPoint localPos = this->convertToNodeSpace(screenPos);

            m_playerFollowLabel->setVisible(true);
            m_playerFollowLabel->setPosition(ccp(localPos.x, localPos.y + 28.f));
            m_playerFollowLabel->setColor(getDifficultyColor(feasibleWindowFrames));
            m_playerFollowLabel->setString(
                fmt::format("{}F (-{}/+{})", feasibleWindowFrames, backwardFrames, forwardFrames).c_str()
            );
        } else {
            m_playerFollowLabel->setVisible(false);
        }
    }
}

void AnticHUDLayer::spawnPlayerPositionBadge(
    PlayLayer* pl,
    int feasibleWindowFrames,
    int backwardFrames,
    int forwardFrames
) {
    if (!AdvancedFrameCounterPopup::isDisplayOnPlayerPosEnabled() || !pl || !pl->m_player1 || !pl->m_objectLayer) {
        return;
    }

    auto* popBadge = CCLabelBMFont::create(
        fmt::format("{}F [-{}/+{}]", feasibleWindowFrames, backwardFrames, forwardFrames).c_str(),
        "bigFont.fnt"
    );
    if (!popBadge) return;

    popBadge->setScale(0.32f);
    popBadge->setColor(getDifficultyColor(feasibleWindowFrames));
    popBadge->setPosition(ccp(pl->m_player1->getPositionX(), pl->m_player1->getPositionY() + 24.f));
    pl->m_objectLayer->addChild(popBadge, 1005);

    // Hiệu ứng bay nhẹ lên và mờ dần theo phong cách Antic's Frame Counter
    auto* moveUp = CCMoveBy::create(0.55f, ccp(0.f, 18.f));
    auto* fadeOut = CCFadeOut::create(0.55f);
    auto* spawn = CCSpawn::createWithTwoActions(moveUp, fadeOut);
    auto* seq = CCSequence::createWithTwoActions(spawn, CCRemoveSelf::create());
    popBadge->runAction(seq);
}
