#pragma once
#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>
#include <matjson.hpp>
#include <vector>
#include <string>
#include <filesystem>

using namespace geode::prelude;

constexpr int MAX_BACKWARD_FRAMES  = 12;
constexpr int MAX_FORWARD_FRAMES   = 12;
constexpr int SURVIVE_CHECK_FRAMES = 72;
constexpr int ACTIONS_PER_UI_TICK  = 3;
constexpr int PREVIEW_STEPS        = 100;
constexpr int WHITE_TRAIL_MAX_PTS  = 180;
constexpr int MAX_BUCKET           = 10;

// Phân loại rõ ràng 2 loại Action của Macro
enum class ActionType : int {
    Press   = 1, // Type 1: Press (Nhấn giữ chuột / phím)
    Release = 2  // Type 2: Release (Nhả chuột / phím)
};

inline const char* actionTypeToString(ActionType t) {
    return (t == ActionType::Press) ? "Type 1: Press" : "Type 2: Release";
}

enum class BotStage {
    Record,       // Ghi Macro trong Practice Mode
    RollingScan,  // Quét cuốn chiếu từ đầu đến cuối level -> lưu JSON
    Replay        // Đọc file JSON và phát lại Replay
};

struct TrajectoryBranch {
    int delta = 0;
    bool survived = false;
    std::vector<CCPoint> points;
};

struct MacroAction {
    int index = 0;
    int frame = 0;
    ActionType type = ActionType::Press; // 1 = Press, 2 = Release
    int button = 1;
    bool player1 = true;
    float x = 0.f;
    float y = 0.f;

    // Kết quả sau khi Rolling Scan
    bool analyzed = false;
    int backwardFrames = 0;
    int forwardFrames = 0;
    int windowSize = 1;

    std::vector<TrajectoryBranch> branches;
};

namespace MacroStorage {
    inline std::filesystem::path getLevelJsonPath(GJGameLevel* level) {
        int id = level ? level->m_levelID.value() : 0;
        std::string filename = (id > 0)
            ? fmt::format("level_{}.json", id)
            : fmt::format("editor_{}.json", level ? std::string(level->m_levelName) : "custom");
        return Mod::get()->getSaveDir() / filename;
    }

    inline void recomputeTally(const std::vector<MacroAction>& actions, int (&tally)[MAX_BUCKET + 2]) {
        std::fill(std::begin(tally), std::end(tally), 0);
        for (const auto& act : actions) {
            if (act.analyzed && act.windowSize > 0) {
                int b = (act.windowSize > MAX_BUCKET) ? (MAX_BUCKET + 1) : act.windowSize;
                tally[b]++;
            }
        }
    }

    inline void saveToJson(
        GJGameLevel* level,
        int targetFps,
        bool analyzed,
        const std::vector<MacroAction>& actions,
        int (&tally)[MAX_BUCKET + 2]
    ) {
        if (!level) return;
        recomputeTally(actions, tally);

        matjson::Value root = matjson::Value::object();
        root["level_id"]      = level->m_levelID.value();
        root["target_fps"]    = targetFps;
        root["analyzed"]      = analyzed;
        root["total_actions"] = static_cast<int>(actions.size());

        matjson::Value tallyObj = matjson::Value::object();
        for (int w = 1; w <= MAX_BUCKET; w++) {
            tallyObj[fmt::format("{}F", w)] = tally[w];
        }
        tallyObj[fmt::format("{}+F", MAX_BUCKET + 1)] = tally[MAX_BUCKET + 1];
        root["tally"] = std::move(tallyObj);

        matjson::Value actionsArr = matjson::Value::array();
        for (size_t i = 0; i < actions.size(); i++) {
            const auto& a = actions[i];
            matjson::Value item = matjson::Value::object();
            item["index"]           = static_cast<int>(i);
            item["frame"]           = a.frame;
            item["type"]            = (a.type == ActionType::Press) ? 1 : 2;
            item["type_label"]      = actionTypeToString(a.type);
            item["button"]          = a.button;
            item["player1"]         = a.player1;
            item["x"]               = a.x;
            item["y"]               = a.y;
            item["analyzed"]        = a.analyzed;
            item["backward_frames"] = a.backwardFrames;
            item["forward_frames"]  = a.forwardFrames;
            item["window_size"]     = a.windowSize;
            actionsArr.push(std::move(item));
        }
        root["actions"] = std::move(actionsArr);

        (void)file::writeString(getLevelJsonPath(level), root.dump());
    }

    inline bool loadFromJson(
        GJGameLevel* level,
        bool& outAnalyzed,
        std::vector<MacroAction>& outActions,
        int (&tally)[MAX_BUCKET + 2]
    ) {
        outActions.clear();
        outAnalyzed = false;
        std::fill(std::begin(tally), std::end(tally), 0);

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
}
