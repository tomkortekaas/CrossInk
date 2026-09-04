#pragma once

#include <cstdint>

namespace dashboard {

enum class ReceiverWindowAction : uint8_t { Listen, ReturnAccepted, ReturnTimedOut, ReturnCancelled };

class ReceiverWindow {
 public:
  // How long the receiver listens before giving up, when no other length is
  // given. Long enough for a phone that is present to connect and transfer,
  // short enough that a wake with nobody there is not paid for twice.
  static constexpr uint32_t DEFAULT_WINDOW_MS = 20'000U;

  explicit ReceiverWindow(uint32_t startedAtMs, uint32_t windowMs = DEFAULT_WINDOW_MS)
      : startedAtMs(startedAtMs), windowMs(windowMs) {}

  ReceiverWindowAction actionAt(uint32_t nowMs, bool accepted, bool cancelled = false) const;

 private:
  uint32_t startedAtMs;
  uint32_t windowMs;
};

}  // namespace dashboard
