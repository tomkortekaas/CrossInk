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
