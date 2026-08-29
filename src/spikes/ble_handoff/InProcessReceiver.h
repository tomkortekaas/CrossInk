#pragma once

#include <stdint.h>

#include "AgendaWakePolicy.h"

namespace dashboard {

// Opens the radio and listens until `windowMs` has elapsed or a complete
// package has arrived. Deliberately leaves the stack up: the caller must still
// be able to notify before tearing it down. Always followed by
// teardownReceiver(), even on failure.
ReceiverResult runReceiverWindow(uint32_t windowMs);

// Sends a status notification over the still-open connection, carrying the
// package id and byte count of the transfer that was just received. Only valid
// between runReceiverWindow() and teardownReceiver().
void notifyReceiverStatus(uint8_t code);

// Tears down the radio and returns the heap. Mandatory: leaving the stack up
// would leave that memory gone as soon as the user opens a book, which is
// exactly why the receiver got its own partition in the first place.
void teardownReceiver();

// Hands the BLE stack's RAM back to the heap for the rest of this boot.
//
// Tearing the stack down is not enough. Linking BLE in at all costs the reader
// 27.4 KB of heap, whether or not the radio was ever switched on: measured on
// hardware 2026-08-29, a boot with `Wake route: Other` (no BLEDevice::init at
// all) still reported a total heap of 220,448 against 247,888 for a build
// without BLE. That deficit is what made a one-page chapter fail to lay out at
// 42,032 bytes free, 3 KB under the 44 KB EPUB_TEXT_LAYOUT_MIN_FREE gate.
//
// Only for boots where the window never ran. After a window teardownReceiver()
// has already called BLEDevice::deinit(true), which releases the controller
// memory itself; calling this on top of that returned status=-1 and delta=0 on
// hardware -- harmless, but a recurring fake error in the log of every agenda
// wake. Irreversible until the next reboot: BLE cannot come back up afterwards.
void releaseBluetoothMemory(const char* reason);

}  // namespace dashboard
