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

// SETTINGS.clockUtcOffsetQ encoding: quarter hours biased by 48, so 48 is UTC.
// The upper bound mirrors the {0, 104} range declared in SettingsList.h.
constexpr int OFFSET_Q_UTC = 48;
constexpr int OFFSET_Q_MAX = 104;
constexpr int MINUTES_PER_QUARTER = 15;

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
    sleepMinutes =
        minutesUntilWindowEnd < AGENDA_WAKE_INTERVAL_MINUTES ? minutesUntilWindowEnd : AGENDA_WAKE_INTERVAL_MINUTES;
  }
  return static_cast<uint64_t>(sleepMinutes) * 60ULL * 1000000ULL;
}

uint16_t localMinuteOfDay(const uint8_t utcHour, const uint8_t utcMinute, const uint8_t offsetQ) {
  const int biased = offsetQ <= OFFSET_Q_MAX ? static_cast<int>(offsetQ) : OFFSET_Q_UTC;
  const int shifted =
      static_cast<int>(utcHour) * 60 + static_cast<int>(utcMinute) + (biased - OFFSET_Q_UTC) * MINUTES_PER_QUARTER;
  const int perDay = static_cast<int>(MINUTES_PER_DAY);
  // Twice, because a negative offset before midnight leaves a negative remainder.
  return static_cast<uint16_t>((shifted % perDay + perDay) % perDay);
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
