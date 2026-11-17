#include "gameplay_copy.hpp"

#include <Geode/Enums.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/cocos/support/zip_support/ZipUtils.h>
#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

std::string_view takeToken(std::string_view& input, char delimiter) {
    const auto end = input.find(delimiter);
    const auto token = input.substr(0, end);
    input.remove_prefix(end == std::string_view::npos ? input.size() : end + 1);
    return token;
}

template <class Number>
Number number(std::string_view input, Number fallback = 0) {
    if (input.empty()) return fallback;
    Number value{};
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
    return error == std::errc{} && end == input.data() + input.size() ? value : fallback;
}

struct Properties {
    std::vector<std::pair<std::string_view, std::string_view>> fields;

    bool read(std::string_view data, char delimiter = ',') {
        fields.clear();
        while (!data.empty()) {
            if (data.find(delimiter) == std::string_view::npos) return false;
            const auto key = takeToken(data, delimiter);
            const auto value = takeToken(data, delimiter);
            // Some native level settings contain empty pairs before later keys.
            if (!key.empty()) fields.emplace_back(key, value);
        }
        return true;
    }

    std::string_view get(std::string_view key) const {
        const auto match = std::ranges::find(fields, key, &decltype(fields)::value_type::first);
        return match == fields.end() ? std::string_view{} : match->second;
    }

    void set(std::string_view key, std::string_view value) {
        const auto match = std::ranges::find(fields, key, &decltype(fields)::value_type::first);
        if (match == fields.end()) fields.emplace_back(key, value);
        else match->second = value;
    }

    void appendTo(std::string& output) const {
        bool first = true;
        for (const auto& [key, value] : fields) {
            if (!first) output += ',';
            output.append(key).append(",").append(value);
            first = false;
        }
        output += ';';
    }
};

bool styleScenery(Properties& settings, std::string& output) {
    // Geometry Dash's saved color channels, keyed by field 6 in kS38.
    constexpr std::string_view scenery =
        "1_0_2_0_3_0_4_-1_5_0_6_1000_7_1|"
        "1_0_2_0_3_0_4_-1_5_0_6_1001_7_1|"
        "1_255_2_255_3_255_4_-1_5_0_6_1002_7_1|"
        "1_0_2_0_3_0_4_-1_5_0_6_1009_7_1|"
        "1_0_2_0_3_0_4_-1_5_0_6_1013_7_1|"
        "1_0_2_0_3_0_4_-1_5_0_6_1014_7_1|";
    std::string colors(scenery);
    auto originalColors = settings.get("kS38");
    Properties channel;
    while (!originalColors.empty()) {
        const auto encoded = takeToken(originalColors, '|');
        if (encoded.empty()) continue;
        if (!channel.read(encoded, '_')) return false;
        const int id = number<int>(channel.get("6"));
        if (id != 1000 && id != 1001 && id != 1002 && id != 1009 && id != 1013 && id != 1014) {
            colors.append(encoded).append("|");
        }
    }
    for (auto key : {"kS1", "kS2", "kS3", "kS4", "kS5", "kS6", "kS7", "kS8", "kS9",
                     "kS16", "kS17", "kS18", "kS29", "kS30", "kS31"}) {
        std::erase_if(settings.fields, [key](const auto& item) { return item.first == key; });
    }
    settings.set("kA6", "1"); // Background
    settings.set("kA7", "1"); // Ground
    settings.set("kA25", "0"); // Middleground
    settings.set("kS38", colors);
    settings.appendTo(output);
    return true;
}

enum class ObjectKind { Gameplay, Structure, Decoration, Trigger, VisualTrigger };

ObjectKind classify(int id, GameObject* object) {
    // Pulse, shaders and tint apply even to the static white channel.
    const bool changesScenery = id == 2999 || (id >= 3029 && id <= 3031);
    const bool tintsObjects = id == 1006 || id == 3010 || id == 3015 || id == 3021;
    if (changesScenery || tintsObjects || (id >= 2904 && id <= 2919)
        || (object && object->isColorTrigger())) {
        return ObjectKind::VisualTrigger;
    }
    if (!object || id == 31 || object->m_isStartPos) return ObjectKind::Gameplay;
    if (object->isEditorSpawnableTrigger()) return ObjectKind::Trigger;

    // The factory constructs the sprite. Custom setup assigns its collision type.
    object->customSetup();
    if (object->m_isTrigger) return ObjectKind::Trigger;

    switch (object->m_objectType) {
        case GameObjectType::Decoration:
            return ObjectKind::Decoration;
        case GameObjectType::Solid:
        case GameObjectType::Slope:
        case GameObjectType::Breakable:
        case GameObjectType::Hazard:
        case GameObjectType::AnimatedHazard:
            return ObjectKind::Structure;
        default:
            return ObjectKind::Gameplay;
    }
}

template <class Callback>
void visitGroups(const Properties& properties, Callback callback) {
    const int legacyGroup = number<int>(properties.get("33"));
    if (legacyGroup > 0) callback(legacyGroup);
    auto groups = properties.get("57");
    while (!groups.empty()) {
        const int id = number<int>(takeToken(groups, '.'));
        if (id > 0) callback(id);
    }
}

void colorStructure(Properties& object) {
    // Reveal hidden collision geometry and apply the static white channel.
    std::erase_if(object.fields, [](const auto& item) {
        const auto key = item.first;
        return key == "19" || key == "41" || key == "42" || key == "43" || key == "44" || key == "135";
    });
    object.set("21", "1011");
    object.set("22", "1011");
    object.set("96", "1"); // No glow
}

} // namespace

std::optional<GameplayCopy> makeGameplayCopy(const PlayLayer& play) {
    if (!play.m_level) return std::nullopt;
    const auto& source = play.m_level->m_levelString;
    const std::string data = source.contains(';') ? source : cocos2d::ZipUtils::decompressString(source, false, 0);
    if (!data.contains(';')) return std::nullopt;

    std::string_view remaining(data);
    Properties properties;
    if (!properties.read(takeToken(remaining, ';')) || properties.fields.empty()) return std::nullopt;
    std::unordered_set<int> positionGroups;
    const int spawnGroup = number<int>(properties.get("kA36"));
    if (spawnGroup > 0) positionGroups.insert(spawnGroup);

    GameplayCopy result;
    result.data.reserve(data.size());
    if (!styleScenery(properties, result.data)) return std::nullopt;

    std::unordered_map<int, ObjectKind> kinds;
    std::unordered_set<int> gameplayGroups;
    std::vector<std::pair<std::string_view, ObjectKind>> objects;
    std::string_view farthestObject;
    double farthestX = -std::numeric_limits<double>::infinity();
    while (!remaining.empty()) {
        const auto encoded = takeToken(remaining, ';');
        if (encoded.empty()) continue;
        if (!properties.read(encoded)) return std::nullopt;
        const int id = number<int>(properties.get("1"));
        if (id <= 0) return std::nullopt;

        auto [entry, inserted] = kinds.try_emplace(id);
        if (inserted) entry->second = classify(id, GameObject::createWithKey(id));
        auto kind = entry->second;
        if (kind == ObjectKind::Structure && properties.get("121") == "1") {
            kind = ObjectKind::Decoration; // NoTouch is set per placed object.
        }
        objects.emplace_back(encoded, kind);
        if (kind == ObjectKind::VisualTrigger) continue;
        const int target = number<int>(properties.get("51"));
        if (target > 0 && (id == 1916 || id == 3022 || id == 3027)) {
            positionGroups.insert(target); // Static camera and teleport destinations
        }
        if (kind == ObjectKind::Trigger) {
            const int position = number<int>(properties.get("71"));
            if (position > 0) positionGroups.insert(position);
            continue;
        }
        if (kind != ObjectKind::Decoration) {
            visitGroups(properties, [&](int group) { gameplayGroups.insert(group); });
        }
        const auto x = number<double>(properties.get("2"), farthestX);
        if (x > farthestX || (x == farthestX && kind != ObjectKind::Decoration)) {
            farthestX = x;
            farthestObject = encoded;
        }
    }

    for (const auto& [encoded, kind] : objects) {
        if (kind == ObjectKind::VisualTrigger) continue;
        if (kind == ObjectKind::Decoration || kind == ObjectKind::Structure) {
            properties.read(encoded);
            if (kind == ObjectKind::Decoration) {
                // Keep invisible anchors for grouped motion and classic level length.
                bool needed = encoded.data() == farthestObject.data();
                const bool groupParent = properties.get("34") == "1";
                visitGroups(properties, [&](int group) {
                    needed |= positionGroups.contains(group)
                        || (groupParent && gameplayGroups.contains(group));
                });
                if (!needed) continue;
                properties.set("135", "1"); // Hide in play mode
                properties.set("96", "1");
            } else {
                colorStructure(properties);
            }
            properties.appendTo(result.data);
        } else {
            result.data.append(encoded).append(";");
        }
        ++result.objectCount;
    }

    result.data = cocos2d::ZipUtils::compressString(result.data, false, 6, 0);
    if (result.data.empty()) return std::nullopt;
    return result;
}
