#include "AgendaWakePolicy.h"

namespace dashboard {
namespace {

#ifndef CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES
#define CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES 15
#endif
constexpr uint32_t AGENDA_WAKE_INTERVAL_MINUTES = CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES;
constexpr uint32_t AGENDA_WAKE_WINDOW_START_MINUTE = 7 * 60;
constexpr uint32_t AGENDA_WAKE_WINDOW_END_MINUTE = 22 * 60;
constexpr uint32_t MINUTES_PER_DAY = 24 * 60;
constexpr uint32_t RECEIVER_RESULT_WORD_MAGIC = 0xA63E4D00U;

bool isPersistableResult(const ReceiverResult result) {
  return result == ReceiverResult::AwaitingWindow || result == ReceiverResult::Accepted ||
         result == ReceiverResult::TimedOut;
}

}  // namespace

uint64_t sleepTimerIntervalUs(const bool agendaSleep, const uint8_t currentHour, const uint8_t currentMinute) {
  if (!agendaSleep) return 0;
  const uint32_t nowMinute = static_cast<uint32_t>(currentHour) * 60U + currentMinute;
  uint32_t sleepMinutes;
  if (nowMinute < AGENDA_WAKE_WINDOW_START_MINUTE) {
    sleepMinutes = AGENDA_WAKE_WINDOW_START_MINUTE - nowMinute;
  } else if (nowMinute >= AGENDA_WAKE_WINDOW_END_MINUTE) {
    sleepMinutes = (MINUTES_PER_DAY - nowMinute) + AGENDA_WAKE_WINDOW_START_MINUTE;
  } else {
    const uint32_t minutesUntilWindowEnd = AGENDA_WAKE_WINDOW_END_MINUTE - nowMinute;
    sleepMinutes = minutesUntilWindowEnd < AGENDA_WAKE_INTERVAL_MINUTES ? minutesUntilWindowEnd
                                                                        : AGENDA_WAKE_INTERVAL_MINUTES;
  }
  return static_cast<uint64_t>(sleepMinutes) * 60ULL * 1000000ULL;
}

AgendaBootRoute chooseAgendaBootRoute(const bool agendaCycleArmed, const WakeSource wakeSource) {
  return agendaCycleArmed && wakeSource == WakeSource::Timer ? AgendaBootRoute::Receiver
                                                             : AgendaBootRoute::NormalReader;
}

uint32_t encodeReceiverResultWord(const ReceiverResult result) {
  return isPersistableResult(result) ? RECEIVER_RESULT_WORD_MAGIC | static_cast<uint32_t>(result) : 0;
}

ReceiverResult decodeReceiverResultWord(const uint32_t word) {
  if ((word & 0xFFFFFF00U) != RECEIVER_RESULT_WORD_MAGIC) return ReceiverResult::None;
  const auto result = static_cast<ReceiverResult>(word & 0xFFU);
  return isPersistableResult(result) ? result : ReceiverResult::None;
}

}  // namespace dashboard
