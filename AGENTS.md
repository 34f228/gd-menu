# GD Menu — Geode mod for Geometry Dash

In-game cheat menu for Geometry Dash 2.2091 on Windows, built with Geode SDK
`6.0.0-alpha.1`. UI is Dear ImGui (via `imgui-cocos`).

## Layout

| Path | What |
| --- | --- |
| `src/main.cpp` | feature flags, ImGui menu, and gameplay hooks |
| `mod.json` | Geode metadata (`id`, `gd`, `geode` version constraint) |
| `CMakeLists.txt` | Geode mod + CPM dependency on `imgui-cocos` |
| `src/replay.cpp` | xdBot replay port and panel |

Outside the repo:

| Path | What |
| --- | --- |
| `%USERPROFILE%\gd-menu\sdk` | Geode SDK checkout (`GEODE_SDK`) |
| `%USERPROFILE%\gd-menu\tools\cli\geode.exe` | Geode CLI 4.0.0 |
| `<Steam library>\steamapps\common\Geometry Dash` | game install |

## Build

Requires `GEODE_SDK` and a `PATH` with the Geode CLI, CMake and Ninja (all ship
with VS 2022 except the CLI):

Run from the repository root in a PowerShell session with CMake and the Geode
CLI on `PATH`. Adjust `GEODE_SDK` to your SDK checkout.

```powershell
$env:GEODE_SDK = Join-Path $env:USERPROFILE 'gd-menu/sdk'
geode build
```

`geode build` copies the packaged `34f.gdmenu.geode` into
`<game>\geode\mods`, so no manual install step.

First configure is slow: it clones the bindings repo and, through CPM, imgui and
`imgui-cocos`.

## Gotchas learned the hard way

- `mod.json` → `"geode": "6.0.0-alpha.1"` — **no leading `v`**. With `v` the
  build fails: "Mod 34f.gdmenu is made for Geode version ... but you have
  6.0.0-alpha.1 SDK installed".
- Do **not** call `include(CPM)` in `CMakeLists.txt`. Geode brings CPM in via
  `add_subdirectory($ENV{GEODE_SDK})`; just call `CPMAddPackage` afterwards.
- `imgui-cocos` is pulled as
  `CPMAddPackage("gh:matcool/gd-imgui-cocos#geode")` and linked with
  `target_link_libraries(${PROJECT_NAME} imgui-cocos)`. It is not a `mod.json`
  dependency.
- ImGui coordinates are real framebuffer pixels (1920x1200), while the game
  design size is 512x320. The UI needs `style.ScaleAllSizes(2.4f)` and
  `io.FontGlobalScale = 2.4f` in the `setup` callback, otherwise everything
  renders tiny.
- `ImGuiCond_FirstUseEver` for window size is a trap: the size is persisted in
  `imgui.ini` and later edits do nothing. Use `ImGuiCond_Always` or
  `ImGuiWindowFlags_AlwaysAutoResize`.
- The Geode bitmap fonts (`bigFont.fnt`, `goldFont.fnt`, `chatFont.fnt`) are
  Latin-only. Cyrillic text renders as nothing.

## Feature notes

- `NoClip` / `NoClip Accuracy` both suppress death by skipping
  `PlayLayer::destroyPlayer`. Collision code is never touched.
- The phantom death at level start was `GJBaseGameLayer::m_anticheatSpike`: the
  game moves an anti-cheat spike under the icon at start/respawn. Filter it by
  pointer.
- Accuracy counts only when `m_started && !m_isPaused`, deduplicated per
  `GameObject*` per attempt, and only for real hazard collisions.
- Speedhack scales `dt` in `GJBaseGameLayer::update`. Doing it via
  `applyTimeWarp` broke the feature and was reverted.
- Opening the menu in a level pauses it with
  `CCScheduler::pauseTarget(PlayLayer)` plus `pauseAllMusic(true)`, so the game
  keeps rendering (ImGui needs that frame loop) while gameplay is frozen.
  Closing calls `resumeTarget` and `resumeAllMusic()`. If the game was already
  paused (`m_isPaused`), the menu does not touch the pause state.
- The menu hotkey is polled with `GetAsyncKeyState` inside the ImGui frame, not
  hooked on `CCKeyboardDispatcher`: `imgui-cocos` hooks that dispatcher too and
  swallows the key while ImGui has focus.
- Replay uses the xdBot port in src/replay.cpp. It records inputs and frame fixes
  at the game's 240 TPS input queue and rolls back the macro on checkpoint load.
  See docs/replay.md for controls, provenance, and the regression scenario.
- Settings live in Geode saved values (`Mod::get()->setSavedValue`), keyed
  `noclip`, `noclip_accuracy`, `speedhack`, `autoclicker`, `speed_mult`,
  `show_fps`, `hide_player`, `practice_music`, `no_particles`, `flash_color`,
  `menu_key`, `bot_record`, `bot_play`.

## Known issue

With noclip on, ships (and probably ball / UFO / spider) do not ride slopes:
skipping the death also skips the collision response that launches the player
off the slope. Not fixed.

## Verifying a change

1. `geode build` — must print `Successfully packaged` and `Installed`.
2. Launch the game, open the menu with the hotkey (default `Tab`, stored in
   `menu_key`).
3. Logs: `<game>\geode\logs\Geode <date>.log`; crashes:
   `<game>\geode\crashlogs`.
