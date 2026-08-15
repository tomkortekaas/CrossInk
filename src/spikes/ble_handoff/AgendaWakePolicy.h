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
AgendaBootRoute chooseAgendaBootRoute(bool agendaCycleArmed, WakeSource wakeSource);
uint32_t encodeReceiverResultWord(ReceiverResult result);
ReceiverResult decodeReceiverResultWord(uint32_t word);

}  // namespace dashboard
