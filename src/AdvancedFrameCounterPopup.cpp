#include "AdvancedFrameCounterPopup.hpp"
#include <array>

namespace {
    constexpr std::array<MacroExtension, 12> kAllExtensions = {
        MacroExtension::INTERNAL,
        MacroExtension::SLC,
        MacroExtension::GDR,
        MacroExtension::GDR2,
        MacroExtension::JSON,
        MacroExtension::TTR,
        MacroExtension::TTR2,
        MacroExtension::ECHO,
        MacroExtension::TCM,
        MacroExtension::RE4,
        MacroExtension::CML,
        MacroExtension::XD
    };
} // namespace

AdvancedFrameCounterPopup* AdvancedFrameCounterPopup::create(std::string const& initialPath) {
    auto* ret = new AdvancedFrameCounterPopup();
    if (ret && ret->initAnchored(380.f, 260.f, initialPath)) {
        ret->autorelease();
        return ret;
    }
    CC_SAFE_DELETE(ret);
    return nullptr;
}

bool AdvancedFrameCounterPopup::setup(std::string const& initialPath) {
    this->setTitle("Antic's Frame Window Counter");

    // Đồng bộ trạng thái đã lưu từ Mod Saved Values (nếu có)
    s_autoDetectExternalInjection = Mod::get()->getSavedValue<bool>("auto-detect-injection", true);
    s_displayOnPlayerPosition    = Mod::get()->getSavedValue<bool>("display-on-player-pos", true);
    s_selectedExtension           = static_cast<MacroExtension>(
        std::clamp(Mod::get()->getSavedValue<int>("selected-macro-ext", static_cast<int>(MacroExtension::ECHO)), 0, 11)
    );

    const CCSize contentSize = m_mainLayer->getContentSize();
    const float centerX = contentSize.width * 0.5f;

    // 1. Khung nền trang trí (Antic Style Dark Panel)
    auto* panelBg = cocos2d::extension::CCScale9Sprite::create("square02b_001.png", {0.f, 0.f, 80.f, 80.f});
    if (panelBg) {
        panelBg->setContentSize({contentSize.width - 32.f, contentSize.height - 62.f});
        panelBg->setColor({0, 0, 0});
        panelBg->setOpacity(95);
        panelBg->setPosition({centerX, contentSize.height * 0.46f});
        m_mainLayer->addChild(panelBg, -1);
    }

    // 2. Khu vực chọn định dạng tệp Macro (Dropdown & Cycle Selector)
    auto* formatLabel = CCLabelBMFont::create("Macro Format:", "goldFont.fnt");
    formatLabel->setScale(0.55f);
    formatLabel->setAnchorPoint({0.f, 0.5f});
    formatLabel->setPosition({30.f, contentSize.height - 62.f});
    m_mainLayer->addChild(formatLabel);

    auto* selectorBg = cocos2d::extension::CCScale9Sprite::create("square02b_001.png", {0.f, 0.f, 80.f, 80.f});
    selectorBg->setContentSize({130.f, 24.f});
    selectorBg->setColor({20, 28, 40});
    selectorBg->setOpacity(180);
    selectorBg->setPosition({centerX + 48.f, contentSize.height - 62.f});
    m_mainLayer->addChild(selectorBg);

    m_extValueLabel = CCLabelBMFont::create(macroExtensionToString(s_selectedExtension), "bigFont.fnt");
    m_extValueLabel->setScale(0.42f);
    m_extValueLabel->setPosition(selectorBg->getPosition());
    m_mainLayer->addChild(m_extValueLabel, 2);

    // Nút điều hướng Trái / Phải & Nút mở danh sách đổ xuống (Dropdown)
    auto* prevSpr = CCSprite::createWithSpriteFrameName("navArrowBtn_001.png");
    prevSpr->setFlipX(true);
    prevSpr->setScale(0.45f);
    auto* prevBtn = CCMenuItemSpriteExtra::create(prevSpr, this, menu_selector(AdvancedFrameCounterPopup::onPrevExtension));
    prevBtn->setPosition({selectorBg->getPositionX() - 78.f, selectorBg->getPositionY()});
    m_buttonMenu->addChild(prevBtn);

    auto* nextSpr = CCSprite::createWithSpriteFrameName("navArrowBtn_001.png");
    nextSpr->setScale(0.45f);
    auto* nextBtn = CCMenuItemSpriteExtra::create(nextSpr, this, menu_selector(AdvancedFrameCounterPopup::onNextExtension));
    nextBtn->setPosition({selectorBg->getPositionX() + 78.f, selectorBg->getPositionY()});
    m_buttonMenu->addChild(nextBtn);

    auto* dropSpr = ButtonSprite::create("List", 36, true, "bigFont.fnt", "GJ_button_04.png", 22.f, 0.4f);
    auto* dropBtn = CCMenuItemSpriteExtra::create(dropSpr, this, menu_selector(AdvancedFrameCounterPopup::onToggleDropdownList));
    dropBtn->setPosition({selectorBg->getPositionX() + 114.f, selectorBg->getPositionY()});
    m_buttonMenu->addChild(dropBtn);

    // Menu lưới Dropdown ẩn/hiện nhanh cho toàn bộ 11 định dạng + INTERNAL
    m_dropdownMenu = CCMenu::create();
    m_dropdownMenu->setPosition({centerX, contentSize.height - 96.f});
    m_dropdownMenu->setVisible(false);
    m_mainLayer->addChild(m_dropdownMenu, 20);

    for (size_t i = 0; i < kAllExtensions.size(); ++i) {
        const auto ext = kAllExtensions[i];
        auto* itemSpr = ButtonSprite::create(
            macroExtensionToString(ext), 46, true, "bigFont.fnt", "GJ_button_05.png", 20.f, 0.32f
        );
        auto* itemBtn = CCMenuItemSpriteExtra::create(
            itemSpr, this, menu_selector(AdvancedFrameCounterPopup::onSelectExtensionFromList)
        );
        itemBtn->setTag(static_cast<int>(ext));

        const int row = static_cast<int>(i / 6);
        const int col = static_cast<int>(i % 6);
        const float offsetX = (static_cast<float>(col) - 2.5f) * 56.f;
        const float offsetY = -static_cast<float>(row) * 24.f;
        itemBtn->setPosition({offsetX, offsetY});
        m_dropdownMenu->addChild(itemBtn);
    }

    // 3. Ô nhập đường dẫn tệp & Nút "Import Macro File"
    m_filePathInput = TextInput::create(220.f, "Macro path (e.g. macros/run.echo)", "chatFont.fnt");
    m_filePathInput->setPosition({centerX - 58.f, contentSize.height - 102.f});
    if (!initialPath.empty()) {
        m_filePathInput->setString(initialPath);
    } else if (!MacroParserManager::get().getActiveFilePath().empty()) {
        m_filePathInput->setString(MacroParserManager::get().getActiveFilePath());
    } else {
        auto defaultPath = (getMacroStorageDir() / "sample.echo").string();
        m_filePathInput->setString(defaultPath);
    }
    m_mainLayer->addChild(m_filePathInput);

    auto* importSpr = ButtonSprite::create(
        "Import Macro File", 110, true, "goldFont.fnt", "GJ_button_01.png", 28.f, 0.5f
    );
    auto* importBtn = CCMenuItemSpriteExtra::create(
        importSpr, this, menu_selector(AdvancedFrameCounterPopup::onImportMacroFile)
    );
    importBtn->setPosition({centerX + 114.f, contentSize.height - 102.f});
    m_buttonMenu->addChild(importBtn);

    // 4. Hai Checkbox cài đặt chuẩn Geode / Cocos2d-x
    // Checkbox 1: "Auto-detect External Macro Injection"
    auto* chkAutoDetect = CCMenuItemToggler::createWithStandardSprites(
        this,
        menu_selector(AdvancedFrameCounterPopup::onToggleAutoDetectInjection),
        0.68f
    );
    chkAutoDetect->toggle(s_autoDetectExternalInjection);
    chkAutoDetect->setPosition({44.f, contentSize.height - 148.f});
    m_buttonMenu->addChild(chkAutoDetect);

    auto* lblAutoDetect = CCLabelBMFont::create("Auto-detect External Macro Injection", "bigFont.fnt");
    lblAutoDetect->setScale(0.36f);
    lblAutoDetect->setAnchorPoint({0.f, 0.5f});
    lblAutoDetect->setPosition({66.f, contentSize.height - 148.f});
    m_mainLayer->addChild(lblAutoDetect);

    // Checkbox 2: "Display Frame Windows on Player Position"
    auto* chkPlayerPos = CCMenuItemToggler::createWithStandardSprites(
        this,
        menu_selector(AdvancedFrameCounterPopup::onToggleDisplayOnPlayerPos),
        0.68f
    );
    chkPlayerPos->toggle(s_displayOnPlayerPosition);
    chkPlayerPos->setPosition({44.f, contentSize.height - 182.f});
    m_buttonMenu->addChild(chkPlayerPos);

    auto* lblPlayerPos = CCLabelBMFont::create("Display Frame Windows on Player Position", "bigFont.fnt");
    lblPlayerPos->setScale(0.36f);
    lblPlayerPos->setAnchorPoint({0.f, 0.5f});
    lblPlayerPos->setPosition({66.f, contentSize.height - 182.f});
    m_mainLayer->addChild(lblPlayerPos);

    // 5. Nhãn trạng thái phản hồi ở chân Popup
    m_statusLabel = CCLabelBMFont::create("Ready to load macro file.", "chatFont.fnt");
    m_statusLabel->setScale(0.65f);
    m_statusLabel->setColor({180, 220, 255});
    m_statusLabel->setPosition({centerX, 26.f});
    m_mainLayer->addChild(m_statusLabel);

    updateExtensionDisplay();
    return true;
}

void AdvancedFrameCounterPopup::onPrevExtension(CCObject*) {
    int idx = static_cast<int>(s_selectedExtension);
    idx = (idx - 1 + static_cast<int>(kAllExtensions.size())) % static_cast<int>(kAllExtensions.size());
    s_selectedExtension = static_cast<MacroExtension>(idx);
    Mod::get()->setSavedValue("selected-macro-ext", idx);
    updateExtensionDisplay();
}

void AdvancedFrameCounterPopup::onNextExtension(CCObject*) {
    int idx = static_cast<int>(s_selectedExtension);
    idx = (idx + 1) % static_cast<int>(kAllExtensions.size());
    s_selectedExtension = static_cast<MacroExtension>(idx);
    Mod::get()->setSavedValue("selected-macro-ext", idx);
    updateExtensionDisplay();
}

void AdvancedFrameCounterPopup::onToggleDropdownList(CCObject*) {
    if (!m_dropdownMenu) return;
    m_dropdownExpanded = !m_dropdownExpanded;
    m_dropdownMenu->setVisible(m_dropdownExpanded);
}

void AdvancedFrameCounterPopup::onSelectExtensionFromList(CCObject* sender) {
    auto* node = static_cast<CCNode*>(sender);
    if (!node) return;
    const int tag = std::clamp(node->getTag(), 0, 11);
    s_selectedExtension = static_cast<MacroExtension>(tag);
    Mod::get()->setSavedValue("selected-macro-ext", tag);
    if (m_dropdownMenu) {
        m_dropdownExpanded = false;
        m_dropdownMenu->setVisible(false);
    }
    updateExtensionDisplay();
}

void AdvancedFrameCounterPopup::onToggleAutoDetectInjection(CCObject* sender) {
    auto* toggler = static_cast<CCMenuItemToggler*>(sender);
    if (!toggler) return;
    // Lưu ý: Trong GD, isToggled() trả về trạng thái TRƯỚC khi callback hoàn tất
    s_autoDetectExternalInjection = !toggler->isToggled();
    Mod::get()->setSavedValue("auto-detect-injection", s_autoDetectExternalInjection);
}

void AdvancedFrameCounterPopup::onToggleDisplayOnPlayerPos(CCObject* sender) {
    auto* toggler = static_cast<CCMenuItemToggler*>(sender);
    if (!toggler) return;
    s_displayOnPlayerPosition = !toggler->isToggled();
    Mod::get()->setSavedValue("display-on-player-pos", s_displayOnPlayerPosition);
}

void AdvancedFrameCounterPopup::onImportMacroFile(CCObject*) {
    std::string targetPath = m_filePathInput ? std::string(m_filePathInput->getString()) : "";
    if (targetPath.empty()) {
        updateStatusBanner("Please enter a valid macro file path.", {255, 90, 90});
        return;
    }

    auto& parser = MacroParserManager::get();
    if (parser.loadMacroFile(targetPath, s_selectedExtension)) {
        s_selectedExtension = parser.getActiveExtension();
        updateExtensionDisplay();

        std::string msg = fmt::format(
            "Loaded {} events ({}) @ {:.0f}Hz",
            parser.getEvents().size(),
            macroExtensionToString(parser.getActiveExtension()),
            parser.getDetectedFramerate()
        );
        updateStatusBanner(msg, {80, 255, 120});

        if (m_onMacroImported) {
            m_onMacroImported();
        }
    } else {
        updateStatusBanner("Import failed: " + parser.getLastError(), {255, 80, 80});
    }
}

void AdvancedFrameCounterPopup::updateExtensionDisplay() {
    if (m_extValueLabel) {
        m_extValueLabel->setString(macroExtensionToString(s_selectedExtension));
    }
}

void AdvancedFrameCounterPopup::updateStatusBanner(std::string const& msg, ccColor3B color) {
    if (m_statusLabel) {
        m_statusLabel->setString(msg.c_str());
        m_statusLabel->setColor(color);
    }
}
