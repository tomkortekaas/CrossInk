#pragma once

namespace probe {

struct DashboardBleSecurityPolicy {
  bool bonding;
  bool mitm;
  bool secureConnections;
  bool authenticatedCharacteristics;
};

// The X3 only needs an encrypted, bonded local link for the dashboard probe.
// Avoid passkey/Secure Connections work on the memory-constrained ESP32-C3.
inline constexpr DashboardBleSecurityPolicy kDashboardBleSecurity{
    .bonding = true,
    .mitm = false,
    .secureConnections = false,
    .authenticatedCharacteristics = false,
};

}  // namespace probe
