#pragma once

namespace probe {

struct DashboardBleSecurityPolicy {
  bool bonding;
  bool mitm;
  bool secureConnections;
  bool encryptedCharacteristics;
  bool authenticatedCharacteristics;
};

// Temporary development-only policy for transport/lifecycle validation. Never
// send personal dashboard data until production link security is restored.
inline constexpr DashboardBleSecurityPolicy kDashboardBleSecurity{
    .bonding = false,
    .mitm = false,
    .secureConnections = false,
    .encryptedCharacteristics = false,
    .authenticatedCharacteristics = false,
};

}  // namespace probe
