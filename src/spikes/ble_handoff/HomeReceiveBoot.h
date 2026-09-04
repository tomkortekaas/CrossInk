#pragma once
#include <cstdint>

namespace dashboard {
constexpr uint32_t HOME_RECEIVE_WINDOW_MS = 60000U;
constexpr uint32_t HOME_RECEIVE_MAGIC = 0x48525800U;
// One RTC word: a tagged request plus the user's Back-button mapping. No NVS writes.
constexpr uint32_t homeReceiveToken(uint8_t backButton) {
  return backButton < 4 ? HOME_RECEIVE_MAGIC | backButton : 0;
}
inline int consumeHomeReceiveToken(uint32_t& token, bool softwareReset) {
  const uint32_t value = token;
  token = 0;  // Consume before any hardware work, including on panic/cold boot.
  return softwareReset && (value & 0xfffffffcU) == HOME_RECEIVE_MAGIC ? static_cast<int>(value & 3U) : -1;
}
// Called only from the home action on builds with the in-process receiver.
void requestHomeReceive();
}  // namespace dashboard
