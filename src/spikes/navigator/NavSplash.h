#pragma once

#include <cstdint>

// Phase-0 navigator boot splash compatibility wrapper.
//
// Kept as a thin default-state wrapper over NavScreenRenderer so the
// already-verified NavSplash draw() signature, portrait transform, and layout
// contract keep working unchanged: draw() renders the default NavState (Left,
// "180 M" on "DUINWEG", "4,2 KM  52 MIN", navigating), which is byte-identical
// to the old static splash. Dynamic screens go through NavScreenRenderer.
class NavSplash {
 public:
  static void draw(uint8_t* frameBuffer, uint16_t widthPx, uint16_t heightPx);
};
