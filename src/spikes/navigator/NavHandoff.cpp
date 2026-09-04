#include "NavHandoff.h"

#include <cstddef>

#if defined(ARDUINO)
#include <nvs.h>
#endif

namespace navigator {
namespace {

constexpr uint32_t kMarkerMagic = 0x56414E58U;  // "XNAV" in little endian.
constexpr uint16_t kMarkerVersion = 1;

uint32_t crc32(const uint8_t* data, const size_t length) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

uint32_t markerCrc(const NavMarker& marker) {
  return crc32(reinterpret_cast<const uint8_t*>(&marker), offsetof(NavMarker, crc));
}

#if defined(ARDUINO)
constexpr char kNvsNamespace[] = "x3navigator";
constexpr char kMarkerKey[] = "launch";
#endif

}  // namespace

NavLaunchDecision chooseNavBootRoute(const bool markerPresent, const bool devAutostartOverride) {
  return markerPresent || devAutostartOverride ? NavLaunchDecision::RunNavigator
                                               : NavLaunchDecision::ReturnToReaderDashboard;
}

NavInputAction chooseNavInputAction(const bool backReleased) {
  return backReleased ? NavInputAction::ReturnToDashboard : NavInputAction::StayInNavigator;
}

bool isValidNavMarker(const NavMarker& marker) {
  return marker.magic == kMarkerMagic && marker.version == kMarkerVersion && marker.crc == markerCrc(marker);
}

#if defined(ARDUINO)
bool writeNavMarker(const uint32_t requestId) {
  NavMarker marker{};
  marker.magic = kMarkerMagic;
  marker.version = kMarkerVersion;
  marker.requestId = requestId;
  marker.crc = markerCrc(marker);

  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  const bool ok = nvs_set_blob(handle, kMarkerKey, &marker, sizeof(marker)) == ESP_OK && nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  if (!ok) return false;

  NavMarker verified{};
  return readNavMarker(verified) && verified.requestId == requestId;
}

bool readNavMarker(NavMarker& marker) {
  marker = {};
  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
  size_t size = sizeof(marker);
  const bool ok = nvs_get_blob(handle, kMarkerKey, &marker, &size) == ESP_OK && size == sizeof(marker);
  nvs_close(handle);
  return ok && isValidNavMarker(marker);
}

bool clearNavMarker() {
  nvs_handle_t handle = 0;
  const esp_err_t opened = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return true;
  if (opened != ESP_OK) return false;
  const esp_err_t erased = nvs_erase_key(handle, kMarkerKey);
  const bool ok = (erased == ESP_OK || erased == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  return ok;
}
#else
bool writeNavMarker(uint32_t) { return false; }
bool readNavMarker(NavMarker& marker) {
  marker = {};
  return false;
}
bool clearNavMarker() { return false; }
#endif

}  // namespace navigator
