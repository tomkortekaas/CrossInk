#pragma once

#include <cstdint>

#include "DashboardBleProbe.h"

class GfxRenderer;

namespace probe {

class DashboardBleProbeRenderer final : public ProbeScreen {
 public:
  explicit DashboardBleProbeRenderer(GfxRenderer& renderer);
  bool renderAccepted(uint32_t messageId, uint8_t payloadLength) override;

 private:
  GfxRenderer& renderer_;
};

}  // namespace probe
