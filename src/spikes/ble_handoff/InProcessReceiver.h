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

}  // namespace dashboard
