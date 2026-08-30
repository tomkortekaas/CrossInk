#include <gtest/gtest.h>

#include "AgendaWakePolicy.h"

#ifndef CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES
#define CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES 15
#endif

TEST(AgendaWakePolicy, ArmsRelativeTimerOnlyForAgendaSleep) {
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 12, 0),
            static_cast<uint64_t>(CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES) * 60ULL * 1000000ULL);
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(false, 12, 0), 0ULL);
}

TEST(AgendaWakePolicy, SleepsThroughTheNightOutsideTheWakeWindow) {
  // 23:00 -> sleep 8 hours straight to 07:00, not another 15-minute tick.
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 23, 0), 8ULL * 60ULL * 60ULL * 1000000ULL);
  // 03:30 -> sleep the remaining 3.5 hours to 07:00.
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 3, 30), (3ULL * 60ULL + 30ULL) * 60ULL * 1000000ULL);
  // Exactly at the window end (22:00) counts as outside the window.
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 22, 0), 9ULL * 60ULL * 60ULL * 1000000ULL);
}

TEST(AgendaWakePolicy, CapsTheLastTickOfTheDayAtTheWindowEnd) {
  // 21:50 -> only 10 minutes left before the window closes, not the full 15.
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 21, 50), 10ULL * 60ULL * 1000000ULL);
  // Exactly at the window start (07:00) still gets the full interval.
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 7, 0),
            static_cast<uint64_t>(CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES) * 60ULL * 1000000ULL);
}

// The RTC runs in UTC; the wake window is meant in local time. These two are the
// same number only for a user on UTC, which is why the bug below stayed hidden.
TEST(AgendaWakePolicy, ShiftsUtcByTheConfiguredQuarterHourOffset) {
  // 48 is the encoding's zero point: UTC in, UTC out.
  EXPECT_EQ(dashboard::localMinuteOfDay(12, 0, 48), 12U * 60U);
  // 56 = +2h, Amsterdam in summer.
  EXPECT_EQ(dashboard::localMinuteOfDay(12, 0, 56), 14U * 60U);
  // 40 = -2h.
  EXPECT_EQ(dashboard::localMinuteOfDay(12, 0, 40), 10U * 60U);
  // Quarter-hour resolution, not whole hours: 46 = -30m.
  EXPECT_EQ(dashboard::localMinuteOfDay(12, 0, 46), 11U * 60U + 30U);
}

TEST(AgendaWakePolicy, WrapsAroundMidnightInBothDirections) {
  // 23:30 UTC at +2h is 01:30 the next day, not minute 1530.
  EXPECT_EQ(dashboard::localMinuteOfDay(23, 30, 56), 90U);
  // 00:30 UTC at -2h is 22:30 the previous day.
  EXPECT_EQ(dashboard::localMinuteOfDay(0, 30, 40), 22U * 60U + 30U);
}

// The offset reaches this code from an NVS mirror that may be empty on the first
// boot after an update, or corrupt. Falling back to UTC keeps the device on the
// old behaviour rather than sending it to a wildly wrong window.
TEST(AgendaWakePolicy, FallsBackToUtcForAnOutOfRangeOffset) {
  // The setting's own range is {0, 104}; anything past it never came from a user.
  EXPECT_EQ(dashboard::localMinuteOfDay(12, 0, 105), 12U * 60U);
  EXPECT_EQ(dashboard::localMinuteOfDay(12, 0, 255), 12U * 60U);
}

// The bug this fixes, as observed in the trace on 2026-08-16: the device kept
// waking every 15 minutes until 22:00 UTC, two hours past the window end,
// because the timeout path had no offset and read 20:00 UTC as 20:00 local.
TEST(AgendaWakePolicy, ClosesTheWindowOnLocalTimeNotUtc) {
  // 20:00 UTC is 22:00 in Amsterdam: the window has closed, sleep to 07:00.
  const uint16_t local = dashboard::localMinuteOfDay(20, 0, 56);
  EXPECT_EQ(local, 22U * 60U);
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, static_cast<uint8_t>(local / 60), static_cast<uint8_t>(local % 60)),
            9ULL * 60ULL * 60ULL * 1000000ULL);
  // What it did instead: treat it as 20:00 local and take another 15-minute tick.
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 20, 0),
            static_cast<uint64_t>(CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES) * 60ULL * 1000000ULL);
}

// The wake window has to land on a fixed grid — :00, :15, :30, :45 for the
// default interval — rather than "interval minutes from whenever we happened to
// fall asleep".
//
// Two reasons, and the second is the important one. A button press wakes the
// device, and going back to sleep used to restart the count, so every touch
// pushed the next BLE window later; the X3 sticks to the back of a phone, so it
// gets touched. And a drifting window cannot be predicted by the phone, which
// is what any fix on that side needs: you cannot wake an app in time for a
// moment you cannot compute. Anchoring to the clock makes the schedule the same
// on both devices without them having to agree on anything.
TEST(AgendaWakePolicy, AnchorsWakesToTheClockGridNotToWhenItFellAsleep) {
  // 12:07 -> the next grid point is 12:15, so 8 minutes. Waking at 12:22
  // (a full interval later) is the drift this prevents.
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 12, 7), 8ULL * 60ULL * 1000000ULL);
  // A button press a minute later must not push the window out again: 12:08
  // still targets 12:15.
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 12, 8), 7ULL * 60ULL * 1000000ULL);
  // Just before a grid point, the wait is short rather than rounded up.
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 12, 14), 1ULL * 60ULL * 1000000ULL);
  // Exactly on a grid point takes the whole interval to the next one — never
  // zero, which would wake the device immediately and spin.
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 12, 15),
            static_cast<uint64_t>(CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES) * 60ULL * 1000000ULL);
}

// A phone-supplied interval that does not divide the hour still gets a stable
// grid: the anchor is midnight, so the schedule repeats daily even when it does
// not repeat hourly.
TEST(AgendaWakePolicy, AnchorsAnUnevenIntervalToMidnight) {
  // 7-minute interval from 07:00: grid points are 07:00, 07:07, 07:14...
  // At 07:10 the next one is 07:14, so 4 minutes.
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 7, 10, 7, 7, 22), 4ULL * 60ULL * 1000000ULL);
  // And exactly on one still takes the full interval.
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 7, 14, 7, 7, 22), 7ULL * 60ULL * 1000000ULL);
}

// The window end still wins over the grid: the last tick of the day is capped
// there, so the device never wakes for a window that has already closed.
TEST(AgendaWakePolicy, StillCapsTheGridAtTheWindowEnd) {
  // 21:58 with a 15-minute grid: the next grid point is 22:00, which is also
  // the window end — 2 minutes, and the wake after that is the night block.
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 21, 58), 2ULL * 60ULL * 1000000ULL);
}

TEST(AgendaWakePolicy, RoutesOnlyTimerWakeToReceiver) {
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::Timer), dashboard::AgendaBootRoute::Receiver);
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::PowerButton),
            dashboard::AgendaBootRoute::NormalReader);
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(false, dashboard::WakeSource::Timer),
            dashboard::AgendaBootRoute::NormalReader);
}

TEST(AgendaWakePolicy, PrefersTheInProcessReceiverWhenAvailable) {
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::Timer, true),
            dashboard::AgendaBootRoute::InProcessReceiver);
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::Timer, false),
            dashboard::AgendaBootRoute::Receiver);
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::PowerButton, true),
            dashboard::AgendaBootRoute::NormalReader);
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(false, dashboard::WakeSource::Timer, true),
            dashboard::AgendaBootRoute::NormalReader);
}

TEST(AgendaWakePolicy, AcceptsOnlyValidOneShotReceiverResults) {
  EXPECT_EQ(dashboard::decodeReceiverResultWord(
                dashboard::encodeReceiverResultWord(dashboard::ReceiverResult::AwaitingWindow)),
            dashboard::ReceiverResult::AwaitingWindow);
  EXPECT_EQ(
      dashboard::decodeReceiverResultWord(dashboard::encodeReceiverResultWord(dashboard::ReceiverResult::Accepted)),
      dashboard::ReceiverResult::Accepted);
  EXPECT_EQ(
      dashboard::decodeReceiverResultWord(dashboard::encodeReceiverResultWord(dashboard::ReceiverResult::TimedOut)),
      dashboard::ReceiverResult::TimedOut);
  EXPECT_EQ(dashboard::decodeReceiverResultWord(0xA63E4DFFU), dashboard::ReceiverResult::None);
  EXPECT_EQ(dashboard::decodeReceiverResultWord(0xA73E4D01U), dashboard::ReceiverResult::None);
}

TEST(AgendaWakePolicy, UsesPhoneSuppliedIntervalAndWindow) {
  // 10-minute interval, 08:00-20:00 window, at 08:00 -> first tick is a full 10 minutes.
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 8, 0, 10, 8, 20), 10ULL * 60ULL * 1000000ULL);
  // At 19:55 with a 10-minute interval, only 5 minutes are left before 20:00.
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 19, 55, 10, 8, 20), 5ULL * 60ULL * 1000000ULL);
  // At 21:00, outside the narrowed window, sleep straight through to 08:00 (11 hours).
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true, 21, 0, 10, 8, 20), 11ULL * 60ULL * 60ULL * 1000000ULL);
}

TEST(ClampWakeSettings, PassesThroughValidValues) {
  const auto settings = dashboard::clampWakeSettings(10, 8, 20);
  EXPECT_EQ(settings.intervalMinutes, 10U);
  EXPECT_EQ(settings.windowStartHour, 8U);
  EXPECT_EQ(settings.windowEndHour, 20U);
}

TEST(ClampWakeSettings, FallsBackToDefaultIntervalWhenOutOfRange) {
  EXPECT_EQ(dashboard::clampWakeSettings(0, 8, 20).intervalMinutes, dashboard::AGENDA_WAKE_INTERVAL_MINUTES);
  EXPECT_EQ(dashboard::clampWakeSettings(61, 8, 20).intervalMinutes, dashboard::AGENDA_WAKE_INTERVAL_MINUTES);
  EXPECT_EQ(dashboard::clampWakeSettings(255, 8, 20).intervalMinutes, dashboard::AGENDA_WAKE_INTERVAL_MINUTES);
}

TEST(ClampWakeSettings, FallsBackToDefaultWindowWhenStartIsNotBeforeEnd) {
  const auto settings = dashboard::clampWakeSettings(15, 20, 8);
  EXPECT_EQ(settings.windowStartHour, dashboard::AGENDA_WAKE_WINDOW_START_HOUR);
  EXPECT_EQ(settings.windowEndHour, dashboard::AGENDA_WAKE_WINDOW_END_HOUR);
}

TEST(ClampWakeSettings, FallsBackToDefaultWindowWhenAnHourIsOutOfRange) {
  const auto settings = dashboard::clampWakeSettings(15, 24, 20);
  EXPECT_EQ(settings.windowStartHour, dashboard::AGENDA_WAKE_WINDOW_START_HOUR);
  EXPECT_EQ(settings.windowEndHour, dashboard::AGENDA_WAKE_WINDOW_END_HOUR);
}

TEST(UtcEpochSecondsFromCivil, ConvertsAKnownInstant) {
  // 2026-08-30T12:00:00Z. Day 20695 since the epoch: 20695 * 86400 + 43200.
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2026, 8, 30, 12, 0), 1788091200ULL);
}

TEST(UtcEpochSecondsFromCivil, ConvertsTheEpochItself) {
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(1970, 1, 1, 0, 0), 0ULL);
}

TEST(UtcEpochSecondsFromCivil, HandlesALeapDay) {
  // 2024-02-29T00:00:00Z = 1709164800.
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2024, 2, 29, 0, 0), 1709164800ULL);
}

TEST(UtcEpochSecondsFromCivil, ReadsTheUnsetRtcDateAsAVeryOldTime) {
  // An RTC that was never set reads 2000-01-01 on this hardware. That is a
  // valid date, so it converts; the staleness rule is what rejects it, by
  // finding a package generated after it.
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2000, 1, 1, 0, 0), 946684800ULL);
}

TEST(UtcEpochSecondsFromCivil, RejectsOutOfRangeFieldsWithZero) {
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2026, 0, 30, 12, 0), 0ULL);
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2026, 13, 30, 12, 0), 0ULL);
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2026, 8, 0, 12, 0), 0ULL);
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2026, 8, 32, 12, 0), 0ULL);
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2026, 8, 30, 24, 0), 0ULL);
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2026, 8, 30, 12, 60), 0ULL);
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(1969, 8, 30, 12, 0), 0ULL);
}

namespace {
// 2026-08-30T12:00:00Z, the instant used as "now" throughout these tests.
constexpr uint64_t NOW = 1788091200ULL;
}  // namespace

TEST(ShouldRefreshAtStandby, RefreshesWhenTheCardIsOlderThanTheInterval) {
  EXPECT_TRUE(dashboard::shouldRefreshAtStandby(true, true, NOW, NOW - 3600, 15));
}

TEST(ShouldRefreshAtStandby, RefreshesExactlyAtTheInterval) {
  EXPECT_TRUE(dashboard::shouldRefreshAtStandby(true, true, NOW, NOW - 15 * 60, 15));
}

TEST(ShouldRefreshAtStandby, LeavesAFreshCardAlone) {
  // A glance: picked up and put down again well inside the interval.
  EXPECT_FALSE(dashboard::shouldRefreshAtStandby(true, true, NOW, NOW - 14 * 60, 15));
}

TEST(ShouldRefreshAtStandby, DoesNotRefreshAfterAJustAcceptedPackage) {
  // This is what stops the loop: the boot that renders an accepted package
  // sleeps through this same rule, seconds after the package was generated.
  EXPECT_FALSE(dashboard::shouldRefreshAtStandby(true, true, NOW, NOW - 5, 15));
}

TEST(ShouldRefreshAtStandby, NeverRefreshesOnANonAgendaSleep) {
  EXPECT_FALSE(dashboard::shouldRefreshAtStandby(false, true, NOW, NOW - 3600, 15));
}

TEST(ShouldRefreshAtStandby, NeverRefreshesWithoutAClock) {
  EXPECT_FALSE(dashboard::shouldRefreshAtStandby(true, false, NOW, NOW - 3600, 15));
}

TEST(ShouldRefreshAtStandby, NeverRefreshesWithoutAPackage) {
  EXPECT_FALSE(dashboard::shouldRefreshAtStandby(true, true, NOW, 0, 15));
}

TEST(ShouldRefreshAtStandby, NeverRefreshesWhenTheClockIsBehindThePackage) {
  // An unset RTC reads 2000-01-01, which is before any package it holds.
  EXPECT_FALSE(dashboard::shouldRefreshAtStandby(true, true, 946684800ULL, NOW, 15));
}

TEST(ShouldRefreshAtStandby, NeverRefreshesOnAZeroInterval) {
  EXPECT_FALSE(dashboard::shouldRefreshAtStandby(true, true, NOW, NOW - 3600, 0));
}

TEST(ShouldRefreshAtStandby, RespectsAPhoneSuppliedInterval) {
  // clampWakeSettings allows 1-60 minutes; a 60-minute interval means a
  // 30-minute-old card is still fresh.
  EXPECT_FALSE(dashboard::shouldRefreshAtStandby(true, true, NOW, NOW - 30 * 60, 60));
  EXPECT_TRUE(dashboard::shouldRefreshAtStandby(true, true, NOW, NOW - 61 * 60, 60));
}

TEST(ActionAfterInProcessWindow, AnAcceptedPackageEarnsAFullBoot) {
  EXPECT_EQ(dashboard::actionAfterInProcessWindow(dashboard::ReceiverResult::Accepted),
            dashboard::InProcessWindowAction::ContinueBoot);
}

TEST(ActionAfterInProcessWindow, ATimedOutWindowSleepsToTheNextTick) {
  EXPECT_EQ(dashboard::actionAfterInProcessWindow(dashboard::ReceiverResult::TimedOut),
            dashboard::InProcessWindowAction::SleepToNextTick);
}

TEST(ActionAfterInProcessWindow, AVerdictlessWindowSleepsToTheNextTick) {
  // The receiver produced no verdict at all. From the user's side nothing
  // arrived, which is the same outcome as a timeout.
  EXPECT_EQ(dashboard::actionAfterInProcessWindow(dashboard::ReceiverResult::None),
            dashboard::InProcessWindowAction::SleepToNextTick);
  EXPECT_EQ(dashboard::actionAfterInProcessWindow(dashboard::ReceiverResult::AwaitingWindow),
            dashboard::InProcessWindowAction::SleepToNextTick);
}
