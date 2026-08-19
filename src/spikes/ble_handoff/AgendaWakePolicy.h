#pragma once

#include <cstdint>

namespace dashboard {

enum class WakeSource : uint8_t { Other, PowerButton, Timer };
enum class AgendaBootRoute : uint8_t { NormalReader, Receiver };
enum class ReceiverResult : uint8_t { None, AwaitingWindow, Accepted, TimedOut };

#ifndef CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES
#define CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES 15
#endif
constexpr uint32_t AGENDA_WAKE_INTERVAL_MINUTES = CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES;
constexpr uint8_t AGENDA_WAKE_WINDOW_START_HOUR = 7;
constexpr uint8_t AGENDA_WAKE_WINDOW_END_HOUR = 22;

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
// local time. Wakes happen every `intervalMinutes` between `windowStartHour`
// and `windowEndHour`; outside that window, sleeps in one block straight
// through to the next `windowStartHour`. The three settings default to the
// compiled-in fallback so every existing call site keeps working unchanged.
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

AgendaBootRoute chooseAgendaBootRoute(bool agendaCycleArmed, WakeSource wakeSource);
uint32_t encodeReceiverResultWord(ReceiverResult result);
ReceiverResult decodeReceiverResultWord(uint32_t word);

}  // namespace dashboard
