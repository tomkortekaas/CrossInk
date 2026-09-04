#pragma once

#include <cstdint>

namespace navigator {

enum class NavLaunchDecision : uint8_t { RunNavigator, ReturnToReaderDashboard };
enum class NavInputAction : uint8_t { StayInNavigator, ReturnToDashboard };

struct NavMarker {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t flags = 0;
  uint32_t requestId = 0;
  uint32_t crc = 0;
};
static_assert(sizeof(NavMarker) == 16);

NavLaunchDecision chooseNavBootRoute(bool markerPresent, bool devAutostartOverride);
NavInputAction chooseNavInputAction(bool backReleased);
bool isValidNavMarker(const NavMarker& marker);

bool writeNavMarker(uint32_t requestId);
bool readNavMarker(NavMarker& marker);
bool clearNavMarker();

}  // namespace navigator
