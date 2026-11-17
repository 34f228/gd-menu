#pragma once

#include <optional>
#include <string>

class PlayLayer;

struct GameplayCopy {
    std::string data;
    int objectCount = 0;
};

std::optional<GameplayCopy> makeGameplayCopy(const PlayLayer& play);
