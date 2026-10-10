#include "maphack.h"

#include "config.h"
#include "game.h"

namespace maphack {

void Tick() {
    if (!g_cfg.map_enabled) return;

    // Every frame: the game rewrites these flags from its own vision logic each
    // frame, so a periodic re-apply just leaves gaps where the reveal is off.
    game::ApplyMaphack(true);
}

}  // namespace maphack
