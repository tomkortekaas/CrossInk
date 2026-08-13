#pragma once

#include <cstdint>

namespace dashboard {

enum class ReceiverWindowAction : uint8_t { Listen, ReturnAccepted, ReturnTimedOut };

class ReceiverWindow {
 public:
  explicit ReceiverWindow(uint32_t startedAtMs) : startedAtMs(startedAtMs) {}

  ReceiverWindowAction actionAt(uint32_t nowMs, bool accepted) const;

 private:
  uint32_t startedAtMs;
};

}  // namespace dashboard
