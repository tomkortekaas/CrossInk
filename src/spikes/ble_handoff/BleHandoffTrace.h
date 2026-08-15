#pragma once

#ifdef CROSSINK_BLE_HANDOFF_READER

#include <stdint.h>

#include "AgendaWakePolicy.h"

namespace dashboard {

// Appends one line per reader boot to /crossink-ble-trace.txt on the SD card.
//
// Why this exists: the only diagnostic channel for the agenda wake cycle is a
// USB serial capture, and USB is not a neutral observer here - a device on USB
// power takes the AfterUSBPower boot route and stays awake instead of sleeping,
// which is the very behaviour under investigation. Every wake observed over
// serial is therefore a wake that happened under different conditions than the
// ones that matter. This writes to storage the device carries, so a night on
// battery can be read back afterwards.
//
// One line per wake cycle: a timer wake hands off to the receiver partition
// (which has no SD access) and resets back into the reader, and it is that
// return boot which lands here, carrying the receiver's verdict. An empty or
// unchanged file after a spell on battery is itself the finding: the device
// never woke.
//
// Call after Storage.begin(); the wake fields are sampled long before the card
// is mounted, so they are passed in rather than read here.
void appendBootTrace(uint8_t wakeupReason, ReceiverResult retainedResult, bool storageReady);

}  // namespace dashboard

#endif
