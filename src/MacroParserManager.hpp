#pragma once
#include "MacroTypes.hpp"
#include <vector>
#include <string>
#include <cstdint>

using namespace geode::prelude;

// ============================================================================
// BỘ XỬ LÝ PHÂN GIẢI TỆP MACRO ĐA ĐỊNH DẠNG (UNIVERSAL MACRO PARSER)
// Hỗ trợ giải mã trực tiếp 11 định dạng (.slc, .gdr, .gdr2, .json, .ttr, .ttr2,
// .echo, .tcm, .re4, .cml, .xd) và chuẩn hoá về std::vector<UniversalInputEvent>
// ============================================================================
class MacroParserManager {
public:
    static MacroParserManager& get() noexcept;

    // Tải và phân giải tệp Macro theo định dạng chỉ định (hoặc tự nhận diện qua đuôi file)
    bool loadMacroFile(std::string const& filePath, MacroExtension ext);

    // Chuyển đổi danh sách UniversalInputEvent đã nạp sang danh sách MacroAction cho PlayLayer
    std::vector<MacroAction> exportAsMacroActions() const;

    // Truy xuất danh sách sự kiện đầu vào đã chuẩn hoá
    const std::vector<UniversalInputEvent>& getEvents() const noexcept { return m_events; }
    std::vector<UniversalInputEvent>& getEventsMutable() noexcept { return m_events; }

    MacroExtension getActiveExtension() const noexcept { return m_activeExtension; }
    void setActiveExtension(MacroExtension ext) noexcept { m_activeExtension = ext; }

    const std::string& getActiveFilePath() const noexcept { return m_activeFilePath; }
    const std::string& getLastError() const noexcept { return m_lastError; }
    double getDetectedFramerate() const noexcept { return m_detectedFps; }

    void clear() noexcept;

private:
    MacroParserManager() = default;

    // Các hàm giải mã chuyên biệt cho từng nhóm cấu trúc tệp
    bool parseJsonFamily(std::string const& rawText, MacroExtension ext);
    bool parseXdBotText(std::string const& rawText);
    bool parseCmlText(std::string const& rawText);
    bool parseSlcBinary(std::vector<uint8_t> const& bytes);
    bool parseGdrBinaryOrText(std::vector<uint8_t> const& bytes, bool isV2);
    bool parseTtrBinary(std::vector<uint8_t> const& bytes, bool isV2);
    bool parseTcmBinary(std::vector<uint8_t> const& bytes);
    bool parseRe4Binary(std::vector<uint8_t> const& bytes);

    // Quy đổi frame từ tần số lấy mẫu gốc của Macro (VD: 360Hz, 480Hz) về chuẩn vật lý 240Hz của GD 2.2
    int normalizeFrameTo240Hz(int rawFrame, double sourceFps) const noexcept;
    void finalizeAndSortEvents();

private:
    std::vector<UniversalInputEvent> m_events;
    MacroExtension m_activeExtension = MacroExtension::INTERNAL;
    std::string m_activeFilePath;
    std::string m_lastError;
    double m_detectedFps = 240.0;
};
