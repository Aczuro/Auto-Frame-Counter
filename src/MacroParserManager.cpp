#include "MacroParserManager.hpp"
#include <fstream>
#include <sstream>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace {
    // Hàm tiện ích đọc an toàn kiểu nguyên mẫu Little-Endian từ mảng byte nhị phân
    template <typename T>
    bool readBinaryValue(std::vector<uint8_t> const& buf, size_t& offset, T& outVal) noexcept {
        if (offset + sizeof(T) > buf.size()) {
            return false;
        }
        std::memcpy(&outVal, buf.data() + offset, sizeof(T));
        offset += sizeof(T);
        return true;
    }

    // Kiểm tra nhanh xem buffer nhị phân có phải là văn bản JSON hợp lệ hay không
    bool looksLikeJsonPayload(std::vector<uint8_t> const& buf) noexcept {
        for (uint8_t b : buf) {
            if (std::isspace(static_cast<unsigned char>(b))) continue;
            return (b == '{' || b == '[');
        }
        return false;
    }
} // namespace

MacroParserManager& MacroParserManager::get() noexcept {
    static MacroParserManager instance;
    return instance;
}

void MacroParserManager::clear() noexcept {
    m_events.clear();
    m_lastError.clear();
    m_detectedFps = 240.0;
}

int MacroParserManager::normalizeFrameTo240Hz(int rawFrame, double sourceFps) const noexcept {
    if (sourceFps <= 0.0 || std::abs(sourceFps - 240.0) < 1e-3) {
        return std::max(0, rawFrame);
    }
    const double scaled = static_cast<double>(rawFrame) * (240.0 / sourceFps);
    return std::max(0, static_cast<int>(std::llround(scaled)));
}

void MacroParserManager::finalizeAndSortEvents() {
    // Sắp xếp ổn định theo thứ tự tăng dần của physicalFrame để con trỏ phát lại (playbackCursor) chạy O(1)
    std::stable_sort(m_events.begin(), m_events.end(), [](const UniversalInputEvent& a, const UniversalInputEvent& b) {
        return a.physicalFrame < b.physicalFrame;
    });
}

std::vector<MacroAction> MacroParserManager::exportAsMacroActions() const {
    std::vector<MacroAction> out;
    out.reserve(m_events.size());
    for (size_t i = 0; i < m_events.size(); ++i) {
        out.push_back(MacroAction::fromUniversalEvent(m_events[i], static_cast<int>(i)));
    }
    return out;
}

bool MacroParserManager::loadMacroFile(std::string const& filePath, MacroExtension ext) {
    clear();
    m_activeFilePath = filePath;

    std::filesystem::path fsPath(filePath);
    if (!std::filesystem::exists(fsPath)) {
        m_lastError = "File does not exist: " + filePath;
        log::error("[MacroParserManager] {}", m_lastError);
        return false;
    }

    // Nếu người dùng để INTERNAL nhưng nạp file ngoài, tự nhận diện qua đuôi file
    if (ext == MacroExtension::INTERNAL) {
        ext = parseMacroExtension(filePath);
    }
    m_activeExtension = ext;

    // Đọc luồng byte thô từ đĩa bằng tiện ích của Geode SDK
    auto binRes = file::readBinary(fsPath);
    if (!binRes.isOk()) {
        m_lastError = "Failed to read binary stream from: " + filePath;
        log::error("[MacroParserManager] {}", m_lastError);
        return false;
    }
    const std::vector<uint8_t> rawBytes = std::move(binRes).unwrap();
    if (rawBytes.empty()) {
        m_lastError = "Macro file is empty: " + filePath;
        return false;
    }

    bool parseOk = false;

    // ========================================================================
    // PHÂN LUỒNG GIẢI MÃ THEO ĐỊNH DẠNG (UNIVERSAL SWITCH-CASE DISPATCHER)
    // ========================================================================
    switch (ext) {
        case MacroExtension::INTERNAL:
        case MacroExtension::JSON:
        case MacroExtension::ECHO: {
            // Luồng 1: Nhóm định dạng JSON văn bản (.json, .echo, .internal)
            std::string rawText(reinterpret_cast<const char*>(rawBytes.data()), rawBytes.size());
            parseOk = parseJsonFamily(rawText, ext);
            break;
        }

        case MacroExtension::XD: {
            // Luồng 2: Định dạng văn bản phân cách bằng dấu '|' hoặc khoảng trắng của xdBot (.xd)
            std::string rawText(reinterpret_cast<const char*>(rawBytes.data()), rawBytes.size());
            parseOk = parseXdBotText(rawText);
            break;
        }

        case MacroExtension::CML: {
            // Luồng 3: Định dạng Camellia / Custom Macro List (.cml - CSV/Text hoặc JSON)
            std::string rawText(reinterpret_cast<const char*>(rawBytes.data()), rawBytes.size());
            if (looksLikeJsonPayload(rawBytes)) {
                parseOk = parseJsonFamily(rawText, ext);
            } else {
                parseOk = parseCmlText(rawText);
            }
            break;
        }

        case MacroExtension::SLC: {
            // Luồng 4: Định dạng nhị phân nguyên khối của Silicate (.slc)
            parseOk = parseSlcBinary(rawBytes);
            break;
        }

        case MacroExtension::GDR: {
            // Luồng 5: Định dạng GDReplayFormat v1 (.gdr - hỗ trợ cả JSON lẫn Binary)
            parseOk = parseGdrBinaryOrText(rawBytes, false);
            break;
        }

        case MacroExtension::GDR2: {
            // Luồng 6: Định dạng GDReplayFormat v2 (.gdr2 - Binary Header + Packed Input Records)
            parseOk = parseGdrBinaryOrText(rawBytes, true);
            break;
        }

        case MacroExtension::TTR: {
            // Luồng 7: Định dạng ToastedReplay / TasBot v1 (.ttr)
            if (looksLikeJsonPayload(rawBytes)) {
                std::string rawText(reinterpret_cast<const char*>(rawBytes.data()), rawBytes.size());
                parseOk = parseJsonFamily(rawText, ext);
            } else {
                parseOk = parseTtrBinary(rawBytes, false);
            }
            break;
        }

        case MacroExtension::TTR2: {
            // Luồng 8: Định dạng ToastedReplay v2 (.ttr2 - Binary Struct Stream)
            parseOk = parseTtrBinary(rawBytes, true);
            break;
        }

        case MacroExtension::TCM: {
            // Luồng 9: Định dạng TC Bot Macro (.tcm - Binary hoặc JSON)
            if (looksLikeJsonPayload(rawBytes)) {
                std::string rawText(reinterpret_cast<const char*>(rawBytes.data()), rawBytes.size());
                parseOk = parseJsonFamily(rawText, ext);
            } else {
                parseOk = parseTcmBinary(rawBytes);
            }
            break;
        }

        case MacroExtension::RE4: {
            // Luồng 10: Định dạng ReplayEngine 4 (.re4 - Binary Frame + XPos + Flags)
            parseOk = parseRe4Binary(rawBytes);
            break;
        }
    }

    if (parseOk && !m_events.empty()) {
        finalizeAndSortEvents();
        log::info(
            "[MacroParserManager] Loaded {} universal input events from {} ({})",
            m_events.size(),
            filePath,
            macroExtensionToString(m_activeExtension)
        );
        return true;
    }

    if (m_lastError.empty()) {
        m_lastError = "No valid input events decoded from file.";
    }
    return false;
}

// ============================================================================
// CHI TIẾT CHUYỂN ĐỔI CẤU TRÚC DỮ LIỆU ĐẦU VÀO CỦA TỪNG ĐỊNH DẠNG
// ============================================================================

bool MacroParserManager::parseJsonFamily(std::string const& rawText, MacroExtension /*ext*/) {
    auto jsonRes = matjson::parse(rawText);
    if (!jsonRes.isOk()) {
        m_lastError = "Invalid JSON syntax in macro file.";
        return false;
    }

    const matjson::Value root = jsonRes.unwrap();

    // 1. Đọc tần số khung hình gốc (fps / framerate) nếu file có khai báo (VD: Echo / GDR JSON / TasBot)
    if (root.contains("fps") && root["fps"].isNumber()) {
        m_detectedFps = root["fps"].asDouble().unwrapOr(240.0);
    } else if (root.contains("framerate") && root["framerate"].isNumber()) {
        m_detectedFps = root["framerate"].asDouble().unwrapOr(240.0);
    }

    // 2. Xác định mảng chứa danh sách sự kiện: hỗ trợ "actions" (Internal/MegaHack), "inputs" (GDR/Echo), "macro" (TasBot)
    const matjson::Value* actionsArrayPtr = nullptr;
    if (root.isArray()) {
        actionsArrayPtr = &root;
    } else if (root.contains("actions") && root["actions"].isArray()) {
        actionsArrayPtr = &root["actions"];
    } else if (root.contains("inputs") && root["inputs"].isArray()) {
        actionsArrayPtr = &root["inputs"];
    } else if (root.contains("echo_inputs") && root["echo_inputs"].isArray()) {
        actionsArrayPtr = &root["echo_inputs"];
    } else if (root.contains("macro") && root["macro"].isArray()) {
        actionsArrayPtr = &root["macro"];
    }

    if (!actionsArrayPtr) {
        m_lastError = "JSON root does not contain a recognized input array (actions/inputs/macro).";
        return false;
    }

    auto arrRes = actionsArrayPtr->asArray();
    if (!arrRes.isOk()) return false;

    for (const auto& item : arrRes.unwrap()) {
        UniversalInputEvent ev{};

        // Giải mã số Frame vật lý
        int rawFrame = 0;
        if (item.contains("frame")) {
            rawFrame = static_cast<int>(item["frame"].asInt().unwrapOr(0));
        } else if (item.contains("f")) {
            rawFrame = static_cast<int>(item["f"].asInt().unwrapOr(0));
        }
        ev.physicalFrame = normalizeFrameTo240Hz(rawFrame, m_detectedFps);

        // Giải mã trạng thái Nhấn (isClick = true) hay Nhả (isClick = false)
        // Hỗ trợ cả kiểu bool ("down", "hold", "press") và kiểu enum int ("type": 1=Press, 2=Release)
        if (item.contains("down") && item["down"].isBool()) {
            ev.isClick = item["down"].asBool().unwrapOr(true);
        } else if (item.contains("hold") && item["hold"].isBool()) {
            ev.isClick = item["hold"].asBool().unwrapOr(true);
        } else if (item.contains("press") && item["press"].isBool()) {
            ev.isClick = item["press"].asBool().unwrapOr(true);
        } else if (item.contains("type") && item["type"].isNumber()) {
            const int t = static_cast<int>(item["type"].asInt().unwrapOr(1));
            ev.isClick = (t == 1);
        }

        // Giải mã toạ độ X (positionX) và Y (positionY) của Player
        if (item.contains("x") && item["x"].isNumber()) {
            ev.positionX = static_cast<float>(item["x"].asDouble().unwrapOr(0.0));
        } else if (item.contains("pos_x") && item["pos_x"].isNumber()) {
            ev.positionX = static_cast<float>(item["pos_x"].asDouble().unwrapOr(0.0));
        }
        if (item.contains("y") && item["y"].isNumber()) {
            ev.positionY = static_cast<float>(item["y"].asDouble().unwrapOr(0.0));
        }

        // Giải mã Button & Player 1/2
        if (item.contains("button") && item["button"].isNumber()) {
            ev.button = static_cast<int>(item["button"].asInt().unwrapOr(1));
        } else if (item.contains("btn") && item["btn"].isNumber()) {
            ev.button = static_cast<int>(item["btn"].asInt().unwrapOr(1));
        }

        if (item.contains("player1") && item["player1"].isBool()) {
            ev.isPlayer1 = item["player1"].asBool().unwrapOr(true);
        } else if (item.contains("2p") && item["2p"].isBool()) {
            ev.isPlayer1 = !item["2p"].asBool().unwrapOr(false);
        } else if (item.contains("player") && item["player"].isNumber()) {
            ev.isPlayer1 = (item["player"].asInt().unwrapOr(1) == 1);
        }

        m_events.push_back(ev);
    }

    return !m_events.empty();
}

bool MacroParserManager::parseXdBotText(std::string const& rawText) {
    // Cấu trúc tệp .xd (xdBot):
    // Dòng đầu có thể chứa FPS (VD: "240" hoặc "fps: 240")
    // Mỗi dòng tiếp theo có định dạng: frame|hold|button|2p|x|y... hoặc cách nhau bởi dấu cách
    std::istringstream iss(rawText);
    std::string line;
    bool firstLine = true;

    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#') continue;

        if (firstLine && line.find('|') == std::string::npos && line.find(' ') == std::string::npos) {
            firstLine = false;
            try {
                double parsedFps = std::stod(line);
                if (parsedFps >= 30.0 && parsedFps <= 2400.0) {
                    m_detectedFps = parsedFps;
                    continue;
                }
            } catch (...) {}
        }
        firstLine = false;

        // Thay thế dấu '|' hoặc ',' bằng khoảng trắng để dễ tách token
        for (char& c : line) {
            if (c == '|' || c == ',') c = ' ';
        }

        std::istringstream ls(line);
        int rawFrame = 0;
        int holdInt = 0;
        int button = 1;
        int isPlayer2Int = 0;
        float posX = 0.0f;
        float posY = 0.0f;

        if (!(ls >> rawFrame >> holdInt)) {
            continue;
        }
        if (!(ls >> button)) button = 1;
        if (!(ls >> isPlayer2Int)) isPlayer2Int = 0;
        if (!(ls >> posX)) posX = 0.0f;
        if (!(ls >> posY)) posY = 0.0f;

        UniversalInputEvent ev{};
        ev.physicalFrame = normalizeFrameTo240Hz(rawFrame, m_detectedFps);
        ev.isClick       = (holdInt != 0);
        ev.button        = std::clamp(button, 1, 3);
        ev.isPlayer1     = (isPlayer2Int == 0);
        ev.positionX     = posX;
        ev.positionY     = posY;
        m_events.push_back(ev);
    }

    return !m_events.empty();
}

bool MacroParserManager::parseCmlText(std::string const& rawText) {
    // Định dạng .cml (Custom Macro List): CSV/Text dạng "frame,isClick,x,button,player1"
    return parseXdBotText(rawText);
}

bool MacroParserManager::parseSlcBinary(std::vector<uint8_t> const& bytes) {
    // Cấu trúc nhị phân .slc (Silicate Replay):
    // Header: [4 bytes Magic "SILC" hoặc FPS double (8 bytes)] + [4 bytes uint32_t inputCount]
    // Mỗi bản ghi (Record): [int32_t frame][uint8_t packedFlags (bit0=isClick, bit1=isP1, bits2..3=btn)][float posX]
    // Hoặc bản ghi rút gọn 8 bytes: [uint32_t frame][uint8_t isClick][uint8_t btn][uint8_t p1][uint8_t pad]
    if (bytes.size() < 12) {
        m_lastError = ".slc binary buffer is smaller than minimum header size.";
        return false;
    }

    size_t offset = 0;
    if (std::memcmp(bytes.data(), "SILC", 4) == 0 || std::memcmp(bytes.data(), "SLC\0", 4) == 0) {
        offset += 4;
    }

    double headerFps = 240.0;
    uint32_t recordCount = 0;
    if (!readBinaryValue(bytes, offset, headerFps) || !readBinaryValue(bytes, offset, recordCount)) {
        return false;
    }
    if (headerFps >= 30.0 && headerFps <= 2400.0) {
        m_detectedFps = headerFps;
    }

    const size_t remaining = bytes.size() - offset;
    // Tự động suy luận kích thước mỗi bản ghi dựa trên dung lượng còn lại
    const size_t stride = (recordCount > 0 && remaining >= recordCount * 9 && (remaining % recordCount == 0))
        ? (remaining / recordCount)
        : 9;

    while (offset + stride <= bytes.size()) {
        const size_t recordStart = offset;
        int32_t rawFrame = 0;
        uint8_t flags = 0;
        float posX = 0.0f;

        readBinaryValue(bytes, offset, rawFrame);
        readBinaryValue(bytes, offset, flags);
        if (stride >= 9) {
            readBinaryValue(bytes, offset, posX);
        }
        offset = recordStart + stride;

        UniversalInputEvent ev{};
        ev.physicalFrame = normalizeFrameTo240Hz(rawFrame, m_detectedFps);
        ev.isClick       = (flags & 0x01) != 0;
        ev.isPlayer1     = ((flags & 0x02) == 0);
        ev.button        = std::clamp(static_cast<int>((flags >> 2) & 0x03), 1, 3);
        ev.positionX     = posX;
        m_events.push_back(ev);
    }

    return !m_events.empty();
}

bool MacroParserManager::parseGdrBinaryOrText(std::vector<uint8_t> const& bytes, bool isV2) {
    // Nếu tệp .gdr được lưu ở dạng JSON (phổ biến ở GDR v1), chuyển sang bộ giải mã JSON
    if (looksLikeJsonPayload(bytes)) {
        std::string rawText(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return parseJsonFamily(rawText, isV2 ? MacroExtension::GDR2 : MacroExtension::GDR);
    }

    // Giải mã luồng nhị phân .gdr / .gdr2:
    // Header: Magic "GDR1" / "GDR2" (4 bytes) + float/double framerate + uint32_t count
    // Record (12 bytes): [uint32_t frame][uint8_t down][uint8_t button][uint8_t player2][uint8_t pad][float xPos]
    size_t offset = 0;
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "GDR", 3) == 0) {
        offset += 4;
        float fps = 240.0f;
        uint32_t count = 0;
        if (readBinaryValue(bytes, offset, fps) && readBinaryValue(bytes, offset, count)) {
            if (fps >= 30.0f && fps <= 2400.0f) m_detectedFps = fps;
        }
    }

    while (offset + 12 <= bytes.size()) {
        uint32_t rawFrame = 0;
        uint8_t down = 0;
        uint8_t btn = 1;
        uint8_t isP2 = 0;
        uint8_t pad = 0;
        float xPos = 0.0f;

        readBinaryValue(bytes, offset, rawFrame);
        readBinaryValue(bytes, offset, down);
        readBinaryValue(bytes, offset, btn);
        readBinaryValue(bytes, offset, isP2);
        readBinaryValue(bytes, offset, pad);
        readBinaryValue(bytes, offset, xPos);

        UniversalInputEvent ev{};
        ev.physicalFrame = normalizeFrameTo240Hz(static_cast<int>(rawFrame), m_detectedFps);
        ev.isClick       = (down != 0);
        ev.button        = std::clamp(static_cast<int>(btn), 1, 3);
        ev.isPlayer1     = (isP2 == 0);
        ev.positionX     = xPos;
        m_events.push_back(ev);
    }

    return !m_events.empty();
}

bool MacroParserManager::parseTtrBinary(std::vector<uint8_t> const& bytes, bool /*isV2*/) {
    // Cấu trúc nhị phân .ttr / .ttr2 (ToastedReplay / TasBot):
    // Header: [4 bytes Magic "TTR1"/"TTR2" (nếu có)] + [float fps (4 bytes)]
    // Record (10 bytes): [int32_t frame][float xPos][uint8_t stateBitmask][uint8_t button]
    size_t offset = 0;
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "TTR", 3) == 0) {
        offset += 4;
        float fps = 240.0f;
        if (readBinaryValue(bytes, offset, fps) && fps >= 30.0f && fps <= 2400.0f) {
            m_detectedFps = fps;
        }
    }

    while (offset + 10 <= bytes.size()) {
        int32_t rawFrame = 0;
        float xPos = 0.0f;
        uint8_t bitmask = 0;
        uint8_t button = 1;

        readBinaryValue(bytes, offset, rawFrame);
        readBinaryValue(bytes, offset, xPos);
        readBinaryValue(bytes, offset, bitmask);
        readBinaryValue(bytes, offset, button);

        UniversalInputEvent ev{};
        ev.physicalFrame = normalizeFrameTo240Hz(rawFrame, m_detectedFps);
        ev.isClick       = (bitmask & 0x01) != 0;
        ev.isPlayer1     = ((bitmask & 0x02) == 0);
        ev.button        = std::clamp(static_cast<int>(button), 1, 3);
        ev.positionX     = xPos;
        m_events.push_back(ev);
    }

    return !m_events.empty();
}

bool MacroParserManager::parseTcmBinary(std::vector<uint8_t> const& bytes) {
    // Cấu trúc nhị phân .tcm (TC Bot Macro):
    // Header: [4 bytes Magic "TCM\0" (tuỳ chọn)] + [float fps]
    // Record (9 bytes): [int32_t frame][uint8_t actionFlags][float xPos]
    size_t offset = 0;
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "TCM", 3) == 0) {
        offset += 4;
        float fps = 240.0f;
        if (readBinaryValue(bytes, offset, fps) && fps >= 30.0f && fps <= 2400.0f) {
            m_detectedFps = fps;
        }
    }

    while (offset + 9 <= bytes.size()) {
        int32_t rawFrame = 0;
        uint8_t flags = 0;
        float xPos = 0.0f;

        readBinaryValue(bytes, offset, rawFrame);
        readBinaryValue(bytes, offset, flags);
        readBinaryValue(bytes, offset, xPos);

        UniversalInputEvent ev{};
        ev.physicalFrame = normalizeFrameTo240Hz(rawFrame, m_detectedFps);
        ev.isClick       = (flags & 0x01) != 0;
        ev.isPlayer1     = ((flags & 0x02) == 0);
        ev.button        = std::clamp(static_cast<int>((flags >> 2) & 0x03), 1, 3);
        ev.positionX     = xPos;
        m_events.push_back(ev);
    }

    return !m_events.empty();
}

bool MacroParserManager::parseRe4Binary(std::vector<uint8_t> const& bytes) {
    // Cấu trúc nhị phân .re4 (ReplayEngine 4):
    // Header: [4 bytes Magic "RE4\0" (tuỳ chọn)] + [float fps]
    // Record (14 bytes): [int32_t frame][float xPos][float yPos][uint8_t isHold][uint8_t playerAndBtn]
    size_t offset = 0;
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "RE4", 3) == 0) {
        offset += 4;
        float fps = 240.0f;
        if (readBinaryValue(bytes, offset, fps) && fps >= 30.0f && fps <= 2400.0f) {
            m_detectedFps = fps;
        }
    }

    while (offset + 14 <= bytes.size()) {
        int32_t rawFrame = 0;
        float xPos = 0.0f;
        float yPos = 0.0f;
        uint8_t isHold = 0;
        uint8_t playerAndBtn = 1;

        readBinaryValue(bytes, offset, rawFrame);
        readBinaryValue(bytes, offset, xPos);
        readBinaryValue(bytes, offset, yPos);
        readBinaryValue(bytes, offset, isHold);
        readBinaryValue(bytes, offset, playerAndBtn);

        UniversalInputEvent ev{};
        ev.physicalFrame = normalizeFrameTo240Hz(rawFrame, m_detectedFps);
        ev.isClick       = (isHold != 0);
        ev.isPlayer1     = ((playerAndBtn & 0x80) == 0);
        ev.button        = std::clamp(static_cast<int>(playerAndBtn & 0x0F), 1, 3);
        ev.positionX     = xPos;
        ev.positionY     = yPos;
        m_events.push_back(ev);
    }

    return !m_events.empty();
}
