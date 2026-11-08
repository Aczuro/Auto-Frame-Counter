#pragma once
#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>
#include <matjson.hpp>
#include <vector>
#include <string>
#include <string_view>
#include <filesystem>
#include <algorithm>
#include <cctype>

using namespace geode::prelude;

// ============================================================================
// HẰNG SỐ CẤU HÌNH HỆ THỐNG ĐẾM FRAME WINDOW & TRAJECTORY
// ============================================================================
constexpr int MAX_BACKWARD_FRAMES  = 12;
constexpr int MAX_FORWARD_FRAMES   = 12;
constexpr int SURVIVE_CHECK_FRAMES = 72;
constexpr int ACTIONS_PER_UI_TICK  = 3;
constexpr int PREVIEW_STEPS        = 100;
constexpr int WHITE_TRAIL_MAX_PTS  = 180;
constexpr int MAX_BUCKET           = 10;

// ============================================================================
// 1. ĐỊNH NGHĨA 11 ĐỊNH DẠNG FILE MACRO PHỔ BIẾN + INTERNAL
// ============================================================================
enum class MacroExtension : int {
    INTERNAL = 0, // Định dạng lưu trữ nội bộ của Mod (level_<id>.json)
    SLC      = 1, // Silicate Bot (.slc - Binary Replay)
    GDR      = 2, // GDReplayFormat v1 (.gdr - JSON / MsgPack)
    GDR2     = 3, // GDReplayFormat v2 (.gdr2 - Binary / MsgPack)
    JSON     = 4, // Generic / MegaHack / Echo JSON (.json)
    TTR      = 5, // TasBot / ToastedReplay v1 (.ttr)
    TTR2     = 6, // TasBot / ToastedReplay v2 (.ttr2)
    ECHO     = 7, // Echo Bot (.echo)
    TCM      = 8, // TC Bot Macro (.tcm)
    RE4      = 9, // ReplayEngine 4 (.re4)
    CML      = 10,// Camellia / Custom Macro List (.cml)
    XD       = 11 // xdBot (.xd - Pipe/Space-delimited Text)
};

// Chuyển đổi MacroExtension sang chuỗi hiển thị chuẩn (dùng cho AnticHUDLayer & Dropdown UI)
inline constexpr const char* macroExtensionToString(MacroExtension ext) noexcept {
    switch (ext) {
        case MacroExtension::INTERNAL: return ".internal";
        case MacroExtension::SLC:      return ".slc";
        case MacroExtension::GDR:      return ".gdr";
        case MacroExtension::GDR2:     return ".gdr2";
        case MacroExtension::JSON:     return ".json";
        case MacroExtension::TTR:      return ".ttr";
        case MacroExtension::TTR2:     return ".ttr2";
        case MacroExtension::ECHO:     return ".echo";
        case MacroExtension::TCM:      return ".tcm";
        case MacroExtension::RE4:      return ".re4";
        case MacroExtension::CML:      return ".cml";
        case MacroExtension::XD:       return ".xd";
    }
    return ".internal";
}

// Tự động nhận diện MacroExtension từ đường dẫn file hoặc chuỗi đuôi file
inline MacroExtension parseMacroExtension(std::string_view extOrPath) {
    std::string lower;
    lower.reserve(extOrPath.size());
    for (char c : extOrPath) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    auto endsWith = [&](std::string_view suffix) {
        return lower.size() >= suffix.size() &&
               lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    if (endsWith(".gdr2")) return MacroExtension::GDR2;
    if (endsWith(".ttr2")) return MacroExtension::TTR2;
    if (endsWith(".echo")) return MacroExtension::ECHO;
    if (endsWith(".json")) return MacroExtension::JSON;
    if (endsWith(".slc"))  return MacroExtension::SLC;
    if (endsWith(".gdr"))  return MacroExtension::GDR;
    if (endsWith(".ttr"))  return MacroExtension::TTR;
    if (endsWith(".tcm"))  return MacroExtension::TCM;
    if (endsWith(".re4"))  return MacroExtension::RE4;
    if (endsWith(".cml"))  return MacroExtension::CML;
    if (endsWith(".xd"))   return MacroExtension::XD;
    return MacroExtension::INTERNAL;
}

// ============================================================================
// 2. CẤU TRÚC SỰ KIỆN ĐẦU VÀO CHUẨN HOÁ (UNIVERSAL INPUT EVENT)
// ============================================================================
struct UniversalInputEvent {
    bool isClick = true;      // true = Nhấn giữ (Press), false = Nhả (Release)
    int physicalFrame = 0;    // Số thứ tự Frame vật lý cố định (Fixed Timestep 240Hz)
    float positionX = 0.0f;   // Tọa độ X của nhân vật tại thời điểm phát sinh input

    // Các trường mở rộng hỗ trợ Dual Mode (2 Player), Platformer & Phân tích Frame Window
    float positionY = 0.0f;
    int button = 1;           // 1 = Jump, 2 = Left, 3 = Right
    bool isPlayer1 = true;    // true = Player 1, false = Player 2
};

// ============================================================================
// 3. CẤU TRÚC MACRO ACTION & HÀM LƯU TRỮ TƯƠNG THÍCH NGƯỢC VỚI LÕI ENGINE
// ============================================================================
enum class ActionType : int {
    Press   = 1, // Type 1: Press (Nhấn giữ chuột / phím)
    Release = 2  // Type 2: Release (Nhả chuột / phím)
};

inline const char* actionTypeToString(ActionType t) {
    return (t == ActionType::Press) ? "Type 1: Press" : "Type 2: Release";
}

enum class BotStage {
    Idle,
    Pass1_Backward,
    Pass2_Forward
};

struct MacroAction {
    int index = 0;
    int frame = 0;
    ActionType type = ActionType::Press;
    int button = 1;
    bool player1 = true;
    float x = 0.f;
    float y = 0.f;

    bool analyzed = false;
    int backwardFrames = 0;
    int forwardFrames = 0;
    int windowSize = 1;

    // Chuyển đổi trực tiếp từ UniversalInputEvent sang MacroAction để chạy TrajectorySim
    static MacroAction fromUniversalEvent(const UniversalInputEvent& ev, int idx = 0) {
        MacroAction act;
        act.index          = idx;
        act.frame          = ev.physicalFrame;
        act.type           = ev.isClick ? ActionType::Press : ActionType::Release;
        act.button         = ev.button;
        act.player1        = ev.isPlayer1;
        act.x              = ev.positionX;
        act.y              = ev.positionY;
        act.analyzed       = false;
        act.backwardFrames = 0;
        act.forwardFrames  = 0;
        act.windowSize     = 1;
        return act;
    }

    UniversalInputEvent toUniversalEvent() const {
        UniversalInputEvent ev;
        ev.isClick       = (type == ActionType::Press);
        ev.physicalFrame = frame;
        ev.positionX     = x;
        ev.positionY     = y;
        ev.button        = button;
        ev.isPlayer1     = player1;
        return ev;
    }
};

inline std::filesystem::path getMacroStorageDir() {
    return Mod::get()->getSaveDir() / "macros";
}

inline std::filesystem::path getLevelJsonPath(GJGameLevel* level) {
    auto dir = getMacroStorageDir();
    (void)file::createDirectoryAll(dir);
    int levelID = level ? level->m_levelID.value() : 0;
    std::string filename = "level_" + std::to_string(levelID) + ".json";
    return dir / filename;
}

inline void recomputeTally(const std::vector<MacroAction>& actions, int tally[MAX_BUCKET]) {
    std::fill(tally, tally + MAX_BUCKET, 0);
    for (const auto& a : actions) {
        if (a.analyzed) {
            int sz = a.windowSize;
            if (sz >= 1 && sz < MAX_BUCKET) {
                tally[sz]++;
            } else if (sz >= MAX_BUCKET) {
                tally[MAX_BUCKET - 1]++;
            }
        }
    }
}

inline bool saveMacroData(GJGameLevel* level, bool analyzed, const std::vector<MacroAction>& actions) {
    auto path = getLevelJsonPath(level);

    matjson::Value root = matjson::Value::object();
    root["levelID"]  = level ? level->m_levelID.value() : 0;
    root["analyzed"] = analyzed;

    matjson::Value arr = matjson::Value::array();
    for (const auto& act : actions) {
        matjson::Value item = matjson::Value::object();
        item["index"]           = act.index;
        item["frame"]           = act.frame;
        item["type"]            = static_cast<int>(act.type);
        item["button"]          = act.button;
        item["player1"]         = act.player1;
        item["x"]               = act.x;
        item["y"]               = act.y;
        item["analyzed"]        = act.analyzed;
        item["backward_frames"] = act.backwardFrames;
        item["forward_frames"]  = act.forwardFrames;
        item["window_size"]     = act.windowSize;
        arr.push(item);
    }

    root["actions"] = arr;
    return file::writeString(path, root.dump(4)).isOk();
}

inline bool loadMacroData(GJGameLevel* level, bool& outAnalyzed, std::vector<MacroAction>& outActions, int tally[MAX_BUCKET]) {
    outActions.clear();
    outAnalyzed = false;
    std::fill(tally, tally + MAX_BUCKET, 0);

    auto path = getLevelJsonPath(level);
    if (!std::filesystem::exists(path)) return false;

    auto strRes = file::readString(path);
    if (!strRes.isOk()) return false;

    auto jsonRes = matjson::parse(strRes.unwrap());
    if (!jsonRes.isOk()) return false;

    matjson::Value root = jsonRes.unwrap();
    outAnalyzed = root["analyzed"].asBool().unwrapOr(false);

    if (root.contains("actions") && root["actions"].isArray()) {
        if (auto arrRes = root["actions"].asArray(); arrRes.isOk()) {
            for (const auto& item : arrRes.unwrap()) {
                MacroAction act;
                act.index          = static_cast<int>(item["index"].asInt().unwrapOr(0));
                act.frame          = static_cast<int>(item["frame"].asInt().unwrapOr(0));
                int typeCode       = static_cast<int>(item["type"].asInt().unwrapOr(1));
                act.type           = (typeCode == 2) ? ActionType::Release : ActionType::Press;
                act.button         = static_cast<int>(item["button"].asInt().unwrapOr(1));
                act.player1        = item["player1"].asBool().unwrapOr(true);
                act.x              = static_cast<float>(item["x"].asDouble().unwrapOr(0.0));
                act.y              = static_cast<float>(item["y"].asDouble().unwrapOr(0.0));
                act.analyzed       = item["analyzed"].asBool().unwrapOr(false);
                act.backwardFrames = static_cast<int>(item["backward_frames"].asInt().unwrapOr(0));
                act.forwardFrames  = static_cast<int>(item["forward_frames"].asInt().unwrapOr(0));
                act.windowSize     = std::max(1, static_cast<int>(item["window_size"].asInt().unwrapOr(1)));
                outActions.push_back(std::move(act));
            }
        }
    }

    recomputeTally(outActions, tally);
    return !outActions.empty();
}
