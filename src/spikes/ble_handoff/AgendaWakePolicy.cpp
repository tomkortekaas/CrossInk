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

// Howard Hinnant's days_from_civil against the Unix epoch: the inverse of the
// civil_from_days in DashboardV3Renderer.cpp. Pure integer arithmetic, no
// <ctime> and no timezone database, so it runs identically on the host and on
// the C3. Valid for any proleptic Gregorian date; the caller range-checks.
int64_t daysFromCivil(int64_t y, const unsigned m, const unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
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

uint64_t utcEpochSecondsFromCivil(const uint16_t year, const uint8_t month, const uint8_t day, const uint8_t hour,
                                  const uint8_t minute) {
  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59) return 0;
  const int64_t days = daysFromCivil(static_cast<int64_t>(year), month, day);
  if (days < 0) return 0;
  return static_cast<uint64_t>(days) * 86400ULL + static_cast<uint64_t>(hour) * 3600ULL +
         static_cast<uint64_t>(minute) * 60ULL;
}

bool shouldRefreshAtStandby(const bool agendaSleep, const bool clockAvailable, const uint64_t nowEpochSeconds,
                            const uint64_t packageGeneratedAt, const uint32_t intervalMinutes) {
  if (!agendaSleep || !clockAvailable) return false;
  if (packageGeneratedAt == 0 || intervalMinutes == 0) return false;
  if (nowEpochSeconds < packageGeneratedAt) return false;
  return (nowEpochSeconds - packageGeneratedAt) >= static_cast<uint64_t>(intervalMinutes) * 60ULL;
}

InProcessWindowAction actionAfterInProcessWindow(const ReceiverResult result) {
  return result == ReceiverResult::Accepted ? InProcessWindowAction::ContinueBoot
                                            : InProcessWindowAction::SleepToNextTick;
}

uint16_t localMinuteOfDay(const uint8_t utcHour, const uint8_t utcMinute, const uint8_t offsetQ) {
  const int biased = offsetQ <= OFFSET_Q_MAX ? static_cast<int>(offsetQ) : OFFSET_Q_UTC;
  const int shifted =
      static_cast<int>(utcHour) * 60 + static_cast<int>(utcMinute) + (biased - OFFSET_Q_UTC) * MINUTES_PER_QUARTER;
  const int perDay = static_cast<int>(MINUTES_PER_DAY);
  // Twice, because a negative offset before midnight leaves a negative remainder.
  return static_cast<uint16_t>((shifted % perDay + perDay) % perDay);
}

bool manualReceiverHoldMet(const uint32_t powerButtonHeldMs) { return powerButtonHeldMs >= MANUAL_RECEIVER_HOLD_MS; }

AgendaBootRoute chooseAgendaBootRoute(const bool agendaCycleArmed, const WakeSource wakeSource,
                                      const bool inProcessAvailable, const bool holdQualified) {
  if (!agendaCycleArmed) return AgendaBootRoute::NormalReader;
  if (wakeSource == WakeSource::PowerButton) {
    // A deliberate ~1s hold during an armed Agenda sleep is a manual request for
    // an immediate in-process receiver window. Without an in-process receiver
    // there is nothing to open on this boot, so the long hold then falls back to
    // the normal reader wake.
    return holdQualified && inProcessAvailable ? AgendaBootRoute::ManualInProcessReceiver
                                               : AgendaBootRoute::NormalReader;
  }
  if (wakeSource != WakeSource::Timer) return AgendaBootRoute::NormalReader;
  return inProcessAvailable ? AgendaBootRoute::InProcessReceiver : AgendaBootRoute::Receiver;
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
