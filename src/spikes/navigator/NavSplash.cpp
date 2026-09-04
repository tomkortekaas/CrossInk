#include "NavSplash.h"

#include "NavScreenRenderer.h"
#include "NavState.h"

// Default-state wrapper: the NavState defaults reproduce the legacy static
// example screen (see NavState.h), so this stays the same recognizable boot
// splash the X3 navigator has always shown, now rendered through the general
// dynamic renderer.
void NavSplash::draw(uint8_t* frameBuffer, uint16_t widthPx, uint16_t heightPx) {
  navigator::NavState state;  // Defaults = the verified example state.
  navigator::NavScreenRenderer::draw(frameBuffer, widthPx, heightPx, state);
}
