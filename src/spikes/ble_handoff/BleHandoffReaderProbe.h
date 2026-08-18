#pragma once

#include <cstdint>

#ifdef CROSSINK_BLE_HANDOFF_READER
class GfxRenderer;
namespace BleHandoffReaderProbe {
void logPersistedPayload();
// Waarom renderDashboardCard() niets kon tekenen. Meegegeven aan de
// terugvalweergave zodat een afkeuring op het paneel te lezen is in plaats van
// stil te blijven: een leeg agendascherm is niet te onderscheiden van een
// toestel dat niet wakker werd.
enum class DashboardSkipReason : uint8_t {
  None = 0,          // er is getekend
  NoPackage,         // niets in NVS
  UnknownTemplate,   // byte 5 kent deze build niet
  Undecodable,       // template bekend, inhoud niet
};

const char* dashboardSkipReasonText(DashboardSkipReason reason);
bool renderDashboardCard(GfxRenderer& renderer, DashboardSkipReason* reasonOut = nullptr);
}  // namespace BleHandoffReaderProbe
#endif
