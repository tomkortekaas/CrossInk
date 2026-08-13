#pragma once

#include <cstdint>

namespace dashboard {

enum class WakeSource : uint8_t { Other, PowerButton, Timer };
enum class AgendaBootRoute : uint8_t { NormalReader, Receiver };
enum class ReceiverResult : uint8_t { None, AwaitingWindow, Accepted, TimedOut };

uint64_t sleepTimerIntervalUs(bool agendaSleep);
AgendaBootRoute chooseAgendaBootRoute(bool agendaCycleArmed, WakeSource wakeSource);
uint32_t encodeReceiverResultWord(ReceiverResult result);
ReceiverResult decodeReceiverResultWord(uint32_t word);

}  // namespace dashboard
