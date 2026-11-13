#define NOMINMAX
#include <windows.h>

#include <Geode/Geode.hpp>
#include <Geode/Enums.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/PlatformToolbox.hpp>
#include <Geode/binding/UILayer.hpp>
#include <Geode/binding/StartPosObject.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/LocalLevelManager.hpp>
#include <Geode/loader/Event.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/utils/cocos.hpp>
#include <Geode/utils/Keyboard.hpp>
#include "replay.hpp"
#include <imgui-cocos.hpp>
#include <imgui.h>
#include <fmt/format.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <array>
#include <chrono>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>

using namespace geode::prelude;

struct Features {
    bool safeMode = false;
    bool autoSafeMode = true;
    bool noclip = false;
    bool accuracy = false;
    bool noclipDeaths = false;
    bool trajectory = false;
    bool speedhack = false;
    bool autoclicker = false;
    bool showFps = false;
    bool showHitboxes = false;
    bool showBestRun = false;
    bool showClicks = false;
    bool showSessionTime = false;
    bool hidePlayer = false;
    bool practiceMusic = false;
    bool noParticles = false;
    float speed = 1.5f;
    float menuScale = 1.2f;
    ccColor3B flashColor{255, 32, 32};
    int menuKey = VK_TAB;
};

static Features F;
static bool g_menuOpen = false;
static bool g_capturing = false;
static bool s_inLevel = false;
static bool s_pausedLevel = false;
static bool s_gameWasPaused = false;
static PlayLayer* s_levelLayer = nullptr;

static void loadFeatures() {
    auto* mod = Mod::get();
    F.safeMode = mod->getSavedValue<bool>("safe_mode", false);
    F.autoSafeMode = mod->getSavedValue<bool>("auto_safe_mode", true);
    F.noclip = mod->getSavedValue<bool>("noclip", false);
    F.accuracy = mod->getSavedValue<bool>("noclip_accuracy", false);
    F.noclipDeaths = mod->getSavedValue<bool>("noclip_deaths", F.accuracy);
    F.trajectory = mod->getSavedValue<bool>("trajectory", false);
    F.speedhack = mod->getSavedValue<bool>("speedhack", false);
    F.autoclicker = mod->getSavedValue<bool>("autoclicker", false);
    F.showFps = mod->getSavedValue<bool>("show_fps", false);
    F.showHitboxes = mod->getSavedValue<bool>("show_hitboxes", false);
    F.showBestRun = mod->getSavedValue<bool>("show_best_run", false);
    F.showClicks = mod->getSavedValue<bool>("show_clicks", false);
    F.showSessionTime = mod->getSavedValue<bool>("show_session_time", false);
    F.hidePlayer = mod->getSavedValue<bool>("hide_player", false);
    F.practiceMusic = mod->getSavedValue<bool>("practice_music", false);
    F.noParticles = mod->getSavedValue<bool>("no_particles", false);
    F.speed = mod->getSavedValue<float>("speed_mult", 1.5f);
    const float menuScale = mod->getSavedValue<float>("menu_scale", 1.2f);
    F.menuScale = std::isfinite(menuScale) ? std::clamp(menuScale, 0.8f, 2.f) : 1.2f;
    F.menuKey = mod->getSavedValue<int>("menu_key", VK_TAB);

    int color = mod->getSavedValue<int>("flash_color", 0xFF2020);
    F.flashColor = {
        static_cast<GLubyte>((color >> 16) & 0xFF),
        static_cast<GLubyte>((color >> 8) & 0xFF),
        static_cast<GLubyte>(color & 0xFF),
    };

    if (F.speed < 0.1f) F.speed = 0.1f;
    if (F.speed > 5.f) F.speed = 5.f;
}

static void saveFeatures() {    auto* mod = Mod::get();
    mod->setSavedValue("safe_mode", F.safeMode);
    mod->setSavedValue("auto_safe_mode", F.autoSafeMode);
    mod->setSavedValue("noclip", F.noclip);
    mod->setSavedValue("noclip_accuracy", F.accuracy);
    mod->setSavedValue("noclip_deaths", F.noclipDeaths);
    mod->setSavedValue("trajectory", F.trajectory);
    mod->setSavedValue("speedhack", F.speedhack);
    mod->setSavedValue("autoclicker", F.autoclicker);
    mod->setSavedValue("show_fps", F.showFps);
    mod->setSavedValue("show_hitboxes", F.showHitboxes);
    mod->setSavedValue("show_best_run", F.showBestRun);
    mod->setSavedValue("show_clicks", F.showClicks);
    mod->setSavedValue("show_session_time", F.showSessionTime);
    mod->setSavedValue("hide_player", F.hidePlayer);
    mod->setSavedValue("practice_music", F.practiceMusic);
    mod->setSavedValue("no_particles", F.noParticles);
    mod->setSavedValue("speed_mult", F.speed);
    mod->setSavedValue("menu_scale", F.menuScale);
    mod->setSavedValue("menu_key", F.menuKey);
    mod->setSavedValue<int>(
        "flash_color",
        (F.flashColor.r << 16) | (F.flashColor.g << 8) | F.flashColor.b);
}

static PlayLayer* s_accOwner = nullptr;
static int s_accFrames = 0;
static int s_accBad = 0;
static int s_accDeaths = 0;
static bool s_accBadFrame = false;
static CCScene* s_accScene = nullptr;
static CCLabelBMFont* s_accLabel = nullptr;
static CCLabelBMFont* s_deathsLabel = nullptr;
static constexpr GLubyte metricOpacity = 110;

static bool showNoclipStats() { return F.accuracy || F.noclipDeaths; }
static bool s_autoSafeAttempt = false;
static GJGameLevel* s_protectedQuitLevel = nullptr;

static bool autoSafeRequired() {
    return F.autoSafeMode && (F.noclip || showNoclipStats() || F.speedhack
        || F.autoclicker || F.trajectory || replay::playing());
}

static bool safeModeActive() {
    return F.safeMode || s_autoSafeAttempt || autoSafeRequired();
}
static std::unordered_set<GameObject*> s_accHitObjects;
static int s_lastAttempts = 0;

static CCLayerColor* s_flash = nullptr;
static CCScene* s_flashScene = nullptr;

static void doFlash() {
    auto* scene = CCDirector::sharedDirector()->getRunningScene();
    if (!scene) {
        return;
    }
    if (s_flashScene != scene) {
        s_flashScene = scene;
        s_flash = nullptr;
    }
    if (!s_flash) {
        s_flash = CCLayerColor::create(ccc4(255, 255, 255, 0));
        s_flash->setZOrder(998);
        scene->addChild(s_flash);
    }
    s_flash->setColor(F.flashColor);
    s_flash->setOpacity(80);
    s_flash->stopAllActions();
    s_flash->runAction(CCFadeTo::create(0.3f, 0));
}

static void resetAccuracy() {
    s_accFrames = 0;
    s_accBad = 0;
    s_accDeaths = 0;
    s_accBadFrame = false;
    s_accHitObjects.clear();
}

static void applyAudioPitch(float pitch) {
    static float last = -1.f;
    if (std::abs(pitch - last) < 0.001f) {
        return;
    }
    last = pitch;

    auto* engine = FMODAudioEngine::sharedEngine();
    if (!engine || !engine->m_system) {
        return;
    }
    FMOD::ChannelGroup* master = nullptr;
    if (engine->m_system->getMasterChannelGroup(&master) == FMOD_OK && master) {
        master->setPitch(pitch);
    }
}

static void setMenuVisible(bool visible) {
    if (g_menuOpen == visible) {
        return;
    }
    g_menuOpen = visible;

    auto& imgui = ImGuiCocos::get();
    if (imgui.isInitialized()) {
        imgui.setInputMode(
            visible ? ImGuiCocos::InputMode::Blocking : ImGuiCocos::InputMode::Default);
        imgui.setVisible(true);
    }

    if (visible) {
        PlatformToolbox::showCursor();
        PlatformToolbox::toggleLockCursor(false);

        if (s_inLevel && s_levelLayer && !s_levelLayer->m_isPaused) {
            s_pausedLevel = true;
            if (auto* scheduler = CCDirector::sharedDirector()->getScheduler()) {
                scheduler->pauseTarget(s_levelLayer);
            }
            if (auto* audio = FMODAudioEngine::sharedEngine()) {
                audio->pauseAllMusic(true);
            }
        }
    } else {
        if (s_pausedLevel) {
            if (s_levelLayer) {
                if (auto* scheduler = CCDirector::sharedDirector()->getScheduler()) {
                    scheduler->resumeTarget(s_levelLayer);
                }
            }
            if (auto* audio = FMODAudioEngine::sharedEngine()) {
                audio->resumeAllMusic();
            }
            PlatformToolbox::hideCursor();
            PlatformToolbox::toggleLockCursor(true);
            s_pausedLevel = false;
        }
    }
}

static bool handleMenuKey(KeyboardInputData& event) {
    using Action = KeyboardInputData::Action;
    const auto key = event.native.code;
    if (g_capturing && event.action == Action::Press && key >= 8 && key < 256) {
        F.menuKey = static_cast<int>(key);
        g_capturing = false;
        saveFeatures();
        return true;
    }
    if (key != static_cast<unsigned>(F.menuKey)) {
        return false;
    }
    if (event.action == Action::Press) {
        setMenuVisible(!g_menuOpen);
    }
    // The menu hotkey never reaches ImGui's focus navigation or the game.
    return true;
}
static const char* keyName(int vk) {
    auto* dispatcher = CCKeyboardDispatcher::get();
    if (!dispatcher) {
        return "?";
    }
    auto* name = dispatcher->keyToString(static_cast<enumKeyCodes>(vk));
    return name ? name : "?";
}

static bool featureRow(const char* label, bool* value) {
    if (ImGui::Selectable(label, *value)) {
        *value = !*value;
        return true;
    }
    return false;
}

static void drawMenuSettings(bool& changed) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Menu key");
    ImGui::SameLine();
    if (ImGui::Button(g_capturing ? "press..." : keyName(F.menuKey))) {
        g_capturing = true;
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    changed |= ImGui::SliderFloat(
        "##interface-scale", &F.menuScale, 0.8f, 2.f, "Interface Scale %.1fx");
}

static void drawSpeedhack(bool& changed) {
    changed |= featureRow("Enabled", &F.speedhack);

    const float fieldWidth = ImGui::CalcTextSize("0.00x").x + ImGui::GetStyle().FramePadding.x * 2.f;
    ImGui::SetNextItemWidth(
        ImGui::GetContentRegionAvail().x - fieldWidth - ImGui::GetStyle().ItemSpacing.x);
    changed |= ImGui::SliderFloat(
        "##speed-slider", &F.speed, 0.1f, 5.f, "", ImGuiSliderFlags_NoInput);
    ImGui::SameLine();

    ImGui::SetNextItemWidth(fieldWidth);
    ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop | ImGuiItemFlags_NoNavDefaultFocus, true);
    float typed = F.speed;
    if (ImGui::InputFloat("##speed-value", &typed, 0.f, 0.f, "%.2fx") && std::isfinite(typed)) {
        const float speed = std::clamp(typed, 0.1f, 5.f);
        changed |= speed != F.speed;
        F.speed = speed;
    }
    ImGui::PopItemFlag();
}

static void drawCosmetics(bool& changed) {
    changed |= featureRow("Hide Player", &F.hidePlayer);
    changed |= featureRow("No Particles", &F.noParticles);
}

static void drawLocalCopy() {
    auto* play = PlayLayer::get();
    auto* level = play ? play->m_level : nullptr;
    const bool canCopy = level && !level->m_levelString.empty();
    ImGui::BeginDisabled(!canCopy);
    if (ImGui::Button("Copy to My Levels", ImVec2(-FLT_MIN, 0.f))) {
        ImGui::OpenPopup("Create level copy");
    }
    ImGui::EndDisabled();

    static std::string result;
    if (ImGui::BeginPopup("Create level copy")) {
        ImGui::TextUnformatted("Create an editable local copy?");
        if (canCopy && ImGui::Button("Create copy")) {
            auto* levels = GameLevelManager::sharedState();
            if (auto* copy = levels->createNewLevel()) {
                copy->copyLevelInfo(level);
                copy->m_levelName = "Copy of " + level->m_levelName;
                copy->m_password = 0;
                const int original = level->m_originalLevel.value();
                copy->m_originalLevel = original != 0 ? original : level->m_levelID.value();
                LocalLevelManager::sharedState()->save(false);
                result = "Copied to My Levels";
            } else {
                result = "Could not create a copy";
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!result.empty()) ImGui::TextWrapped("%s", result.c_str());
}

static void drawGameplay(bool& changed) {
    changed |= featureRow("Auto Clicker", &F.autoclicker);
    changed |= featureRow("Noclip", &F.noclip);
    changed |= featureRow("Safe Mode", &F.safeMode);
    changed |= featureRow("Auto Safe Mode", &F.autoSafeMode);
    if (PlayLayer::get() && safeModeActive()) ImGui::TextDisabled("Progress protected");
    ImGui::SetItemTooltip("Play without saving level progress or rewards.");
    changed |= featureRow("Practice Music", &F.practiceMusic);
    changed |= featureRow("Show Hitboxes", &F.showHitboxes);
    changed |= featureRow("Dual Trajectory (Beta)", &F.trajectory);
    ImGui::SetItemTooltip("Classic modes: cyan = release, pink = hold. Red marks a possible hazard contact.");
    drawLocalCopy();
}

static void drawLabels(bool& changed) {
    changed |= featureRow("FPS Counter", &F.showFps);
    changed |= featureRow("Best Run", &F.showBestRun);
    changed |= featureRow("CPS / Attempt Clicks", &F.showClicks);
    changed |= featureRow("Session Time", &F.showSessionTime);
    changed |= featureRow("Noclip Accuracy", &F.accuracy);
    changed |= featureRow("Noclip Deaths", &F.noclipDeaths);

    float color[3] = {
        F.flashColor.r / 255.f,
        F.flashColor.g / 255.f,
        F.flashColor.b / 255.f,
    };
    ImGui::SetNextItemWidth(ImGui::GetFrameHeight());
    if (ImGui::ColorEdit3("Flash Color", color, ImGuiColorEditFlags_NoInputs)) {
        F.flashColor = {
            static_cast<GLubyte>(color[0] * 255.f),
            static_cast<GLubyte>(color[1] * 255.f),
            static_cast<GLubyte>(color[2] * 255.f),
        };
        changed = true;
    }
}

static void drawReplay(bool&) { replay::draw(); }

struct MenuPanel {
    const char* title;
    void (*draw)(bool&);
    bool expanded = true;
};

static void drawPanel(MenuPanel& panel, float width, bool& changed) {
    if (ImGui::BeginChild(panel.title, ImVec2(width, 0.f),
        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders)) {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(234, 54, 102, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(249, 73, 119, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(214, 42, 88, 255));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32_WHITE);
        if (ImGui::Button(panel.title, ImVec2(-FLT_MIN, 0.f))) {
            panel.expanded = !panel.expanded;
        }
        const auto start = ImGui::GetItemRectMin();
        const auto padding = ImGui::GetStyle().FramePadding;
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(start.x + padding.x, start.y + padding.y),
            IM_COL32_WHITE, panel.expanded ? "-" : "+");
        ImGui::PopStyleColor(4);

        if (panel.expanded) {
            panel.draw(changed);
        }
    }
    ImGui::EndChild();
}

static void applyMenuStyle(float scale) {
    auto& style = ImGui::GetStyle();
    style = ImGuiStyle{};
    style.WindowPadding = ImVec2(4.f, 2.f);
    style.FramePadding = ImVec2(4.f, 2.f);
    style.ItemSpacing = ImVec2(4.f, 1.f);
    style.ItemInnerSpacing = ImVec2(3.f, 2.f);
    style.ChildBorderSize = 1.f;
    style.WindowBorderSize = 0.f;
    style.ScrollbarSize = 7.f;
    style.GrabMinSize = 8.f;

    auto* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImColor(13, 14, 15, 239);
    colors[ImGuiCol_ChildBg] = ImColor(40, 40, 40);
    colors[ImGuiCol_Border] = ImColor(58, 58, 58);
    colors[ImGuiCol_Text] = ImColor(217, 217, 217);
    colors[ImGuiCol_TextDisabled] = ImColor(171, 171, 171);
    colors[ImGuiCol_FrameBg] = ImColor(28, 28, 28);
    colors[ImGuiCol_FrameBgHovered] = ImColor(62, 53, 55);
    colors[ImGuiCol_FrameBgActive] = ImColor(79, 53, 60);
    colors[ImGuiCol_Header] = ImColor(87, 45, 56);
    colors[ImGuiCol_HeaderHovered] = ImColor(66, 54, 57);
    colors[ImGuiCol_HeaderActive] = ImColor(124, 51, 72);
    colors[ImGuiCol_Button] = ImColor(32, 32, 32);
    colors[ImGuiCol_ButtonHovered] = ImColor(68, 52, 56);
    colors[ImGuiCol_ButtonActive] = ImColor(102, 49, 65);
    colors[ImGuiCol_SliderGrab] = ImColor(234, 54, 102);
    colors[ImGuiCol_SliderGrabActive] = ImColor(249, 73, 119);
    colors[ImGuiCol_TextSelectedBg] = ImColor(128, 49, 74);
    colors[ImGuiCol_NavCursor] = colors[ImGuiCol_SliderGrab];

    style.ScaleAllSizes(scale);
    style.FontScaleMain = scale;
    style.FontSizeBase = 11.f;
}

static void drawMenu() {
    if (!g_menuOpen) {
        return;
    }

    static MenuPanel panels[] = {
        {"Menu", drawMenuSettings},
        {"Speedhack", drawSpeedhack},
        {"Cosmetic", drawCosmetics},
        {"Gameplay", drawGameplay},
        {"Labels", drawLabels},
        {"Replay", drawReplay},
    };

    const auto display = ImGui::GetIO().DisplaySize;
    const float scale = std::clamp(display.x / 1400.f, 0.85f, 2.f) * (F.menuScale / 1.2f);
    if (ImGui::GetStyle().FontScaleMain != scale) {
        applyMenuStyle(scale);
    }

    bool changed = false;
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(display);
    if (ImGui::Begin("##gdmenu-panels", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        const float inset = 12.f * scale;
        const float available = ImGui::GetContentRegionAvail().x - inset;
        const float width = std::min(132.f * scale, available);
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const int columns = std::max(1, static_cast<int>((available + spacing) / (width + spacing)));
        int index = 0;
        for (auto& panel : panels) {
            if (index % columns == 0) {
                ImGui::SetCursorPosX(inset);
            } else {
                ImGui::SameLine();
            }
            drawPanel(panel, width, changed);
            ++index;
        }
    }
    ImGui::End();
    if (changed) {
        saveFeatures();
    }
}

$on_mod(Loaded) {
    loadFeatures();
    KeyboardInputEvent().listen(handleMenuKey, -10).leak();

    auto& imgui = ImGuiCocos::get();
    imgui.setup([]() {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        char windowsDir[MAX_PATH];
        auto length = GetWindowsDirectoryA(windowsDir, MAX_PATH);
        if (length && length < MAX_PATH) {
            auto fontPath = fmt::format("{}/Fonts/segoeui.ttf", windowsDir);
            if (GetFileAttributesA(fontPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                io.FontDefault = io.Fonts->AddFontFromFileTTF(fontPath.c_str());
            }
        }
        applyMenuStyle(1.f);
    });
    imgui.draw(drawMenu);
    imgui.setVisible(true);
}


class $modify(MyMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) {
            return false;
        }
        s_inLevel = false;
        applyAudioPitch(1.0f);
        log::info("gdmenu: main menu ready");
        return true;
    }
};

using StatsClock = std::chrono::steady_clock;

struct SessionStats {
    double seconds = 0.0;
    std::unordered_map<std::string, float> bestByLevel;
};

static SessionStats s_sessionStats;

struct AttemptStats {
    Ref<CCLabelBMFont> label;
    float* best = nullptr;
    StartPosObject* bestStart = nullptr;
    std::array<bool, 2> held{};
    std::deque<StatsClock::time_point> clicks;
    unsigned totalClicks = 0;
    bool forcedHitboxes = false;

    void resetClicks() {
        held = {};
        clicks.clear();
        totalClicks = 0;
    }
};

static AttemptStats s_attemptStats;

static std::string bestRunLevelKey(GJGameLevel& level) {
    const int id = level.m_levelID;
    if (level.m_levelType == GJLevelType::Main) {
        return fmt::format("main:{}", id);
    }
    if (level.m_levelType != GJLevelType::Editor && id > 0) {
        return fmt::format("online:{}", id);
    }
    if (level.m_levelType == GJLevelType::Editor && level.m_levelIndex != 0) {
        return fmt::format("editor:{}", level.m_levelIndex);
    }
    return fmt::format("local:{}:{}", level.m_levelName, std::hash<std::string>{}(level.m_levelString));
}

static void updateRunBest(PlayLayer* play, float percent) {
    if (!play->m_level || !std::isfinite(percent)) return;
    auto* start = play->m_startPosObject;
    if (!s_attemptStats.best || s_attemptStats.bestStart != start) {
        auto key = bestRunLevelKey(*play->m_level);
        if (start) {
            const auto point = start->getPosition();
            const auto index = play->m_startPosObjects ? play->m_startPosObjects->indexOfObject(start) : 0;
            key += fmt::format(":start:{}:{:.3f}:{:.3f}", index, point.x, point.y);
        }
        s_attemptStats.bestStart = start;
        s_attemptStats.best = &s_sessionStats.bestByLevel[key];
    }
    auto& best = *s_attemptStats.best;
    best = std::max(best, std::clamp(percent, 0.f, 100.f));
}

static void updateSessionStats(PlayLayer* play, float dt) {
    if (play->m_started && !play->m_isPaused && !play->m_hasCompletedLevel) {
        s_sessionStats.seconds += std::max(dt, 0.f);
        updateRunBest(play, play->getCurrentPercent());
    }

    auto& stats = s_attemptStats;
    if (!stats.best || stats.bestStart != play->m_startPosObject) updateRunBest(play, 0.f);
    const auto cutoff = StatsClock::now() - std::chrono::seconds(1);
    while (!stats.clicks.empty() && stats.clicks.front() <= cutoff) stats.clicks.pop_front();

    if (!F.showBestRun && !F.showClicks && !F.showSessionTime) {
        if (stats.label) stats.label->setVisible(false);
        return;
    }
    if (!stats.label) {
        if (!play->m_uiLayer) return;
        auto* label = CCLabelBMFont::create("", "bigFont.fnt");
        if (!label) return;
        label->setScale(0.36f);
        label->setAnchorPoint({0.f, 1.f});
        label->setAlignment(kCCTextAlignmentLeft);
        play->m_uiLayer->addChild(label, 1000);
        stats.label = label;
    }
    std::string value;
    if (F.showBestRun) {
        value += fmt::format("Best Run{}: {:.2f}%", stats.bestStart ? " (StartPos)" : "",
            stats.best ? *stats.best : 0.f);
    }
    if (F.showClicks) {
        if (!value.empty()) value += '\n';
        value += fmt::format("CPS: {}   Clicks: {}", stats.clicks.size(), stats.totalClicks);
    }
    if (F.showSessionTime) {
        if (!value.empty()) value += '\n';
        const auto seconds = static_cast<unsigned long long>(s_sessionStats.seconds);
        value += fmt::format("Session: {:02}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60);
    }
    if (value != stats.label->getString()) {
        stats.label->setString(value.c_str());
        stats.label->setOpacity(metricOpacity);
    }
    const auto size = CCDirector::sharedDirector()->getWinSize();
    stats.label->setPosition({6.f, size.height - (showNoclipStats() ? 21.f : 6.f)});
    stats.label->setVisible(true);
}

struct PlayerTrajectory {
    Ref<CCDrawNode> node;
    CCPoint lastPosition{};
    double lastLevelTime = -1.;
    float dxPerTick = 0.f;
    std::optional<float> groundY;

    void update(PlayLayer* play, PlayerObject* player) {
        if (node) node->clear();
        if (!F.trajectory || !player || !play->m_objectLayer || play->m_isPaused
            || play->m_hasCompletedLevel || player->m_isDead || play->m_isPlatformer
            || player->m_isSideways || player->m_isDashing || player->m_isOnSlope) {
            lastLevelTime = -1.;
            groundY.reset();
            return;
        }

        const auto start = player->getPosition();
        const double levelTime = play->m_gameState.m_levelTime;
        const double elapsedTicks = (levelTime - lastLevelTime) * 240.;
        if (player->m_isOnGround) groundY = start.y;
        if (lastLevelTime >= 0. && elapsedTicks > 0.0) {
            dxPerTick = static_cast<float>((start.x - lastPosition.x) / elapsedTicks);
        }
        if (lastLevelTime < 0. || elapsedTicks < 0.) dxPerTick = 0.f;
        lastPosition = start;
        lastLevelTime = levelTime;
        if (!(std::abs(dxPerTick) > 0.05f && std::abs(dxPerTick) < 5.f)) return;

        if (!node) {
            auto* draw = CCDrawNode::create();
            if (!draw) return;
            play->m_objectLayer->addChild(draw, 1000);
            node = draw;
        }

        const auto shape = player->getObjectRect();
        const auto offset = shape.origin - start;
        struct Obstacle {
            CCRect rect;
            bool hazard;
            bool solid;
        };
        std::vector<Obstacle> obstacles;
        const auto& objects = play->m_activeObjects;
        const auto count = std::min(objects.size(), static_cast<size_t>(std::max(play->m_activeObjectsCount, 0)));
        const float zoom = std::max(play->m_gameState.m_cameraZoom, 0.1f);
        const float screenWidth = CCDirector::sharedDirector()->getWinSize().width / zoom;
        const int ticks = std::clamp(static_cast<int>(std::ceil(screenWidth / std::abs(dxPerTick))), 220, 720);
        const float endX = start.x + dxPerTick * ticks;
        for (size_t i = 0; i < count; ++i) {
            auto* object = objects[i];
            if (!object || object == play->m_anticheatSpike || object->m_isDisabled || object->m_isDisabled2
                || object->m_objectType == GameObjectType::Decoration
                || object->m_objectType == GameObjectType::Collectible
                || object->m_objectType == GameObjectType::UserCoin
                || object->m_objectType == GameObjectType::SecretCoin) continue;
            const auto center = object->getPosition();
            if (center.x < std::min(start.x, endX) - 100.f || center.x > std::max(start.x, endX) + 100.f) continue;
            const auto rect = object->getObjectRect();
            if (rect.size.width > 0.f && rect.size.height > 0.f) {
                const bool hazard = object->m_objectType == GameObjectType::Hazard
                    || object->m_objectType == GameObjectType::AnimatedHazard;
                const bool solid = object->m_objectType == GameObjectType::Solid
                    || object->m_objectType == GameObjectType::Breakable;
                if (solid || !shape.intersectsRect(rect)) obstacles.push_back({rect, hazard, solid});
            }
        }

        const auto drawBox = [&](CCRect const& rect, float width, ccColor4F color) {
            const auto lower = rect.origin;
            const CCPoint right{lower.x + rect.size.width, lower.y};
            const CCPoint top{right.x, lower.y + rect.size.height};
            const CCPoint left{lower.x, top.y};
            node->drawSegment(lower, right, width, color);
            node->drawSegment(right, top, width, color);
            node->drawSegment(top, left, width, color);
            node->drawSegment(left, lower, width, color);
        };

        const bool mini = player->m_vehicleSize < 0.9f;
        const float jumpVelocity = mini ? 8.9f : 11.18f;
        const float gravityMultiplier = std::clamp(std::abs(player->m_gravityMod), 0.05f, 8.f);
        const float boxSpacing = std::max(5.f, shape.size.width * 0.6f);
        const auto playerRect = [&](CCPoint position) {
            return CCRect(position.x + offset.x, position.y + offset.y,
                shape.size.width, shape.size.height);
        };
        const auto heldJump = player->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
        const bool alreadyHeld = heldJump != player->m_holdingButtons.end() && heldJump->second;

        const auto drawBranch = [&](bool held, ccColor4F color, float lineWidth) {
            auto position = start;
            auto velocity = static_cast<float>(player->m_yVelocity);
            float gravityDirection = player->m_isUpsideDown ? 1.f : -1.f;
            bool grounded = player->m_isOnGround;
            bool pendingPress = held && !alreadyHeld;
            int robotBoost = 0;
            float distanceSinceBox = boxSpacing;
            auto outline = color;
            outline.a *= 0.47f;

            for (int tick = 0; tick < ticks; ++tick) {
                const auto old = position;
                position.x += dxPerTick;
                bool teleported = false;
                if (player->m_isDart) {
                    position.y += std::abs(dxPerTick) * gravityDirection * (held ? -1.f : 1.f)
                        * (mini ? 2.f : 1.f);
                } else {
                    if ((player->m_isBall || player->m_isSpider) && pendingPress && grounded) {
                        pendingPress = false;
                        gravityDirection *= -1.f;
                        grounded = false;
                        velocity = 0.f;
                        if (player->m_isSpider) {
                            auto rect = playerRect(position);
                            std::optional<float> nearest;
                            for (auto const& object : obstacles) {
                                if (!object.solid || rect.getMaxX() <= object.rect.getMinX()
                                    || rect.getMinX() >= object.rect.getMaxX()) continue;
                                const float target = gravityDirection > 0.f
                                    ? object.rect.getMinY() - offset.y - shape.size.height
                                    : object.rect.getMaxY() - offset.y;
                                const float distance = (target - position.y) * gravityDirection;
                                if (distance > 0.f && (!nearest || distance < *nearest)) nearest = distance;
                            }
                            if (!nearest) break;
                            position.y += *nearest * gravityDirection;
                            grounded = true;
                            teleported = true;
                        }
                    } else if (player->m_isSwing && pendingPress) {
                        pendingPress = false;
                        gravityDirection *= -1.f;
                        grounded = false;
                    } else if ((player->m_isBird && pendingPress)
                        || (held && grounded && !player->m_isShip && !player->m_isSwing
                            && !player->m_isBall && !player->m_isSpider && !player->m_isBird)) {
                        pendingPress = false;
                        velocity = -gravityDirection * (player->m_isBird ? 8.5f : jumpVelocity);
                        grounded = false;
                        if (player->m_isRobot) robotBoost = 30;
                    }

                    if (player->m_isShip && held) grounded = false;
                    if (!grounded) {
                        position.y += velocity * 0.225f;
                        float acceleration = 0.212f * gravityDirection;
                        if (player->m_isShip) acceleration = (held ? -0.33f : 0.25f) * gravityDirection;
                        if (player->m_isSwing) acceleration = 0.30f * gravityDirection;
                        if (held && robotBoost > 0) {
                            acceleration *= 0.32f;
                            --robotBoost;
                        }
                        velocity += acceleration * gravityMultiplier;
                        if (player->m_isShip || player->m_isSwing) velocity = std::clamp(velocity, -10.f, 10.f);
                        if (groundY && gravityDirection == (player->m_isUpsideDown ? 1.f : -1.f)
                            && (position.y - *groundY) * gravityDirection >= 0.f
                            && velocity * gravityDirection >= 0.f) {
                            position.y = *groundY;
                            velocity = 0.f;
                            grounded = true;
                        }
                    }
                }

                // Continue from a solid surface when the projected box crosses its top
                // (or underside under inverted gravity). Side contacts still end the branch.
                if (!player->m_isDart && !teleported && velocity * gravityDirection >= 0.f) {
                    const auto before = playerRect(old);
                    const auto after = playerRect(position);
                    std::optional<float> landing;
                    for (auto const& object : obstacles) {
                        if (!object.solid || after.getMaxX() <= object.rect.getMinX()
                            || after.getMinX() >= object.rect.getMaxX()) continue;
                        const float surface = gravityDirection < 0.f ? object.rect.getMaxY() : object.rect.getMinY();
                        const float previousEdge = gravityDirection < 0.f ? before.getMinY() : before.getMaxY();
                        const float nextEdge = gravityDirection < 0.f ? after.getMinY() : after.getMaxY();
                        if ((previousEdge - surface) * gravityDirection > 0.05f
                            || (nextEdge - surface) * gravityDirection < 0.f) continue;
                        const float y = surface - offset.y - (gravityDirection > 0.f ? shape.size.height : 0.f);
                        if (!landing || (y - *landing) * gravityDirection < 0.f) landing = y;
                    }
                    if (landing) {
                        position.y = *landing;
                        velocity = 0.f;
                        grounded = true;
                    } else if (grounded && (!groundY || std::abs(position.y - *groundY) > 0.05f)) {
                        grounded = false;
                    }
                }
                if (!teleported) node->drawSegment(old, position, lineWidth, color);
                const auto rect = playerRect(position);
                const auto contact = std::find_if(obstacles.begin(), obstacles.end(),
                    [&](Obstacle const& object) {
                        // A shared floor edge is not an overlap.
                        return rect.getMaxX() > object.rect.getMinX() + 0.05f
                            && rect.getMinX() < object.rect.getMaxX() - 0.05f
                            && rect.getMaxY() > object.rect.getMinY() + 0.05f
                            && rect.getMinY() < object.rect.getMaxY() - 0.05f;
                    });
                if (contact != obstacles.end()) {
                    const ccColor4F contactColor = contact->hazard
                        ? ccColor4F{1.f, 0.25f, 0.18f, 1.f}
                        : ccColor4F{1.f, 0.78f, 0.25f, 0.95f};
                    drawBox(rect, 0.60f / zoom, contactColor);
                    if (contact->hazard) drawBox(contact->rect, 0.50f / zoom, contactColor);
                    break;
                }

                distanceSinceBox += std::hypot(position.x - old.x, position.y - old.y);
                if (distanceSinceBox >= boxSpacing || tick + 1 == ticks) {
                    drawBox(rect, 0.25f / zoom, outline);
                    distanceSinceBox = 0.f;
                }
            }
        };

        drawBranch(false, {0.15f, 0.9f, 1.f, 0.85f}, 0.36f);
        drawBranch(true, {1.f, 0.25f, 0.65f, 0.95f}, 0.27f);
    }

    void clear() {
        if (node) node->clear();
        lastLevelTime = -1.;
        groundY.reset();
    }
};

static std::array<PlayerTrajectory, 2> s_trajectories;

class $modify(SafeModeGameLevel, GJGameLevel) {
    void savePercentage(int percent, bool practice, int clicks, int attempts, bool valid) {
        const auto* play = PlayLayer::get();
        if (s_protectedQuitLevel == this || (play && play->m_level == this && safeModeActive())) return;
        GJGameLevel::savePercentage(percent, practice, clicks, attempts, valid);
    }
};

class $modify(GdMenuPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::levelComplete", -100);
    }

    bool init(GJGameLevel* level, bool dontCreateObjects) {
        s_autoSafeAttempt = false;
        s_attemptStats = {};
        s_trajectories = {};
        return PlayLayer::init(level, dontCreateObjects);
    }

    bool shouldDrawDebugHitboxes() {
        return F.showHitboxes;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (showNoclipStats() && object && player && m_started && !m_isPaused
            && object != m_anticheatSpike) {
            if (s_accHitObjects.insert(object).second) {
                s_accBadFrame = true;
                s_accDeaths++;
                doFlash();
            }
        }
        if (F.noclip || showNoclipStats()) {
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        updateRunBest(this, 100.f);
        // The game uses its test-mode completion path for editor playtests.
        const bool wasTestMode = m_isTestMode;
        if (safeModeActive()) m_isTestMode = true;
        PlayLayer::levelComplete();
        m_isTestMode = wasTestMode;
    }

    void resetLevel() {
        s_attemptStats.resetClicks();
        for (auto& trajectory : s_trajectories) trajectory.clear();
        if (s_accOwner == this) resetAccuracy();
        PlayLayer::resetLevel();
        s_autoSafeAttempt = autoSafeRequired();
        s_attemptStats.best = nullptr;
    }

    void onQuit() {
        s_protectedQuitLevel = safeModeActive() ? m_level : nullptr;
        s_attemptStats = {};
        s_trajectories = {};
        s_inLevel = false;
        s_levelLayer = nullptr;
        s_pausedLevel = false;
        PlayLayer::onQuit();
        s_protectedQuitLevel = nullptr;
        s_autoSafeAttempt = false;
    }
};

class $modify(GJBaseGameLayer) {
    void update(float dt) {
        float speed = F.speedhack ? F.speed : 1.0f;
        applyAudioPitch(speed);

        if (!g_menuOpen && static_cast<GJBaseGameLayer*>(this) == static_cast<GJBaseGameLayer*>(PlayLayer::get())) replay::beforeUpdate();
        auto* pl = typeinfo_cast<PlayLayer*>(this);
        const bool nativeDebugDraw = m_isDebugDrawEnabled;
        const bool nativeHitboxesOnDeath = m_hitboxesOnDeath;
        const bool nativeDisablePlayerHitbox = m_disablePlayerHitbox;
        if (pl && F.showHitboxes) {
            m_isDebugDrawEnabled = true;
            m_hitboxesOnDeath = false;
            m_disablePlayerHitbox = false;
            s_attemptStats.forcedHitboxes = true;
        } else if (pl && s_attemptStats.forcedHitboxes) {
            if (m_debugDrawNode) m_debugDrawNode->clear();
            if (m_debugDrawNode2) m_debugDrawNode2->clear();
            s_attemptStats.forcedHitboxes = false;
        }

        if (pl && autoSafeRequired()) s_autoSafeAttempt = true;
        GJBaseGameLayer::update(dt * speed);
        if (pl && F.showHitboxes) {
            m_isDebugDrawEnabled = nativeDebugDraw;
            m_hitboxesOnDeath = nativeHitboxesOnDeath;
            m_disablePlayerHitbox = nativeDisablePlayerHitbox;
        }
        if (pl) {
            s_inLevel = true;
            s_levelLayer = pl;
            this->updateAccuracy(pl);
            updateSessionStats(pl, dt);
            s_trajectories[0].update(pl, pl->m_player1);
            s_trajectories[1].update(pl, pl->m_gameState.m_isDualMode ? pl->m_player2 : nullptr);
        }

        if (!F.autoclicker || replay::active()) {
            return;
        }
        if (!pl || !pl->m_player1 || !pl->m_player1->m_isOnGround) {
            return;
        }

        static int frame = 0;
        if ((frame++ & 1) == 0) {
            pl->m_player1->pushButton(PlayerButton::Jump);
        } else {
            pl->m_player1->releaseButton(PlayerButton::Jump);
        }
    }

    void handleButton(bool down, int button, bool player1) {
        auto* play = PlayLayer::get();
        if (play && static_cast<GJBaseGameLayer*>(play) == static_cast<GJBaseGameLayer*>(this)
            && button == static_cast<int>(PlayerButton::Jump) && !replay::playing()) {
            bool& held = s_attemptStats.held[player1 ? 0 : 1];
            if (down && !held && !g_menuOpen && !play->m_isPaused && !play->m_hasCompletedLevel
                && play->m_player1 && !play->m_player1->m_isDead) {
                ++s_attemptStats.totalClicks;
                s_attemptStats.clicks.push_back(StatsClock::now());
            }
            held = down;
        }
        GJBaseGameLayer::handleButton(down, button, player1);
    }

    void updateAccuracy(PlayLayer* pl) {
        if (s_accOwner != pl) {
            s_accOwner = pl;
            s_lastAttempts = pl->m_attempts;
            resetAccuracy();
        } else if (pl->m_attempts < s_lastAttempts) {
            s_lastAttempts = pl->m_attempts;
            resetAccuracy();
        } else {
            s_lastAttempts = pl->m_attempts;
        }

        auto* scene = CCDirector::sharedDirector()->getRunningScene();
        if (s_accScene != scene) {
            s_accScene = scene;
            s_accLabel = nullptr;
            s_deathsLabel = nullptr;
        }

        if (!showNoclipStats() || !scene) {
            if (s_accLabel) s_accLabel->setVisible(false);
            if (s_deathsLabel) s_deathsLabel->setVisible(false);
            return;
        }

        ++s_accFrames;
        if (s_accBadFrame) {
            ++s_accBad;
            s_accBadFrame = false;
        }

        const auto updateLabel = [&](CCLabelBMFont*& label, bool enabled, float x, std::string const& value) {
            if (!enabled) {
                if (label) label->setVisible(false);
                return;
            }
            if (!label) {
                label = CCLabelBMFont::create("", "bigFont.fnt");
                if (!label) return;
                label->setScale(0.45f);
                label->setAnchorPoint({0.f, 1.f});
                scene->addChild(label, 1000);
            }
            const auto size = CCDirector::sharedDirector()->getWinSize();
            label->setPosition({x, size.height - 6.f});
            if (value != label->getString()) {
                label->setString(value.c_str());
                label->setOpacity(metricOpacity);
            }
            label->setVisible(true);
        };
        const float accuracy = 100.f * (1.f - static_cast<float>(s_accBad) / s_accFrames);
        updateLabel(s_accLabel, F.accuracy, 6.f, fmt::format("Acc {:.1f}%", accuracy));
        updateLabel(s_deathsLabel, F.noclipDeaths, 113.f, fmt::format("Deaths {}", s_accDeaths));
    }
};
