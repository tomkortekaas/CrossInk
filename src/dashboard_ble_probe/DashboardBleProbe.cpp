#include "DashboardBleProbe.h"

#include <algorithm>

namespace probe {

DashboardBleProbe::DashboardBleProbe(ProbeScreen& screen, ProbeStatusSink& status) : screen_(screen), status_(status) {}

void DashboardBleProbe::begin() { status_.publish(Status::Ready, false, 0); }

bool DashboardBleProbe::enqueue(const uint8_t* bytes, const size_t length) {
  if (bytes == nullptr || length == 0 || length > kMaxFrameBytes) return false;

#ifdef ARDUINO
  portENTER_CRITICAL(&mux_);
#endif
  const bool accepted = !pendingReady_;
  if (accepted) {
    std::copy_n(bytes, length, pending_.begin());
    pendingLength_ = length;
    pendingReady_ = true;
  }
#ifdef ARDUINO
  portEXIT_CRITICAL(&mux_);
#endif
  return accepted;
}

void DashboardBleProbe::loop() {
  std::array<uint8_t, kMaxFrameBytes> frame{};
  size_t length = 0;

#ifdef ARDUINO
  portENTER_CRITICAL(&mux_);
#endif
  if (pendingReady_) {
    length = pendingLength_;
    std::copy_n(pending_.begin(), length, frame.begin());
    pendingReady_ = false;
    pendingLength_ = 0;
  }
#ifdef ARDUINO
  portEXIT_CRITICAL(&mux_);
#endif

  if (length == 0) return;
  const DecodeResult result = decodeFrame(frame.data(), length);
  if (result.status != Status::Accepted) {
    status_.publish(result.status, result.hasMessageId, result.messageId);
    return;
  }

  const bool rendered = screen_.renderAccepted(result.messageId, result.payloadLength);
  status_.publish(rendered ? Status::Accepted : Status::RenderFailed, true, result.messageId);
}

}  // namespace probe
