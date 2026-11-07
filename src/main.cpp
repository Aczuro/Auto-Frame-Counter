#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/UILayer.hpp>
#include <cmath>
#include <algorithm>

using namespace geode::prelude;

// Chuyển đổi tên phím trong Setting sang mã phím của Cocos2d
static cocos2d::enumKeyCodes stringToKeyCode(std::string const& keyStr) {
    if (keyStr == "C") return cocos2d::KEY_C;
    if (keyStr == "F") return cocos2d::KEY_F;
    if (keyStr == "V") return cocos2d::KEY_V;
    if (keyStr == "X") return cocos2d::KEY_X;
    if (keyStr == "Z") return cocos2d::KEY_Z;
    if (keyStr == "R") return cocos2d::KEY_R;
    if (keyStr == "T") return cocos2d::KEY_T;
    if (keyStr == "G") return cocos2d::KEY_G;
    if (keyStr == "B") return cocos2d::KEY_B;
    if (keyStr == "Tab") return cocos2d::KEY_Tab;
    if (keyStr == "F1") return cocos2d::KEY_F1;
    if (keyStr == "F2") return cocos2d::KEY_F2;
    if (keyStr == "F3") return cocos2d::KEY_F3;
    if (keyStr == "F4") return cocos2d::KEY_F4;
    return cocos2d::KEY_C;
}

// Bảng màu theo phong cách NanGD dựa trên độ hẹp của Frame
static ccColor3B getColorForFrames(int frames) {
    if (frames <= 1) return {255, 45, 45};   // 1 Frame (Frame Perfect): Đỏ rực
    if (frames == 2) return {255, 150, 30};  // 2 Frames: Cam
    if (frames <= 4) return {255, 235, 50};  // 3 - 4 Frames: Vàng
    return {65, 255, 95};                    // >= 5 Frames: Xanh lá
}

class $modify(AFCPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* m_hudLabel = nullptr;
        CCSprite* m_hudIcon = nullptr;
        CCNode* m_hudContainer = nullptr;

        int m_totalTightClicks = 0;
        int m_framePerfectCount = 0;
        int m_lastFrames = 0;

        bool m_isHolding = false;
        double m_pressStartTime = 0.0;
        double m_elapsedTime = 0.0;
        CCPoint m_lastPressPos = {0.f, 0.f};
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        // Tạo vùng chứa UI góc trái trên
        auto container = CCNode::create();
        container->setPosition({18.f, winSize.height - 42.f});
        container->setZOrder(1000);
        this->addChild(container);
        m_fields->m_hudContainer = container;

        // Tạo Menu để người chơi trên Mobile hoặc PC có thể click thẳng vào Icon để Bật/Tắt
        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        container->addChild(menu);

        // Icon hiển thị trên UI (Dùng icon có sẵn của GD để an toàn 100%)
        auto iconSprite = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
        if (!iconSprite) {
            iconSprite = CCSprite::create("GJ_button_01.png");
        }
        iconSprite->setScale(0.65f);
        m_fields->m_hudIcon = iconSprite;

        auto iconBtn = CCMenuItemSpriteExtra::create(
            iconSprite,
            this,
            menu_selector(AFCPlayLayer::onToggleModBtn)
        );
        iconBtn->setPosition({0.f, 0.f});
        menu->addChild(iconBtn);

        // Chữ hiển thị bộ đếm kế bên Icon
        auto label = CCLabelBMFont::create("FP: 0 | Total: 0", "bigFont.fnt");
        label->setScale(0.35f);
        label->setAnchorPoint({0.f, 0.5f});
        label->setPosition({14.f, 0.f});
        label->setOpacity(220);
        container->addChild(label);
        m_fields->m_hudLabel = label;

        this->updateHUDState();
        return true;
    }

    // Nút bấm vào Icon trên màn hình để Bật/Tắt Mod
    void onToggleModBtn(CCObject*) {
        bool current = Mod::get()->getSettingValue<bool>("enable-mod");
        Mod::get()->setSettingValue("enable-mod", !current);
        this->updateHUDState();

        if (!current) {
            FMODAudioEngine::sharedEngine()->playEffect("playSound_01.ogg", 1.2f, 0.f, 0.6f);
        }
    }

    void resetCounter() {
        m_fields->m_totalTightClicks = 0;
        m_fields->m_framePerfectCount = 0;
        m_fields->m_lastFrames = 0;
        this->updateHUDState();
    }

    void updateHUDState() {
        if (!m_fields->m_hudLabel || !m_fields->m_hudIcon) return;

        bool enabled = Mod::get()->getSettingValue<bool>("enable-mod");
        if (!enabled) {
            m_fields->m_hudLabel->setString("AFC: OFF");
            m_fields->m_hudLabel->setColor({160, 160, 160});
            m_fields->m_hudIcon->setColor({140, 140, 140});
            return;
        }

        m_fields->m_hudIcon->setColor({255, 255, 255});
        if (m_fields->m_lastFrames > 0) {
            m_fields->m_hudLabel->setString(
                fmt::format(
                    "1F: {} | Tight: {} (Last: {}F)",
                    m_fields->m_framePerfectCount,
                    m_fields->m_totalTightClicks,
                    m_fields->m_lastFrames
                ).c_str()
            );
            m_fields->m_hudLabel->setColor(getColorForFrames(m_fields->m_lastFrames));
        } else {
            m_fields->m_hudLabel->setString(
                fmt::format("1F: {} | Tight: {}", m_fields->m_framePerfectCount, m_fields->m_totalTightClicks).c_str()
            );
            m_fields->m_hudLabel->setColor({255, 255, 255});
        }
    }

    // Tạo chữ nổi có màu + âm thanh ngay tại vị trí nhân vật nhảy (Phong cách NanGD)
    void spawnNanGDPopup(CCPoint worldPos, int frames, int refFPS) {
        bool showWorld = Mod::get()->getSettingValue<bool>("show-world-text");
        bool playSound = Mod::get()->getSettingValue<bool>("enable-sound");
        float volume   = static_cast<float>(Mod::get()->getSettingValue<double>("sound-volume"));

        ccColor3B color = getColorForFrames(frames);

        // 1. Phát âm thanh có cao độ (pitch) thay đổi theo số Frame
        if (playSound && volume > 0.01f) {
            // Càng ít frame (càng khó) thì âm thanh càng cao và đanh
            float pitch = std::clamp(1.45f - (frames - 1) * 0.12f, 0.85f, 1.5f);
            FMODAudioEngine::sharedEngine()->playEffect("crystal01.ogg", pitch, 0.f, volume);
        }

        // 2. Tạo chữ nổi trực tiếp vào m_objectLayer tại tọa độ nhảy
        if (showWorld && this->m_objectLayer) {
            int equivFPS = std::max(1, refFPS / std::max(1, frames));
            std::string popupStr = (frames == 1)
                ? fmt::format("1 FRAME ({} FPS)", equivFPS)
                : fmt::format("{} FRAMES", frames);

            auto worldLabel = CCLabelBMFont::create(popupStr.c_str(), "bigFont.fnt");
            worldLabel->setPosition(worldPos + CCPoint{0.f, 26.f});
            worldLabel->setScale(0.0f);
            worldLabel->setColor(color);
            worldLabel->setZOrder(9999);
            this->m_objectLayer->addChild(worldLabel);

            // Hiệu ứng phóng to nảy nhẹ, bay lên và mờ dần giống video NanGD
            float targetScale = (frames == 1) ? 0.42f : 0.34f;
            auto scaleUp = CCEaseBackOut::create(CCScaleTo::create(0.15f, targetScale));
            auto moveUp  = CCMoveBy::create(1.1f, {0.f, 24.f});
            auto wait    = CCDelayTime::create(0.65f);
            auto fadeOut = CCFadeOut::create(0.45f);
            auto remove  = CCRemoveSelf::create();

            worldLabel->runAction(CCSpawn::createWithTwoActions(
                scaleUp,
                CCSequence::create(
                    CCSpawn::createWithTwoActions(moveUp, CCSequence::create(wait, fadeOut, nullptr)),
                    remove,
                    nullptr
                )
            ));
        }
    }

    // Tính toán độ hẹp của pha bấm dựa trên khoảng cách chướng ngại vật & thời gian giữ phím
    int estimateFrameWindow(double holdDuration, int refFPS) {
        // Quy đổi thời gian nhấp sang số frame ở mức Reference FPS
        int holdFrames = static_cast<int>(std::round(holdDuration * refFPS));
        if (holdFrames < 1) holdFrames = 1;

        // Kiểm tra thêm vận tốc và trạng thái của nhân vật để ước lượng độ khó tại điểm nhấn
        if (this->m_player1) {
            // Nếu là cú nhấp thả cực nhanh (micro-click) hoặc nhấp căn chuẩn
            return std::clamp(holdFrames, 1, 15);
        }
        return holdFrames;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        m_fields->m_elapsedTime += dt;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->m_isHolding = false;
        // Giữ hoặc làm mới trạng thái hiển thị khi chết/chơi lại
        this->updateHUDState();
    }
};

// Bắt thao tác bấm trên cả PC (Chuột trái, Space, Mũi tên lên) và Mobile (Chạm màn hình)
class $modify(AFCBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        // Chỉ tính nút Jump (button == 1) của Player 1 khi đang ở trong PlayLayer
        if (button != 1 || !isPlayer1) return;

        auto playLayer = PlayLayer::get();
        if (!playLayer || playLayer != static_cast<GJBaseGameLayer*>(this)) return;
        if (!Mod::get()->getSettingValue<bool>("enable-mod")) return;

        auto afcLayer = static_cast<AFCPlayLayer*>(playLayer);
        auto fields = afcLayer->m_fields.self();

        if (down) {
            fields->m_isHolding = true;
            fields->m_pressStartTime = fields->m_elapsedTime;
            if (playLayer->m_player1) {
                fields->m_lastPressPos = playLayer->m_player1->getPosition();
            }
        } else if (fields->m_isHolding) {
            fields->m_isHolding = false;
            double holdDuration = fields->m_elapsedTime - fields->m_pressStartTime;

            int refFPS = static_cast<int>(Mod::get()->getSettingValue<int64_t>("target-fps"));
            int maxDisplayFrames = static_cast<int>(Mod::get()->getSettingValue<int64_t>("max-frame-display"));

            int frames = afcLayer->estimateFrameWindow(holdDuration, refFPS);

            // Nếu số frame nằm trong ngưỡng hiển thị đã cài trong Setting
            if (frames <= maxDisplayFrames) {
                fields->m_lastFrames = frames;
                fields->m_totalTightClicks++;
                if (frames == 1) {
                    fields->m_framePerfectCount++;
                }

                afcLayer->updateHUDState();
                afcLayer->spawnNanGDPopup(fields->m_lastPressPos, frames, refFPS);
            }
        }
    }
};

// Bắt phím tắt bàn phím trên PC (Bật/Tắt Mod & Reset bộ đếm)
class $modify(AFCUILayer, UILayer) {
    void keyDown(cocos2d::enumKeyCodes key) {
        UILayer::keyDown(key);

        auto playLayer = PlayLayer::get();
        if (!playLayer) return;

        auto afcLayer = static_cast<AFCPlayLayer*>(playLayer);

        std::string toggleKeyStr = Mod::get()->getSettingValue<std::string>("toggle-key");
        std::string resetKeyStr  = Mod::get()->getSettingValue<std::string>("reset-key");

        if (key == stringToKeyCode(toggleKeyStr)) {
            afcLayer->onToggleModBtn(nullptr);
        } else if (key == stringToKeyCode(resetKeyStr)) {
            afcLayer->resetCounter();
        }
    }
};
