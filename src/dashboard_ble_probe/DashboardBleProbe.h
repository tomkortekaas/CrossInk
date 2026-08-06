#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "DashboardBleProtocol.h"

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif

namespace probe {

class ProbeScreen {
 public:
  virtual ~ProbeScreen() = default;
  virtual bool renderAccepted(uint32_t messageId, uint8_t payloadLength) = 0;
};

class ProbeStatusSink {
 public:
  virtual ~ProbeStatusSink() = default;
  virtual void publish(Status status, bool hasMessageId, uint32_t messageId) = 0;
};

class DashboardBleProbe {
 public:
  DashboardBleProbe(ProbeScreen& screen, ProbeStatusSink& status);

  void begin();
  bool enqueue(const uint8_t* bytes, size_t length);
  void loop();

 private:
  ProbeScreen& screen_;
  ProbeStatusSink& status_;
  std::array<uint8_t, kMaxFrameBytes> pending_{};
  size_t pendingLength_ = 0;
  bool pendingReady_ = false;
#ifdef ARDUINO
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
#endif
};

}  // namespace probe
