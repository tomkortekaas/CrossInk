#include "ReceiverWindow.h"

namespace dashboard {

ReceiverWindowAction ReceiverWindow::actionAt(const uint32_t nowMs, const bool accepted, const bool cancelled) const {
  if (accepted) return ReceiverWindowAction::ReturnAccepted;
  if (cancelled) return ReceiverWindowAction::ReturnCancelled;
  return nowMs - startedAtMs >= windowMs ? ReceiverWindowAction::ReturnTimedOut : ReceiverWindowAction::Listen;
}

}  // namespace dashboard
