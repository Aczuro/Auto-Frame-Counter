#pragma once
#include "MacroTypes.hpp"
#include "MacroParserManager.hpp"
#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <string>
#include <functional>

using namespace geode::prelude;

// ============================================================================
// Adapter đa phiên bản cho Geode Popup:
// - Trên Geode SDK v4.x: `geode::Popup<Args...>` là class template có `setup(Args...)`.
// - Trên Geode SDK v5.x (`mod.json` -> "geode": "5.11.2"): `geode::Popup` không còn
//   là template mà truyền tham số trực tiếp vào `Popup::init(width, height)`.
// Đoạn kiểm tra dưới đây cho phép giữ nguyên cú pháp kế thừa
// `public AFCPopupBase<std::string const&>` và biên dịch sạch trên cả Geode v4 lẫn v5.
// ============================================================================
template <typename... Args>
class AFCPopupBase : public geode::Popup {
protected:
    bool initAnchored(float width, float height, Args... args) {
        if (!geode::Popup::init(width, height)) {
            return false;
        }
        return this->setup(std::forward<Args>(args)...);
    }

    virtual bool setup(Args... args) = 0;
};

// ============================================================================
// GIAO DIỆN CÀI ĐẶT CHÍNH (MAIN MOD MENU UI - ANTIC'S FRAME COUNTER STYLE)
// ============================================================================
class AdvancedFrameCounterPopup : public AFCPopupBase<std::string const&> {
public:
    static AdvancedFrameCounterPopup* create(std::string const& initialPath = "");

    // Callback tuỳ chọn để thông báo cho PlayLayer nạp lại danh sách Macro ngay sau khi Import
    void setOnMacroImportedCallback(std::function<void()> cb) {
        m_onMacroImported = std::move(cb);
    }

    static bool isAutoDetectInjectionEnabled() noexcept { return s_autoDetectExternalInjection; }
    static bool isDisplayOnPlayerPosEnabled() noexcept  { return s_displayOnPlayerPosition; }
    static MacroExtension getSelectedFormat() noexcept  { return s_selectedExtension; }

protected:
    bool setup(std::string const& initialPath) override;

    // Xử lý sự kiện nút bấm trên Popup
    void onPrevExtension(CCObject* sender);
    void onNextExtension(CCObject* sender);
    void onToggleDropdownList(CCObject* sender);
    void onSelectExtensionFromList(CCObject* sender);
    void onImportMacroFile(CCObject* sender);
    void onToggleAutoDetectInjection(CCObject* sender);
    void onToggleDisplayOnPlayerPos(CCObject* sender);

    void updateExtensionDisplay();
    void updateStatusBanner(std::string const& msg, ccColor3B color);

private:
    static inline MacroExtension s_selectedExtension = MacroExtension::ECHO;
    static inline bool s_autoDetectExternalInjection = true;
    static inline bool s_displayOnPlayerPosition    = true;

    CCLabelBMFont* m_extValueLabel = nullptr;
    CCLabelBMFont* m_statusLabel   = nullptr;
    TextInput* m_filePathInput     = nullptr;
    CCMenu* m_dropdownMenu         = nullptr;
    bool m_dropdownExpanded        = false;

    std::function<void()> m_onMacroImported;
};
