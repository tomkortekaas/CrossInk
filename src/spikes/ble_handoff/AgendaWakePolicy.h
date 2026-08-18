#pragma once

#include <cstdint>

namespace dashboard {

enum class WakeSource : uint8_t { Other, PowerButton, Timer };
enum class AgendaBootRoute : uint8_t { NormalReader, Receiver };
enum class ReceiverResult : uint8_t { None, AwaitingWindow, Accepted, TimedOut };

// Microseconds to sleep before the next Agenda BLE wake, given the current
// local time. Wakes happen every ~15 minutes between 07:00 and 22:00; outside
// that window, sleeps in one block straight through to the next 07:00.
uint64_t sleepTimerIntervalUs(bool agendaSleep, uint8_t currentHour, uint8_t currentMinute);

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
