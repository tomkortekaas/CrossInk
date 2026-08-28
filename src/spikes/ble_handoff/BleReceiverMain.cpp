#ifdef CROSSINK_BLE_HANDOFF_RECEIVER

#include <Arduino.h>

#include "AgendaWakeRetention.h"
#include "DashboardBootSwitch.h"
#include "InProcessReceiver.h"

namespace {

bool returnToReader(const dashboard::ReceiverResult result) {
  dashboard::retainReceiverResult(result);
  if (!dashboard_boot::switchToReader()) {
    dashboard::retainReceiverResult(dashboard::ReceiverResult::AwaitingWindow);
    dashboard::notifyReceiverStatus(0x14);
    dashboard::teardownReceiver();
    return false;
  }
  if (result == dashboard::ReceiverResult::Accepted) {
    dashboard::notifyReceiverStatus(0x03);
  }
  delay(150);
  ESP.restart();
  return true;
}

}  // namespace

void setup() {
  delay(250);
  Serial.begin(115200);
  if (!dashboard_boot::isRunningReceiver() ||
      dashboard::retainedReceiverResult() != dashboard::ReceiverResult::AwaitingWindow) {
    Serial.println("BLE-RX invalid launch route; returning to reader");
    if (dashboard_boot::switchToReader()) {
      dashboard::retainReceiverResult(dashboard::ReceiverResult::None);
      delay(50);
      ESP.restart();
    }
    return;
  }
  const dashboard::ReceiverResult result = dashboard::runReceiverWindow(20000);
  if (!returnToReader(result)) delay(100);
}

void loop() {}

#endif
