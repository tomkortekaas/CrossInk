#pragma once

#include <cstdint>

namespace dashboard {

enum class WakeSource : uint8_t { Other, PowerButton, Timer };
enum class AgendaBootRoute : uint8_t { NormalReader, Receiver, InProcessReceiver, ManualInProcessReceiver };
enum class ReceiverResult : uint8_t { None, AwaitingWindow, Accepted, TimedOut, Cancelled };
enum class InProcessWindowAction : uint8_t { ContinueBoot, SleepToNextTick };

#ifndef CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES
#define CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES 15
#endif
constexpr uint32_t AGENDA_WAKE_INTERVAL_MINUTES = CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES;
constexpr uint8_t AGENDA_WAKE_WINDOW_START_HOUR = 7;
constexpr uint8_t AGENDA_WAKE_WINDOW_END_HOUR = 22;

// Cumulative power-button hold that qualifies as a manual request for an
// on-demand receiver window during an armed Agenda sleep. Measured on the hold
// the firmware can observe: InputManager stamps the press at the first update()
// after the wake and getPowerButtonHeldTime() counts from that stamp, so the
// portion of the hold verifyPowerButtonWakeup consumed counts toward it, and
// the boundary is deliberately NOT a fresh count started after verification.
constexpr uint32_t MANUAL_RECEIVER_HOLD_MS = 1000;

// How long the manual (power-hold requested) in-process receiver window
// listens. Longer than the automatic 20 s timer window because the phone was
// not scheduled for this wake and may need time to notice and connect.
constexpr uint32_t MANUAL_RECEIVER_WINDOW_MS = 30000;

// The wake interval and window the device falls back to until it has ever
// received a package, or when a received one carries an out-of-range value.
// `clampWakeSettings` is what applies that fallback; this struct is just its
// result type.
struct WakeSettings {
  uint32_t intervalMinutes = AGENDA_WAKE_INTERVAL_MINUTES;
  uint8_t windowStartHour = AGENDA_WAKE_WINDOW_START_HOUR;
  uint8_t windowEndHour = AGENDA_WAKE_WINDOW_END_HOUR;
};

// Microseconds to sleep before the next Agenda BLE wake, given the current
// local time. Wakes land on a fixed grid of `intervalMinutes` anchored at
// midnight — :00, :15, :30, :45 at the default — between `windowStartHour` and
// `windowEndHour`, with the last tick of the day capped at the window end.
// Outside the window, sleeps in one block straight through to the next
// `windowStartHour`. The three settings default to the compiled-in fallback so
// every existing call site keeps working unchanged.
//
// The grid is anchored to the clock rather than to the moment of falling
// asleep, because a button press wakes the device mid-interval and counting
// afresh from there pushed every subsequent window later. It also makes the
// schedule predictable from the clock alone, which is what any phone-side fix
// needs: an app cannot be woken in time for a moment it cannot compute.
uint64_t sleepTimerIntervalUs(bool agendaSleep, uint8_t currentHour, uint8_t currentMinute,
                               uint32_t intervalMinutes = AGENDA_WAKE_INTERVAL_MINUTES,
                               uint8_t windowStartHour = AGENDA_WAKE_WINDOW_START_HOUR,
                               uint8_t windowEndHour = AGENDA_WAKE_WINDOW_END_HOUR);

// Clamps raw, phone-supplied settings into a safe range: 1-60 minutes for the
// interval, a same-day, in-range hour pair for the window. An invalid value
// falls back to the compiled-in default for that field (the window's two
// hours fall back together, since a start/end pair only makes sense as a
// unit) rather than rejecting anything - a bad settings byte must not cost
// the whole dashboard, the way an unknown widget type silently does today.
WakeSettings clampWakeSettings(uint8_t rawIntervalMinutes, uint8_t rawWindowStartHour, uint8_t rawWindowEndHour);

// Local minute-of-day from the RTC's UTC reading, given SETTINGS.clockUtcOffsetQ:
// quarter hours biased by 48, so 48 means UTC and 56 means +2h. The window above
// is documented as local time, and this is the only thing that makes it so.
//
// Lives here rather than in main.cpp because the caller that got this wrong runs
// before the SD card is mounted, and a pure function is the part that can be
// checked on the host. Out-of-range offsets fall back to UTC.
uint16_t localMinuteOfDay(uint8_t utcHour, uint8_t utcMinute, uint8_t offsetQ);

// The RTC's UTC reading as Unix epoch seconds, so it can be compared against a
// package's `generatedAt`. The RTC exposes no seconds field, so the result
// lands on the minute - far finer than the staleness rule needs.
//
// Returns 0 for any field out of range, and for years before 1970. A caller
// cannot tell that apart from midnight on 1970-01-01, which is deliberate:
// both mean "no usable time", and the one caller treats 0 as unknown.
uint64_t utcEpochSecondsFromCivil(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute);

// Microseconds of deep sleep before the window a standby refresh asks for. Long
// enough that the sleep card has been painted and the panel has settled, short
// enough that the fresh card lands while the user is still putting the device
// down.
constexpr uint64_t STANDBY_REFRESH_DELAY_US = 2ULL * 1000ULL * 1000ULL;

// Whether the sleep now being entered should be cut short to one immediate
// agenda window instead of running to the next tick of the grid.
//
// Every uncertainty resolves to false. A wrong `true` wakes the device every two
// seconds and empties the battery overnight; a wrong `false` leaves a card
// exactly as stale as it is today. The two mistakes are not comparable, so the
// rule declines whenever it cannot establish the age: no clock, no package, or
// a clock reading earlier than the package it holds.
//
// Declining once the package is fresh is also what ends the cycle. The boot that
// renders an accepted package sleeps through this same rule (main.cpp calls
// enterDeepSleepInternal after rendering), and by then the package is seconds
// old - so one put-down buys exactly one window, with no flag to keep in step.
bool shouldRefreshAtStandby(bool agendaSleep, bool clockAvailable, uint64_t nowEpochSeconds,
                            uint64_t packageGeneratedAt, uint32_t intervalMinutes);

// What the reader does once an in-process agenda window has closed.
//
// Only an accepted package earns the rest of the boot, because only then is
// there a new card to render. Every other outcome means nothing arrived, and
// booting on would land the reader's home screen on a panel whose whole purpose
// is to show the dashboard - and hold the device awake until the inactivity
// timeout. The partition route has always done this (main.cpp, the
// returnedFromReceiver branch); the in-process route did not.
InProcessWindowAction actionAfterInProcessWindow(ReceiverResult result);

// Pure decision boundary for the manual receiver hold: whether a cumulative
// power-button hold of `powerButtonHeldMs` - as reported by
// gpio.getPowerButtonHeldTime(), measured from the press that woke the device -
// has reached MANUAL_RECEIVER_HOLD_MS. Lives here so the threshold semantics
// are host-testable; main.cpp owns the bounded polling of the live GPIO around
// it (waiting for the crossing or for the release, never indefinitely).
bool manualReceiverHoldMet(uint32_t powerButtonHeldMs);

// Chooses what the boot does with an armed Agenda cycle.
//
// NormalReader unless a timer wake (or a manual power-hold request) earns a
// receiver window. A manual request - a deliberate ~1s power-button hold during
// an armed Agenda sleep - opens the in-process receiver immediately, but only
// when one is linked: without it the long hold falls back to the normal reader
// wake (a partition switch from a button press is out of scope). `holdQualified`
// only ever matters for a PowerButton wake; timer and other wake sources ignore
// it, keeping the automatic schedule unchanged.
AgendaBootRoute chooseAgendaBootRoute(bool agendaCycleArmed, WakeSource wakeSource, bool inProcessAvailable = false,
                                      bool holdQualified = false);
uint32_t encodeReceiverResultWord(ReceiverResult result);
ReceiverResult decodeReceiverResultWord(uint32_t word);

}  // namespace dashboard
