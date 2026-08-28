#include "AgendaWakePolicy.h"

namespace dashboard {
namespace {

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

uint64_t sleepTimerIntervalUs(const bool agendaSleep, const uint8_t currentHour, const uint8_t currentMinute,
                               const uint32_t intervalMinutes, const uint8_t windowStartHour,
                               const uint8_t windowEndHour) {
  if (!agendaSleep) return 0;
  const uint32_t windowStartMinute = static_cast<uint32_t>(windowStartHour) * 60U;
  const uint32_t windowEndMinute = static_cast<uint32_t>(windowEndHour) * 60U;
  const uint32_t nowMinute = static_cast<uint32_t>(currentHour) * 60U + currentMinute;
  uint32_t sleepMinutes;
  if (nowMinute < windowStartMinute) {
    sleepMinutes = windowStartMinute - nowMinute;
  } else if (nowMinute >= windowEndMinute) {
    sleepMinutes = (MINUTES_PER_DAY - nowMinute) + windowStartMinute;
  } else {
    // Sleep to the next point on a fixed grid anchored at midnight, not for a
    // whole interval from now. Those are the same thing only when the device
    // fell asleep on a grid point, and it usually does not: a button press
    // wakes it mid-interval, and counting afresh from there pushed the next BLE
    // window later every single time. The X3 sticks to the back of a phone, so
    // it gets pressed, and the window drifted all day.
    //
    // The grid also makes the schedule computable from the clock alone, which
    // is what lets the phone know when to be awake without the two devices
    // having to agree on anything.
    const uint32_t interval = intervalMinutes > 0 ? intervalMinutes : 1;
    // Runs 1..interval rather than 0..interval-1: landing exactly on a grid
    // point means the *next* one is a full interval away. A zero here would
    // arm a timer that fires immediately and spin the device.
    const uint32_t minutesUntilGrid = interval - (nowMinute % interval);
    const uint32_t minutesUntilWindowEnd = windowEndMinute - nowMinute;
    sleepMinutes = minutesUntilWindowEnd < minutesUntilGrid ? minutesUntilWindowEnd : minutesUntilGrid;
  }
  return static_cast<uint64_t>(sleepMinutes) * 60ULL * 1000000ULL;
}

WakeSettings clampWakeSettings(const uint8_t rawIntervalMinutes, const uint8_t rawWindowStartHour,
                                const uint8_t rawWindowEndHour) {
  WakeSettings settings;
  if (rawIntervalMinutes >= 1 && rawIntervalMinutes <= 60) {
    settings.intervalMinutes = rawIntervalMinutes;
  }
  if (rawWindowStartHour <= 23 && rawWindowEndHour <= 23 && rawWindowStartHour < rawWindowEndHour) {
    settings.windowStartHour = rawWindowStartHour;
    settings.windowEndHour = rawWindowEndHour;
  }
  return settings;
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
