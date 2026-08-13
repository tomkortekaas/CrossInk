#include <gtest/gtest.h>

#include "AgendaWakePolicy.h"

#ifndef CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES
#define CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES 15
#endif

TEST(AgendaWakePolicy, ArmsRelativeTimerOnlyForAgendaSleep) {
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(true),
            static_cast<uint64_t>(CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES) * 60ULL * 1000000ULL);
  EXPECT_EQ(dashboard::sleepTimerIntervalUs(false), 0ULL);
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
