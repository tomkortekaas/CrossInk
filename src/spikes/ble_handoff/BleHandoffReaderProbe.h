#pragma once

#ifdef CROSSINK_BLE_HANDOFF_READER
class GfxRenderer;
namespace BleHandoffReaderProbe {
void logPersistedPayload();
// Renders the persisted last-known-good dashboard package regardless of
// which template produced it (TEMPLATE_AGENDA or TEMPLATE_WIDGET_GRID).
// Returns false - leaving the screen untouched - when nothing is persisted
// or the persisted template is not one this build knows how to render, so
// the caller can fall back to the existing dashboard sleep screen.
bool renderDashboardCard(GfxRenderer& renderer);
}  // namespace BleHandoffReaderProbe
#endif
