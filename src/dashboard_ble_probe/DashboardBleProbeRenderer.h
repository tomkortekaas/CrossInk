#pragma once

#include <cstdint>

#include "DashboardBleProbe.h"
#include "activities/boot_sleep/DashboardSleepScreen.h"

class GfxRenderer;

namespace probe {

class DashboardBleProbeRenderer final : public ProbeScreen, public DashboardSleepScreen {
 public:
  explicit DashboardBleProbeRenderer(GfxRenderer& renderer);
  bool renderAccepted(uint32_t messageId, uint8_t payloadLength) override;
  bool renderForSleep() override;

 private:
  bool drawAccepted(uint32_t messageId, uint8_t payloadLength);
  GfxRenderer& renderer_;
  bool hasAcceptedFrame_ = false;
  uint32_t lastMessageId_ = 0;
  uint8_t lastPayloadLength_ = 0;
};

}  // namespace probe
