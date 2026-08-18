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

TEST(AgendaWakePolicy, RoutesOnlyTimerWakeToReceiver) {
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::Timer), dashboard::AgendaBootRoute::Receiver);
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::PowerButton),
            dashboard::AgendaBootRoute::NormalReader);
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(false, dashboard::WakeSource::Timer),
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
