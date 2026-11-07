#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/UILayer.hpp>
#include <vector>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <cctype>

using namespace geode::prelude;

struct FramePointData {
    float x;
    float y;
    int frames;
    bool isClick;
    bool triggered;
};

// Bộ chuyển đổi Custom Key: Nhận diện mọi phím người dùng tự nhập (không phân biệt hoa/thường)
static cocos2d::enumKeyCodes stringToKeyCode(std::string rawStr) {
    std::string s;
    for (unsigned char c : rawStr) {
        if (!std::isspace(c)) {
            s += static_cast<char>(std::toupper(c));
        }
    }
    if (s.empty()) return cocos2d::KEY_None;

    // 1. Một ký tự đơn: Chữ cái A-Z hoặc Số 0-9
    if (s.size() == 1) {
        char c = s[0];
        if (c >= 'A' && c <= 'Z') {
            return static_cast<cocos2d::enumKeyCodes>(cocos2d::KEY_A + (c - 'A'));
        }
        if (c >= '0' && c <= '9') {
            return static_cast<cocos2d::enumKeyCodes>(cocos2d::KEY_Zero + (c - '0'));
        }
    }

    // 2. Phím chức năng F1 -> F12
    if (s == "F1")  return cocos2d::KEY_F1;
    if (s == "F2")  return cocos2d::KEY_F2;
    if (s == "F3")  return cocos2d::KEY_F3;
    if (s == "F4")  return cocos2d::KEY_F4;
    if (s == "F5")  return cocos2d::KEY_F5;
    if (s == "F6")  return cocos2d::KEY_F6;
    if (s == "F7")  return cocos2d::KEY_F7;
    if (s == "F8")  return cocos2d::KEY_F8;
    if (s == "F9")  return cocos2d::KEY_F9;
    if (s == "F10") return cocos2d::KEY_F10;
    if (s == "F11") return cocos2d::KEY_F11;
    if (s == "F12") return cocos2d::KEY_F12;

    // 3. Các phím đặc biệt thông dụng trên PC
    if (s == "TAB") return cocos2d::KEY_Tab;
    if (s == "SPACE" || s == "SPACEBAR") return cocos2d::KEY_Space;
    if (s == "SHIFT" || s == "LSHIFT") return cocos2d::KEY_LeftShift;
    if (s == "RSHIFT") return cocos2d::KEY_RightShift;
    if (s == "CTRL" || s == "CONTROL" || s == "LCTRL") return cocos2d::KEY_LeftControl;
    if (s == "ALT" || s == "LALT") return cocos2d::KEY_LeftMenu;
    if (s == "ENTER" || s == "RETURN") return cocos2d::KEY_Enter;
    if (s == "BACKSPACE" || s == "BACK") return cocos2d::KEY_Backspace;
    if (s == "DELETE" || s == "DEL") return cocos2d::KEY_Delete;
    if (s == "INSERT" || s == "INS") return cocos2d::KEY_Insert;
    if (s == "HOME") return cocos2d::KEY_Home;
    if (s == "END") return cocos2d::KEY_End;
    if (s == "PAGEUP" || s == "PGUP") return cocos2d::KEY_PageUp;
    if (s == "PAGEDOWN" || s == "PGDN") return cocos2d::KEY_PageDown;
    if (s == "UP") return cocos2d::KEY_Up;
    if (s == "DOWN") return cocos2d::KEY_Down;
    if (s == "LEFT") return cocos2d::KEY_Left;
    if (s == "RIGHT") return cocos2d::KEY_Right;

    return cocos2d::KEY_None;
}

// 7 bậc hiển thị của bảng NAN COUNTER
static int getTierIndex(int frames) {
    if (frames <= 1) return 0;
    if (frames == 2) return 1;
    if (frames == 3) return 2;
    if (frames == 4) return 3;
    if (frames <= 6) return 4;
    if (frames <= 8) return 5;
    return 6;
}

static ccColor3B getTierColor(int tierIdx) {
    switch (tierIdx) {
        case 0: return {255, 65, 55};    // 1: Đỏ
        case 1: return {255, 165, 45};   // 2: Cam
        case 2: return {255, 240, 75};   // 3: Vàng
        case 3: return {255, 255, 255};  // 4: Trắng
        case 4: return {115, 242, 130};  // 5-6: Xanh lá
        case 5: return {100, 195, 255};  // 7-8: Xanh dương nhạt
        default: return {125, 140, 255}; // 9-12: Tím xanh
    }
}

static ccColor4F toColor4F(ccColor3B c, float alpha = 1.0f) {
    return {c.r / 255.f, c.g / 255.f, c.b / 255.f, alpha};
}

static void drawNanGDRing(CCDrawNode* drawer, CCPoint center, float radius, ccColor3B color) {
    if (!drawer) return;
    constexpr int segments = 28;
    constexpr float twoPi = 6.2831853f;

    ccColor4F blackBorder = {0.f, 0.f, 0.f, 0.95f};
    ccColor4F innerColor  = toColor4F(color, 1.0f);

    for (int i = 0; i < segments; ++i) {
        float a1 = (i * twoPi) / segments;
        float a2 = ((i + 1) * twoPi) / segments;
        CCPoint p1 = ccp(center.x + std::cos(a1) * radius, center.y + std::sin(a1) * radius);
        CCPoint p2 = ccp(center.x + std::cos(a2) * radius, center.y + std::sin(a2) * radius);
        drawer->drawSegment(p1, p2, 3.8f, blackBorder);
    }

    for (int i = 0; i < segments; ++i) {
        float a1 = (i * twoPi) / segments;
        float a2 = ((i + 1) * twoPi) / segments;
        CCPoint p1 = ccp(center.x + std::cos(a1) * radius, center.y + std::sin(a1) * radius);
        CCPoint p2 = ccp(center.x + std::cos(a2) * radius, center.y + std::sin(a2) * radius);
        drawer->drawSegment(p1, p2, 2.5f, innerColor);
    }
}

class $modify(AFCPlayLayer, PlayLayer) {
    struct Fields {
        CCNode* m_leftBoard = nullptr;
        CCLabelBMFont* m_watermark = nullptr;
        CCSprite* m_logoSprite = nullptr;
        CCLabelBMFont* m_tierTitleLabels[7] = {nullptr};
        CCLabelBMFont* m_tierValueLabels[7] = {nullptr};
        CCLabelBMFont* m_tierGlowLabels[7]  = {nullptr};
        int m_tierCounts[7] = {0, 0, 0, 0, 0, 0, 0};

        CCDrawNode* m_trailDrawer = nullptr;
        CCDrawNode* m_projDrawer = nullptr;
        CCDrawNode* m_ringDrawer = nullptr;
        CCNode* m_worldTextContainer = nullptr;

        CCPoint m_prevPos = {0.f, 0.f};
        CCPoint m_vel = {311.58f, 0.f};
        float m_gravity = -2600.f;
        bool m_hasPrevPos = false;
        bool m_isHolding = false;
        double m_pressStartTime = 0.0;
        double m_elapsedTime = 0.0;
        CCPoint m_pressPos = {0.f, 0.f};
        CCPoint m_pressVel = {311.58f, 0.f};

        std::vector<CCPoint> m_trailPoints;
        std::vector<CCRect> m_nearbyHazards;
        int m_hazardUpdateTick = 0;
        std::vector<FramePointData> m_savedPoints;
    };

    std::filesystem::path getLevelMacroFilePath() {
        int levelID = 0;
        std::string levelName = "custom";
        if (this->m_level) {
            levelID = static_cast<int>(this->m_level->m_levelID);
            levelName = std::string(this->m_level->m_levelName);
            levelName.erase(std::remove_if(levelName.begin(), levelName.end(), [](unsigned char c) {
                return !std::isalnum(c);
            }), levelName.end());
        }
        if (levelName.empty()) levelName = "level";
        return Mod::get()->getSaveDir() / fmt::format("macro_{}_{}.afc", levelID, levelName);
    }

    void loadSavedMacroFrames() {
        m_fields->m_savedPoints.clear();
        auto path = this->getLevelMacroFilePath();
        std::ifstream in(path);
        if (!in.is_open()) return;

        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            FramePointData pt{};
            int clickInt = 1;
            if (ss >> pt.x >> pt.y >> pt.frames >> clickInt) {
                pt.isClick = (clickInt != 0);
                pt.triggered = false;
                m_fields->m_savedPoints.push_back(pt);
            }
        }
    }

    void saveMacroFramesToDisk() {
        auto path = this->getLevelMacroFilePath();
        std::ofstream out(path, std::ios::trunc);
        if (!out.is_open()) return;

        for (auto const& pt : m_fields->m_savedPoints) {
            out << pt.x << " " << pt.y << " " << pt.frames << " " << (pt.isClick ? 1 : 0) << "\n";
        }
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        // 1. Bảng NAN COUNTER bên trái
        auto board = CCNode::create();
        board->setPosition(ccp(0.f, 0.f));
        board->setZOrder(1000);
        this->addChild(board);
        m_fields->m_leftBoard = board;

        static const char* tierNames[7] = {
            "1:", "2:", "3:", "4:", "5-6:", "7-8:", "9-12:"
        };

        float topY = winSize.height - 18.f;
        float rowStep = 24.f;

        for (int i = 0; i < 7; ++i) {
            int rowFromTop = 6 - i;
            float yPos = topY - rowFromTop * rowStep;
            ccColor3B col = getTierColor(i);

            auto titleLbl = CCLabelBMFont::create(tierNames[i], "bigFont.fnt");
            titleLbl->setAnchorPoint(ccp(0.f, 0.5f));
            titleLbl->setPosition(ccp(6.f, yPos));
            titleLbl->setScale(0.52f);
            titleLbl->setColor(col);
            board->addChild(titleLbl);
            m_fields->m_tierTitleLabels[i] = titleLbl;

            auto glowLbl = CCLabelBMFont::create("0", "bigFont.fnt");
            glowLbl->setAnchorPoint(ccp(0.5f, 0.5f));
            glowLbl->setPosition(ccp(92.f, yPos));
            glowLbl->setScale(0.62f);
            glowLbl->setColor({255, 255, 255});
            glowLbl->setOpacity(0);
            board->addChild(glowLbl);
            m_fields->m_tierGlowLabels[i] = glowLbl;

            auto valLbl = CCLabelBMFont::create("0", "bigFont.fnt");
            valLbl->setAnchorPoint(ccp(0.5f, 0.5f));
            valLbl->setPosition(ccp(92.f, yPos));
            valLbl->setScale(0.52f);
            valLbl->setColor(col);
            board->addChild(valLbl);
            m_fields->m_tierValueLabels[i] = valLbl;
        }

        // 2. Nhãn NAN COUNTER kèm hiển thị Logo ở góc phải trên
        auto watermark = CCLabelBMFont::create("NAN COUNTER", "bigFont.fnt");
        watermark->setAnchorPoint(ccp(1.f, 0.5f));
        watermark->setPosition(ccp(winSize.width - 8.f, winSize.height - 34.f));
        watermark->setScale(0.40f);
        watermark->setColor({200, 200, 200});
        watermark->setOpacity(95);
        watermark->setZOrder(1000);
        this->addChild(watermark);
        m_fields->m_watermark = watermark;

        // Tự động đọc logo.png của mod (nếu đã đóng gói hợp lệ) để gắn kế bên chữ NAN COUNTER
        if (auto logoSpr = CCSprite::create("logo.png"_spr)) {
            float targetSize = 18.f;
            float maxSide = std::max(logoSpr->getContentWidth(), logoSpr->getContentHeight());
            if (maxSide > 0.f) {
                logoSpr->setScale(targetSize / maxSide);
            }
            logoSpr->setPosition(ccp(
                winSize.width - watermark->getScaledContentSize().width - 20.f,
                winSize.height - 34.f
            ));
            logoSpr->setOpacity(150);
            logoSpr->setZOrder(1000);
            this->addChild(logoSpr);
            m_fields->m_logoSprite = logoSpr;
        }

        // 3. Khởi tạo các node vẽ trong m_objectLayer
        if (this->m_objectLayer) {
            auto trailDrawer = CCDrawNode::create();
            trailDrawer->setZOrder(9990);
            this->m_objectLayer->addChild(trailDrawer);
            m_fields->m_trailDrawer = trailDrawer;

            auto projDrawer = CCDrawNode::create();
            projDrawer->setZOrder(9991);
            this->m_objectLayer->addChild(projDrawer);
            m_fields->m_projDrawer = projDrawer;

            auto ringDrawer = CCDrawNode::create();
            ringDrawer->setZOrder(9995);
            this->m_objectLayer->addChild(ringDrawer);
            m_fields->m_ringDrawer = ringDrawer;

            auto textContainer = CCNode::create();
            textContainer->setPosition(ccp(0.f, 0.f));
            textContainer->setZOrder(9996);
            this->m_objectLayer->addChild(textContainer);
            m_fields->m_worldTextContainer = textContainer;
        }

        this->loadSavedMacroFrames();
        this->refreshBoardUI();

        return true;
    }

    void refreshBoardUI() {
        bool enabled = Mod::get()->getSettingValue<bool>("enable-mod");
        if (m_fields->m_leftBoard) m_fields->m_leftBoard->setVisible(enabled);
        if (m_fields->m_watermark) m_fields->m_watermark->setVisible(enabled);
        if (m_fields->m_logoSprite) m_fields->m_logoSprite->setVisible(enabled);
        if (m_fields->m_trailDrawer) m_fields->m_trailDrawer->setVisible(enabled);
        if (m_fields->m_projDrawer) m_fields->m_projDrawer->setVisible(enabled);
        if (m_fields->m_ringDrawer) m_fields->m_ringDrawer->setVisible(enabled);
        if (m_fields->m_worldTextContainer) m_fields->m_worldTextContainer->setVisible(enabled);

        for (int i = 0; i < 7; ++i) {
            if (!m_fields->m_tierValueLabels[i]) continue;
            int cnt = m_fields->m_tierCounts[i];
            m_fields->m_tierValueLabels[i]->setString(fmt::format("{}", cnt).c_str());

            if (cnt == 0) {
                m_fields->m_tierValueLabels[i]->setColor(getTierColor(i));
                if (m_fields->m_tierGlowLabels[i]) {
                    m_fields->m_tierGlowLabels[i]->setOpacity(0);
                }
            }
        }
    }

    void incrementTierCount(int frames) {
        int idx = getTierIndex(frames);
        m_fields->m_tierCounts[idx]++;

        auto valLbl  = m_fields->m_tierValueLabels[idx];
        auto glowLbl = m_fields->m_tierGlowLabels[idx];
        if (!valLbl || !glowLbl) return;

        std::string numStr = fmt::format("{}", m_fields->m_tierCounts[idx]);
        valLbl->setString(numStr.c_str());
        glowLbl->setString(numStr.c_str());

        valLbl->setColor({255, 235, 235});
        valLbl->stopAllActions();
        valLbl->setScale(0.64f);
        valLbl->runAction(CCEaseBackOut::create(CCScaleTo::create(0.18f, 0.52f)));

        glowLbl->stopAllActions();
        glowLbl->setOpacity(175);
        glowLbl->setScale(0.76f);
        glowLbl->runAction(CCSpawn::createWithTwoActions(
            CCScaleTo::create(0.35f, 0.62f),
            CCFadeTo::create(0.35f, 95)
        ));

        if (Mod::get()->getSettingValue<bool>("enable-sound")) {
            float vol = static_cast<float>(Mod::get()->getSettingValue<double>("sound-volume"));
            if (vol > 0.01f) {
                float pitch = std::clamp(1.45f - (frames - 1) * 0.08f, 0.85f, 1.45f);
                FMODAudioEngine::sharedEngine()->playEffect("crystal01.ogg", pitch, 0.f, vol);
            }
        }
    }

    void spawnWorldMarker(CCPoint pos, int frames) {
        if (!m_fields->m_ringDrawer || !m_fields->m_worldTextContainer) return;

        int tierIdx = getTierIndex(frames);
        ccColor3B color = getTierColor(tierIdx);

        drawNanGDRing(m_fields->m_ringDrawer, pos, 11.5f, color);

        auto numLbl = CCLabelBMFont::create(fmt::format("{}", frames).c_str(), "bigFont.fnt");
        numLbl->setAnchorPoint(ccp(1.f, 0.5f));
        numLbl->setPosition(ccp(pos.x - 15.f, pos.y));
        numLbl->setScale(0.38f);
        numLbl->setColor(color);
        m_fields->m_worldTextContainer->addChild(numLbl);
    }

    void updateNearbyHazards(CCPoint playerPos) {
        m_fields->m_nearbyHazards.clear();
        if (!this->m_objects) return;

        unsigned int count = this->m_objects->count();
        for (unsigned int i = 0; i < count; ++i) {
            auto node = static_cast<CCNode*>(this->m_objects->objectAtIndex(i));
            if (!node) continue;

            float ox = node->getPositionX();
            float oy = node->getPositionY();

            if (ox < playerPos.x - 70.f || ox > playerPos.x + 360.f) continue;
            if (std::abs(oy - playerPos.y) > 260.f) continue;

            CCRect box = node->boundingBox();
            if (box.size.width < 4.f || box.size.height < 4.f) continue;
            if (box.size.width > 120.f || box.size.height > 120.f) continue;

            CCRect hitRect(
                box.origin.x + box.size.width * 0.2f,
                box.origin.y + box.size.height * 0.2f,
                box.size.width * 0.6f,
                box.size.height * 0.6f
            );
            m_fields->m_nearbyHazards.push_back(hitRect);
        }
    }

    bool isTrajectoryPointColliding(CCPoint pt) {
        if (pt.y < 85.f || pt.y > 2400.f) return true;

        CCRect playerHitbox(pt.x - 8.f, pt.y - 8.f, 16.f, 16.f);
        for (auto const& hz : m_fields->m_nearbyHazards) {
            if (playerHitbox.intersectsRect(hz)) {
                return true;
            }
        }
        return false;
    }

    int analyzeTrajectoryFrameWindow(CCPoint actionPos, CCPoint actionVel, bool isClick, double holdDuration, int refFPS) {
        this->updateNearbyHazards(actionPos);

        float dtStep = 1.0f / static_cast<float>(std::max(60, refFPS));
        float vx = std::max(220.f, actionVel.x);
        float grav = m_fields->m_gravity;

        float postActionVy = isClick ? 540.f : std::min(actionVel.y, 120.f);
        float preActionVy  = actionVel.y;

        int survivingOffsets = 0;

        for (int delta = -12; delta <= 12; ++delta) {
            if (delta == 0) {
                survivingOffsets++;
                continue;
            }

            bool alive = true;
            float shiftTime = delta * dtStep;

            CCPoint shiftedStart(
                actionPos.x + vx * shiftTime,
                std::max(105.f, actionPos.y + preActionVy * shiftTime + 0.5f * grav * shiftTime * shiftTime)
            );

            if (delta > 0) {
                for (int s = 1; s <= delta; ++s) {
                    float t = s * dtStep;
                    CCPoint prePt(
                        actionPos.x + vx * t,
                        std::max(105.f, actionPos.y + preActionVy * t + 0.5f * grav * t * t)
                    );
                    if (this->isTrajectoryPointColliding(prePt)) {
                        alive = false;
                        break;
                    }
                }
            }

            if (alive) {
                for (int step = 1; step <= 28; ++step) {
                    float t = step * dtStep;
                    CCPoint simPt(
                        shiftedStart.x + vx * t,
                        std::max(105.f, shiftedStart.y + postActionVy * t + 0.5f * grav * t * t)
                    );
                    if (this->isTrajectoryPointColliding(simPt)) {
                        alive = false;
                        break;
                    }
                }
            }

            if (alive) {
                survivingOffsets++;
            }
        }

        if (!isClick) {
            int holdFrames = static_cast<int>(std::round(holdDuration * refFPS));
            if (holdFrames >= 1 && holdFrames <= 12) {
                survivingOffsets = std::min(survivingOffsets, holdFrames);
            }
        }

        return std::clamp(survivingOffsets, 1, 15);
    }

    void drawAheadTrajectory(CCPoint curPos, CCPoint curVel, int refFPS) {
        if (!m_fields->m_projDrawer) return;
        m_fields->m_projDrawer->clear();

        if (!Mod::get()->getSettingValue<bool>("enable-mod")) return;
        if (!Mod::get()->getSettingValue<bool>("show-ahead-trajectory")) return;

        float dtStep = 1.0f / static_cast<float>(std::max(60, refFPS));
        float vx = std::max(220.f, curVel.x);
        float vy = curVel.y;
        float grav = m_fields->m_gravity;

        CCPoint prev = curPos;
        for (int step = 1; step <= 45; ++step) {
            float t = step * dtStep;
            CCPoint next(
                curPos.x + vx * t,
                std::max(105.f, curPos.y + vy * t + 0.5f * grav * t * t)
            );

            bool hit = this->isTrajectoryPointColliding(next);
            ccColor4F segColor = hit ? ccColor4F{1.f, 0.2f, 0.2f, 0.85f} : ccColor4F{0.35f, 1.f, 0.45f, 0.65f};
            m_fields->m_projDrawer->drawSegment(prev, next, 1.4f, segColor);

            if (hit) {
                m_fields->m_projDrawer->drawDot(next, 3.8f, {1.f, 0.15f, 0.15f, 0.95f});
                break;
            }
            prev = next;
        }
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (!this->m_player1) return;
        m_fields->m_elapsedTime += dt;

        CCPoint curPos = this->m_player1->getPosition();

        if (m_fields->m_hasPrevPos && dt > 0.0001f) {
            float newVx = (curPos.x - m_fields->m_prevPos.x) / dt;
            float newVy = (curPos.y - m_fields->m_prevPos.y) / dt;
            if (newVx > 10.f) m_fields->m_vel.x = newVx;
            m_fields->m_vel.y = newVy;
        }
        m_fields->m_prevPos = curPos;
        m_fields->m_hasPrevPos = true;

        if (!Mod::get()->getSettingValue<bool>("enable-mod")) return;

        if (Mod::get()->getSettingValue<bool>("show-white-trail") && m_fields->m_trailDrawer) {
            if (m_fields->m_trailPoints.empty() || ccpDistance(m_fields->m_trailPoints.back(), curPos) >= 2.5f) {
                if (m_fields->m_trailPoints.size() > 1) {
                    CCPoint lastPt = m_fields->m_trailPoints.back();
                    if (std::abs(curPos.x - lastPt.x) < 60.f && std::abs(curPos.y - lastPt.y) < 60.f) {
                        m_fields->m_trailDrawer->drawSegment(lastPt, curPos, 2.8f, {0.f, 0.f, 0.f, 0.65f});
                        m_fields->m_trailDrawer->drawSegment(lastPt, curPos, 1.9f, {1.f, 1.f, 1.f, 0.95f});
                    }
                }
                m_fields->m_trailPoints.push_back(curPos);
            }
        }

        m_fields->m_hazardUpdateTick++;
        if (m_fields->m_hazardUpdateTick >= 8) {
            m_fields->m_hazardUpdateTick = 0;
            this->updateNearbyHazards(curPos);
        }
        int refFPS = static_cast<int>(Mod::get()->getSettingValue<int64_t>("target-fps"));
        this->drawAheadTrajectory(curPos, m_fields->m_vel, refFPS);

        for (auto& pt : m_fields->m_savedPoints) {
            if (!pt.triggered && curPos.x >= pt.x - 2.0f) {
                pt.triggered = true;
                this->spawnWorldMarker(ccp(pt.x, pt.y), pt.frames);
                this->incrementTierCount(pt.frames);
            }
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        m_fields->m_isHolding = false;
        m_fields->m_hasPrevPos = false;
        m_fields->m_trailPoints.clear();

        for (int i = 0; i < 7; ++i) {
            m_fields->m_tierCounts[i] = 0;
        }
        for (auto& pt : m_fields->m_savedPoints) {
            pt.triggered = false;
        }

        if (m_fields->m_trailDrawer) m_fields->m_trailDrawer->clear();
        if (m_fields->m_projDrawer) m_fields->m_projDrawer->clear();
        if (m_fields->m_ringDrawer) m_fields->m_ringDrawer->clear();
        if (m_fields->m_worldTextContainer) m_fields->m_worldTextContainer->removeAllChildren();

        this->refreshBoardUI();
    }

    void clearSavedLevelData() {
        m_fields->m_savedPoints.clear();
        std::error_code ec;
        std::filesystem::remove(this->getLevelMacroFilePath(), ec);

        for (int i = 0; i < 7; ++i) {
            m_fields->m_tierCounts[i] = 0;
        }
        if (m_fields->m_ringDrawer) m_fields->m_ringDrawer->clear();
        if (m_fields->m_worldTextContainer) m_fields->m_worldTextContainer->removeAllChildren();
        this->refreshBoardUI();
    }
};

class $modify(AFCBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (button != 1 || !isPlayer1) return;

        auto playLayer = PlayLayer::get();
        if (!playLayer || playLayer != static_cast<GJBaseGameLayer*>(this)) return;
        if (!Mod::get()->getSettingValue<bool>("enable-mod")) return;

        auto afcLayer = static_cast<AFCPlayLayer*>(playLayer);
        auto fields = afcLayer->m_fields.self();

        std::string mode = Mod::get()->getSettingValue<std::string>("work-mode");
        bool canAnalyzeAndRecord = (mode != "Replay Saved Data Only");

        int refFPS = static_cast<int>(Mod::get()->getSettingValue<int64_t>("target-fps"));

        if (down) {
            fields->m_isHolding = true;
            fields->m_pressStartTime = fields->m_elapsedTime;
            if (playLayer->m_player1) {
                fields->m_pressPos = playLayer->m_player1->getPosition();
                fields->m_pressVel = fields->m_vel;
            }

            if (canAnalyzeAndRecord && playLayer->m_player1) {
                for (auto const& existing : fields->m_savedPoints) {
                    if (std::abs(existing.x - fields->m_pressPos.x) < 10.f) {
                        return;
                    }
                }

                int frames = afcLayer->analyzeTrajectoryFrameWindow(
                    fields->m_pressPos, fields->m_pressVel, true, 0.0, refFPS
                );

                if (frames <= 12) {
                    FramePointData newPt{
                        fields->m_pressPos.x,
                        fields->m_pressPos.y,
                        frames,
                        true,
                        true
                    };
                    fields->m_savedPoints.push_back(newPt);
                    afcLayer->saveMacroFramesToDisk();

                    afcLayer->spawnWorldMarker(fields->m_pressPos, frames);
                    afcLayer->incrementTierCount(frames);
                }
            }
        } else if (fields->m_isHolding) {
            fields->m_isHolding = false;
            double holdDuration = fields->m_elapsedTime - fields->m_pressStartTime;

            if (canAnalyzeAndRecord && playLayer->m_player1) {
                CCPoint releasePos = playLayer->m_player1->getPosition();

                int holdFrames = static_cast<int>(std::round(holdDuration * refFPS));
                int releaseFrames = afcLayer->analyzeTrajectoryFrameWindow(
                    releasePos, fields->m_vel, false, holdDuration, refFPS
                );
                int finalFrames = std::min(holdFrames, releaseFrames);
                if (finalFrames < 1) finalFrames = 1;

                for (auto& existing : fields->m_savedPoints) {
                    if (std::abs(existing.x - releasePos.x) < 14.f) {
                        return;
                    }
                }

                if (finalFrames <= 12) {
                    FramePointData newPt{
                        releasePos.x,
                        releasePos.y,
                        finalFrames,
                        false,
                        true
                    };
                    fields->m_savedPoints.push_back(newPt);
                    afcLayer->saveMacroFramesToDisk();

                    afcLayer->spawnWorldMarker(releasePos, finalFrames);
                    afcLayer->incrementTierCount(finalFrames);
                }
            }
        }
    }
};

class $modify(AFCUILayer, UILayer) {
    void keyDown(cocos2d::enumKeyCodes key, double timestamp) {
        UILayer::keyDown(key, timestamp);

        if (key == cocos2d::KEY_None) return;

        auto playLayer = PlayLayer::get();
        if (!playLayer) return;

        auto afcLayer = static_cast<AFCPlayLayer*>(playLayer);

        auto toggleKey = stringToKeyCode(Mod::get()->getSettingValue<std::string>("toggle-key"));
        auto trajKey   = stringToKeyCode(Mod::get()->getSettingValue<std::string>("trajectory-key"));
        auto resetKey  = stringToKeyCode(Mod::get()->getSettingValue<std::string>("reset-key"));

        if (key == toggleKey) {
            bool cur = Mod::get()->getSettingValue<bool>("enable-mod");
            Mod::get()->setSettingValue("enable-mod", !cur);
            afcLayer->refreshBoardUI();
        } else if (key == trajKey) {
            bool cur = Mod::get()->getSettingValue<bool>("show-ahead-trajectory");
            Mod::get()->setSettingValue("show-ahead-trajectory", !cur);
        } else if (key == resetKey) {
            afcLayer->clearSavedLevelData();
        }
    }
};
