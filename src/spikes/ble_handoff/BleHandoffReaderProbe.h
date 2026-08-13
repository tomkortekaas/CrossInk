#pragma once

#ifdef CROSSINK_BLE_HANDOFF_READER
class GfxRenderer;
namespace BleHandoffReaderProbe {
void logPersistedPayload();
bool renderAgendaCard(GfxRenderer& renderer);
}  // namespace BleHandoffReaderProbe
#endif
