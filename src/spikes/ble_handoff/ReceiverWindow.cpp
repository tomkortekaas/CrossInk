#include "ReceiverWindow.h"

namespace dashboard {
namespace {

constexpr uint32_t RECEIVER_WINDOW_MS = 20'000U;

}  // namespace

ReceiverWindowAction ReceiverWindow::actionAt(const uint32_t nowMs, const bool accepted) const {
  if (accepted) return ReceiverWindowAction::ReturnAccepted;
  return nowMs - startedAtMs >= RECEIVER_WINDOW_MS ? ReceiverWindowAction::ReturnTimedOut
                                                   : ReceiverWindowAction::Listen;
}

}  // namespace dashboard
