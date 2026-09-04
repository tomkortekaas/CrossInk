#pragma once

namespace dashboard_boot {

bool switchToSlot0();
bool switchToSlot1();
bool isRunningInSlot0();
bool isRunningInSlot1();

// Compatibility names retained for the original reader/receiver development
// spike, where reader=slot0 and receiver=slot1.
bool switchToReader();
bool switchToReceiver();
bool isRunningReader();
bool isRunningReceiver();

}  // namespace dashboard_boot
