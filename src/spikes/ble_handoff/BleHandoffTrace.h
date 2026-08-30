#pragma once

#ifdef CROSSINK_BLE_HANDOFF_READER

#include <stdint.h>

#include "AgendaWakePolicy.h"

namespace dashboard {

// How far the boot got. Without this every line reads the same, and a wake that
// was rejected before it did anything is indistinguishable from one that ran the
// whole cycle - which is exactly the distinction the battery question turns on.
enum class BootTraceStage : uint8_t {
  Full,                     // reached the normal SD mount and carried on booting
  ReceiverHandoff,          // timer wake, about to restart into the receiver partition
  ReceiverTimedOut,         // came back from the receiver empty-handed
  PowerButtonRejected,      // wake failed its hold check; sleeps again WITHOUT a timer
  StandbyRefreshRequested,  // going to sleep on a stale card; asking for one window first
};

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
// `resetName` is the boot's reset reason, already named by the caller (main.cpp
// owns that switch). A BROWNOUT here on battery and SW on USB would say the
// device does wake and then collapses under the radio's current draw, rather
// than never waking at all.
void appendBootTrace(uint8_t wakeupReason, ReceiverResult retainedResult, BootTraceStage stage,
                     const char* resetName, bool storageReady);

// The same record for the boots that never reach the mount above.
//
// Three of them end tens of lines earlier: a timer wake handing off to the
// receiver restarts into the other partition, a receiver window that expired
// goes straight back to sleep, and a power-button wake that fails its hold
// check does the same. Those are exactly the cycles worth seeing, and none of
// them left any record at all - which made an empty trace file after a night
// on battery ambiguous between "never woke" and "woke every quarter of an hour
// and found nobody there".
//
// Mounts the card itself. That costs a few hundred milliseconds on a wake whose
// radio window is twenty seconds, so it does not meaningfully change the
// battery behaviour being measured.
void appendEarlyBootTrace(uint8_t wakeupReason, ReceiverResult retainedResult, BootTraceStage stage,
                          const char* resetName);

}  // namespace dashboard

#endif
