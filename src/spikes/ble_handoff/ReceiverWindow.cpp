#include "ReceiverWindow.h"

namespace dashboard {

ReceiverWindowAction ReceiverWindow::actionAt(const uint32_t nowMs, const bool accepted) const {
  if (accepted) return ReceiverWindowAction::ReturnAccepted;
  return nowMs - startedAtMs >= windowMs ? ReceiverWindowAction::ReturnTimedOut : ReceiverWindowAction::Listen;
}

}  // namespace dashboard
