// Replay scheduling, frame fixes and practice rollback adapted from xdBot
// (Zilko and contributors; slideglide/xdBot at 054764c2), with author permission
// as conveyed by the GD Menu maintainer. See docs/replay.md.
#include "replay.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/GameToolbox.hpp>
#include <Geode/binding/DashRingObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/utils/file.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <limits>
#include <climits>
#include <map>
#include <unordered_map>

using namespace geode::prelude;

namespace {
constexpr int tickRate = 240;
constexpr auto filename = "xdbot-replay.json";

enum class Mode { idle, record, play };

struct Input {
    int frame;
    int button;
    bool player1;
    bool down;
};

struct Pose {
    CCPoint position;
    float rotation;
    double yVelocity;
    double xVelocity;

    static Pose capture(PlayerObject* p) {
        if (!p) return {};
        return {p->getPosition(), p->getRotation(), p->m_yVelocity, p->m_platformerXVelocity};
    }

    void apply(PlayerObject* p) const {
        if (!p) return;
        p->setPosition(position);
        p->setRotation(rotation);
        p->m_yVelocity = yVelocity;
        p->m_platformerXVelocity = xVelocity;
    }
};

struct FrameFix {
    int frame;
    Pose p1, p2;
    bool dual;
};

struct PlayerState {
    Pose pose;
    std::map<int, bool> held;
    bool jumpBuffered;
    bool left, right, leftFirst;
    bool grounded, onSlope, wasOnSlope;
    float slopeVelocity;
    bool dashing;
    double dashX, dashY, dashAngle, dashStartTime;
    Ref<DashRingObject> dashRing;

    static PlayerState capture(PlayerObject* p) {
        if (!p) return {};
        return {
            Pose::capture(p), p->m_holdingButtons, p->m_jumpBuffered,
            p->m_holdingLeft, p->m_holdingRight, p->m_leftPressedFirst,
            p->m_isOnGround, p->m_isOnSlope, p->m_wasOnSlope, p->m_slopeVelocity,
            p->m_isDashing, p->m_dashX, p->m_dashY, p->m_dashAngle, p->m_dashStartTime, p->m_dashRing
        };
    }

    void apply(PlayerObject* p) const {
        if (!p) return;
        pose.apply(p);
        p->m_holdingButtons = held;
        p->m_jumpBuffered = jumpBuffered;
        p->m_holdingLeft = left;
        p->m_holdingRight = right;
        p->m_leftPressedFirst = leftFirst;
        p->m_isOnGround = grounded;
        p->m_isOnSlope = onSlope;
        p->m_wasOnSlope = wasOnSlope;
        p->m_slopeVelocity = slopeVelocity;
        p->m_isDashing = dashing;
        p->m_dashX = dashX;
        p->m_dashY = dashY;
        p->m_dashAngle = dashAngle;
        p->m_dashStartTime = dashStartTime;
        p->m_dashRing = dashRing.data();
    }
};

struct Checkpoint {
    size_t inputs, fixes;
    int frame;
    uint64_t seed;
    PlayerState p1, p2;
    bool dual;
};

struct Macro {
    std::string level;
    uint64_t seed = 0;
    std::vector<Input> inputs;
    std::vector<FrameFix> fixes;
};

struct Session {
    Mode mode = Mode::idle;
    Mode pending = Mode::idle;
    PlayLayer* owner = nullptr;
    Macro macro;
    std::unordered_map<CheckpointObject*, Checkpoint> checkpoints;
    size_t nextInput = 0;
    size_t nextFix = 0;
    int frame = 0;
    int previousFrame = -1;
    bool resetting = false;
    bool processing = false;
    bool restored = false;
    std::string status = "Ready";
    std::string loadedName;
} s;

std::string levelKey(PlayLayer* pl) {
    return pl && pl->m_level
        ? fmt::format("{}:{}", static_cast<int>(pl->m_level->m_levelID), pl->m_level->m_levelName)
        : "";
}

bool playingTick(PlayLayer* pl) {
    return pl && !pl->m_hasCompletedLevel && !pl->m_isPaused
        && pl->m_player1 && !pl->m_player1->m_isDead
        && (!pl->m_gameState.m_isDualMode || (pl->m_player2 && !pl->m_player2->m_isDead));
}

void resetClock() {
    s.frame = 0;
    s.previousFrame = -1;
    s.nextInput = s.nextFix = 0;
}

void releaseButtons(PlayLayer* pl) {
    if (!pl) return;
    pl->m_queuedButtons.clear();
    if (pl->m_player1) pl->m_player1->releaseAllButtons();
    if (pl->m_player2) pl->m_player2->releaseAllButtons();
}

matjson::Value writePose(Pose const& pose) {
    auto out = matjson::Value::array();
    out.push(pose.position.x);
    out.push(pose.position.y);
    out.push(pose.rotation);
    out.push(pose.yVelocity);
    out.push(pose.xVelocity);
    return out;
}

bool readPose(matjson::Value const& in, Pose& pose) {
    if (!in.isArray() || in.size() != 5) return false;
    double numbers[5];
    for (size_t i = 0; i < 5; ++i) {
        numbers[i] = in[i].asDouble().unwrapOr(std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(numbers[i]) || std::abs(numbers[i]) > 1e9) return false;
    }
    pose = {{static_cast<float>(numbers[0]), static_cast<float>(numbers[1])},
        static_cast<float>(numbers[2]), numbers[3], numbers[4]};
    return true;
}

asp::fs::Path libraryDirectory() {
    return Mod::get()->getSaveDir() / "macros";
}

void save(std::string name = {}) {
    if (s.macro.inputs.empty()) { s.status = "No inputs to save"; return; }
    if (name.empty()) {
        const auto separator = s.macro.level.find(':');
        name = s.macro.level.substr(separator == std::string::npos ? 0 : separator + 1);
    }
    for (auto& ch : name) {
        if (static_cast<unsigned char>(ch) < 32 || std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos) {
            ch = '_';
        }
    }
    if (name.empty()) name = "Replay";
    const auto directory = libraryDirectory();
    if (!directory.exists() && !utils::file::createDirectoryAll(directory)) {
        s.status = "Cannot create macro library";
        return;
    }
    // The prefix also keeps Windows device names from becoming file names.
    const std::string base = "Macro - " + name;
    std::string unique = base;
    for (unsigned i = 2; (directory / (unique + ".json")).exists(); ++i) {
        unique = fmt::format("{} ({})", base, i);
    }
    auto out = matjson::Value();
    out["version"] = 1;
    out["tps"] = tickRate;
    out["level"] = s.macro.level;
    out["seed"] = std::to_string(s.macro.seed);
    out["inputs"] = matjson::Value::array();
    out["fixes"] = matjson::Value::array();
    for (auto const& input : s.macro.inputs) {
        auto item = matjson::Value::array();
        item.push(input.frame);
        item.push(input.button);
        item.push(input.player1);
        item.push(input.down);
        out["inputs"].push(std::move(item));
    }
    for (auto const& fix : s.macro.fixes) {
        auto item = matjson::Value::array();
        item.push(fix.frame);
        item.push(writePose(fix.p1));
        item.push(writePose(fix.p2));
        item.push(fix.dual);
        out["fixes"].push(std::move(item));
    }
    const auto result = utils::file::writeStringSafe(directory / (unique + ".json"), out.dump());
    s.status = result ? "Saved" : "Save failed";
    if (result) {
        s.loadedName = unique.substr(8);
        log::info("Replay saved: {} inputs, {} frame fixes", s.macro.inputs.size(), s.macro.fixes.size());
    }
}

bool decode(matjson::Value const& in, Macro& macro) {
    if (in["version"].asInt().unwrapOr(0) != 1 || in["tps"].asInt().unwrapOr(0) != tickRate
        || !in["inputs"].isArray() || !in["fixes"].isArray()) return false;
    macro.level = in["level"].asString().unwrapOr("");
    if (macro.level.empty()) return false;
    auto seed = utils::numFromString<uint64_t>(in["seed"].asString().unwrapOr(""));
    if (!seed) return false;
    macro.seed = seed.unwrap();
    int last = 0;
    for (auto const& item : in["inputs"]) {
        if (!item.isArray() || item.size() != 4) return false;
        const auto frame = item[0].asInt().unwrapOr(-1);
        const auto button = item[1].asInt().unwrapOr(0);
        auto p1 = item[2].asBool();
        auto down = item[3].asBool();
        if (frame < last || frame > INT_MAX || button < 1 || button > 3 || !p1 || !down) return false;
        last = static_cast<int>(frame);
        macro.inputs.push_back({last, static_cast<int>(button), p1.unwrap(), down.unwrap()});
    }
    last = -1;
    for (auto const& item : in["fixes"]) {
        if (!item.isArray() || item.size() != 4) return false;
        const auto frame = item[0].asInt().unwrapOr(-1);
        auto dual = item[3].asBool();
        FrameFix fix{};
        if (frame <= last || frame > INT_MAX || !dual || !readPose(item[1], fix.p1)
            || !readPose(item[2], fix.p2)) return false;
        fix.frame = last = static_cast<int>(frame);
        fix.dual = dual.unwrap();
        macro.fixes.push_back(fix);
    }
    return !macro.inputs.empty();
}

void load(asp::fs::Path const& path, std::string name) {
    auto result = utils::file::readJson(path);
    if (!result) { s.status = "Cannot read saved replay"; return; }
    Macro macro;
    if (!decode(result.unwrap(), macro)) { s.status = "Invalid replay"; return; }
    s.macro = std::move(macro);
    s.loadedName = std::move(name);
    s.status = "Loaded";
}

// xdBot records button transitions at the point the game consumes them.
// Ignore repeated presses and releases that have no matching press.
void recordMacroInput(Input input) {
    auto& inputs = s.macro.inputs;
    for (auto it = inputs.rbegin(); it != inputs.rend(); ++it) {
        if (it->button != input.button || it->player1 != input.player1) continue;
        if (it->down == input.down) return;
        inputs.push_back(input);
        return;
    }
    if (input.down) inputs.push_back(input);
}

void finish(PlayLayer* pl) {
    const auto mode = s.mode;
    s.pending = s.mode = Mode::idle;
    s.checkpoints.clear();
    s.owner = nullptr;
    if (mode == Mode::record) save();
    if (mode == Mode::play) {
        releaseButtons(pl);
        s.status = "Playback stopped";
    }
}
} // namespace

bool replay::active() {
    return s.mode != Mode::idle || s.pending != Mode::idle;
}

bool replay::playing() {
    return s.mode == Mode::play;
}

void replay::beforeUpdate() {
    if (s.pending == Mode::idle) return;
    auto* pl = PlayLayer::get();
    if (!pl || pl != s.owner) { finish(nullptr); return; }
    if (pl->m_isPaused) return;
    if (s.pending == Mode::play && pl->m_isPracticeMode) {
        finish(nullptr);
        s.status = "Switch to Normal Mode";
        return;
    }
    s.mode = s.pending;
    s.pending = Mode::idle;
    s.checkpoints.clear();
    resetClock();
    if (s.mode == Mode::record) {
        s.macro = {};
        s.loadedName.clear();
        s.macro.level = levelKey(pl);
    }
    releaseButtons(pl);
    pl->resetLevelFromStart();
    s.status = s.mode == Mode::record ? "Recording" : "Playing";
}

void replay::draw() {
    auto* pl = PlayLayer::get();
    if (active()) {
        if (ImGui::Button("Stop", ImVec2(-FLT_MIN, 0))) finish(pl);
    } else {
        ImGui::BeginDisabled(!pl || levelKey(pl).empty());
        if (ImGui::Button("Record from start", ImVec2(-FLT_MIN, 0))) {
            if (s.macro.inputs.empty()) {
                s.owner = pl;
                s.pending = Mode::record;
                s.status = "Close menu to start";
            } else {
                ImGui::OpenPopup("Replace replay?");
            }
        }
        if (ImGui::BeginPopup("Replace replay?")) {
            ImGui::TextUnformatted("Start a new replay?");
            if (ImGui::Button("Record")) {
                s.owner = pl;
                s.pending = Mode::record;
                s.status = "Close menu to start";
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        const bool matches = pl && !s.macro.inputs.empty() && levelKey(pl) == s.macro.level;
        ImGui::BeginDisabled(!matches || pl->m_isPracticeMode);
        if (ImGui::Button("Play from start", ImVec2(-FLT_MIN, 0))) {
            s.owner = pl;
            s.pending = Mode::play;
            s.status = "Close menu to start";
        }
        ImGui::EndDisabled();
        if (pl && pl->m_isPracticeMode && matches) ImGui::TextWrapped("Play in Normal Mode");
        ImGui::BeginDisabled(s.macro.inputs.empty());
        if (ImGui::Button("Save as")) ImGui::OpenPopup("Save macro");
        if (ImGui::BeginPopup("Save macro")) {
            static char name[81]{};
            ImGui::InputTextWithHint("##macro-name", "Macro name", name, sizeof(name));
            if (ImGui::Button("Save")) {
                save(name);
                name[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Library")) ImGui::OpenPopup("Macro library");
        ImGui::SetNextWindowSize(ImVec2(440.f, 320.f), ImGuiCond_Appearing);
        if (ImGui::BeginPopup("Macro library")) {
            struct Entry {
                asp::fs::Path path;
                std::string name;
                std::string level;
            };
            static std::vector<Entry> entries;
            static bool currentLevelOnly = true;
            if (ImGui::IsWindowAppearing()) {
                entries.clear();
                const auto add = [&](asp::fs::Path const& path, std::string name) {
                    auto json = utils::file::readJson(path);
                    if (!json) return;
                    auto level = json.unwrap()["level"].asString().unwrapOr("");
                    if (!level.empty()) entries.push_back({path, std::move(name), std::move(level)});
                };
                if (auto files = utils::file::readDirectory(libraryDirectory()); files) {
                    for (auto const& path : files.unwrap()) {
                        if (path.extension() != "json") continue;
                        const auto file = path.filename();
                        if (!file) continue;
                        auto name = std::string(file->view());
                        if (!name.starts_with("Macro - ") || !name.ends_with(".json")) continue;
                        add(path, name.substr(8, name.size() - 13));
                    }
                }
                const auto legacy = Mod::get()->getSaveDir() / filename;
                if (legacy.exists()) add(legacy, "Previous save");
                std::sort(entries.begin(), entries.end(), [](Entry const& a, Entry const& b) {
                    return a.name < b.name;
                });
            }
            const auto level = levelKey(pl);
            if (!level.empty()) ImGui::Checkbox("Current level only", &currentLevelOnly);
            bool found = false;
            for (size_t i = 0; i < entries.size(); ++i) {
                auto const& entry = entries[i];
                if (currentLevelOnly && !level.empty() && entry.level != level) continue;
                found = true;
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(entry.name.c_str())) {
                    load(entry.path, entry.name);
                    ImGui::CloseCurrentPopup();
                }
                const auto separator = entry.level.find(':');
                ImGui::SetItemTooltip("%s", entry.level.c_str() + (separator == std::string::npos ? 0 : separator + 1));
                ImGui::PopID();
            }
            if (!found) ImGui::TextDisabled("No macros to show");
            ImGui::EndPopup();
        }
    }
    if (!s.loadedName.empty()) ImGui::TextWrapped("%s", s.loadedName.c_str());
    ImGui::TextWrapped("%s", s.status.c_str());
    if (!s.macro.level.empty()) {
        const auto separator = s.macro.level.find(':');
        ImGui::TextWrapped("%s", s.macro.level.c_str() + separator + 1);
    }
    ImGui::TextWrapped("%zu inputs", s.macro.inputs.size());
}

class $modify(XdReplayPlayLayer, PlayLayer) {
    void resetLevel() {
        if (s.owner != this || s.mode == Mode::idle) return PlayLayer::resetLevel();
        s.restored = false;
        s.resetting = true;
        PlayLayer::resetLevel();
        s.resetting = false;
        if (s.restored) return;

        resetClock();
        s.checkpoints.clear();
        if (s.mode == Mode::record) {
            s.macro.inputs.clear();
            s.macro.fixes.clear();
            s.macro.seed = GameToolbox::getfast_srand();
        } else {
            GameToolbox::fast_srand(s.macro.seed);
        }
    }

    void storeCheckpoint(CheckpointObject* cp) {
        PlayLayer::storeCheckpoint(cp);
        if (!cp || s.owner != this || s.mode != Mode::record || s.resetting) return;
        const bool dual = m_gameState.m_isDualMode;
        s.checkpoints.insert_or_assign(cp, Checkpoint{
            s.macro.inputs.size(), s.macro.fixes.size(), s.frame, GameToolbox::getfast_srand(),
            PlayerState::capture(m_player1), PlayerState::capture(dual ? m_player2 : nullptr), dual});
    }

    void removeCheckpoint(bool first) {
        PlayLayer::removeCheckpoint(first);
        if (s.owner != this || !m_checkpointArray) return;
        std::erase_if(s.checkpoints, [&](auto const& item) {
            return !m_checkpointArray->containsObject(item.first);
        });
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        const bool wasResetting = s.resetting;
        const bool recording = s.owner == this && s.mode == Mode::record;
        auto it = s.checkpoints.find(cp);
        std::optional<Checkpoint> checkpoint;
        if (recording && it != s.checkpoints.end()) checkpoint = it->second;
        if (recording) s.resetting = true;
        PlayLayer::loadFromCheckpoint(cp);
        s.resetting = wasResetting;
        if (!checkpoint) return;

        auto const& state = *checkpoint;
        // Roll back the active macro, keeping the route up to this checkpoint.
        s.macro.inputs.resize(std::min(state.inputs, s.macro.inputs.size()));
        s.macro.fixes.resize(std::min(state.fixes, s.macro.fixes.size()));
        s.frame = state.frame;
        s.previousFrame = s.macro.fixes.empty() ? -1 : s.macro.fixes.back().frame;
        s.restored = true;
        state.p1.apply(m_player1);
        if (state.dual) state.p2.apply(m_player2);
        GameToolbox::fast_srand(state.seed);
        std::erase_if(s.checkpoints, [&](auto const& item) {
            return item.second.inputs > state.inputs || item.second.fixes > state.fixes;
        });
    }

    void levelComplete() {
        if (s.owner == this) finish(this);
        PlayLayer::levelComplete();
    }

    void onQuit() {
        if (s.owner == this) finish(this);
        PlayLayer::onQuit();
    }
};

class $modify(XdReplayGameLayer, GJBaseGameLayer) {
    void processQueuedButtons(float dt, bool clearInputQueue) {
        auto* pl = PlayLayer::get();
        if (static_cast<GJBaseGameLayer*>(pl) != static_cast<GJBaseGameLayer*>(this) || s.owner != pl || s.mode == Mode::idle || s.resetting || !playingTick(pl)) {
            GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
            return;
        }

        const int frame = s.frame;
        if (s.mode == Mode::play) {
            const int lastInput = s.macro.inputs.back().frame;
            const int lastFix = s.macro.fixes.empty() ? 0 : s.macro.fixes.back().frame;
            if (s.nextInput == s.macro.inputs.size() && s.nextFix == s.macro.fixes.size()
                && frame > std::max(lastInput, lastFix)) {
                finish(pl);
                s.status = "Playback finished";
                GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
                return;
            }
            m_queuedButtons.clear();
            // Match xdBot's dispatch order: queue due inputs and apply the frame fix
            // before the game consumes its button queue.
            while (s.nextInput < s.macro.inputs.size() && s.macro.inputs[s.nextInput].frame <= frame) {
                auto const& input = s.macro.inputs[s.nextInput++];
                queueButton(input.button, input.down, input.player1, 0.0);
            }
            while (s.nextFix < s.macro.fixes.size() && s.macro.fixes[s.nextFix].frame <= frame) {
                auto const& fix = s.macro.fixes[s.nextFix++];
                fix.p1.apply(m_player1);
                if (fix.dual && m_gameState.m_isDualMode) fix.p2.apply(m_player2);
            }
        }

        s.processing = true;
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
        s.processing = false;
        if (s.owner != pl || s.mode == Mode::idle || !playingTick(pl)) return;
        if (s.mode == Mode::record && frame > s.previousFrame && !s.macro.inputs.empty()) {
            const bool dual = m_gameState.m_isDualMode;
            s.macro.fixes.push_back({frame, Pose::capture(m_player1),
                Pose::capture(dual ? m_player2 : nullptr), dual});
            s.previousFrame = frame;
        }
        // xdBot's Windows frame counter derives the next tick from game time,
        // not render frames or player X.
        s.frame = std::max(frame, static_cast<int>(m_gameState.m_levelTime * tickRate) + 1);
    }

    void handleButton(bool down, int button, bool player1) {
        auto* pl = PlayLayer::get();
        const bool ours = static_cast<GJBaseGameLayer*>(pl) == static_cast<GJBaseGameLayer*>(this) && pl == s.owner && !s.resetting;
        if (ours && s.mode == Mode::play && !s.processing) return;

        GJBaseGameLayer::handleButton(down, button, player1);
        if (ours && s.mode == Mode::record && s.processing && playingTick(pl) && button >= 1 && button <= 3) {
            recordMacroInput({s.frame, button, player1, down});
        }
    }
};