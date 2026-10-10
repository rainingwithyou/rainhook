#pragma once
// Maphack: forces client-side visibility flags on enemy units.
//
// ShowEntity carries presentation flags that the client sets from its own
// visibility logic (grass occlusion, small-map filtering, renderer culling).
// Flipping them client-side reveals enemies on the minimap and through bushes
// without touching movement, damage or any server-authoritative state.

namespace maphack {
void Tick();
}
