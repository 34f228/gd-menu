#pragma once

class PlayLayer;

namespace trajectory {

void update(PlayLayer* play, bool enabled);
void clear();
void reset();
bool interceptDeath(PlayLayer* play);

}
