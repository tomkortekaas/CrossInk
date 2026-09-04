#include <gtest/gtest.h>

#include <cstdint>

#include "AgendaWakePolicy.h"
#include "ReceiverWindow.h"

// The approved manual navigator receive window (dashboard-x3): while the Agenda
// sleep frame is visible, holding the physical power/right button for about one
// second requests an on-demand in-process BLE receiver window. A short press
// keeps waking straight into the reader, and an empty manual window returns
// directly to Agenda sleep on the normal fixed-grid timer.
//
// These tests pin the pure, host-testable policy surface that main.cpp feeds
// with the live GPIO state: the hold threshold and window-length constants, the
// hold boundary, the boot-route decision, and the empty-window action.

TEST(ManualReceiverWindowPolicy, HoldThresholdIsOneSecond) { EXPECT_EQ(dashboard::MANUAL_RECEIVER_HOLD_MS, 1000U); }

TEST(ManualReceiverWindowPolicy, ManualWindowIsThirtySecondsAndLongerThanAutomatic) {
  // Longer than the automatic window because the phone was not scheduled for
  // this wake and may need time to notice and connect.
  EXPECT_EQ(dashboard::MANUAL_RECEIVER_WINDOW_MS, 30000U);
  // The existing automatic quarter-hour timer window keeps its own length.
  EXPECT_EQ(dashboard::ReceiverWindow::DEFAULT_WINDOW_MS, 20000U);
  EXPECT_GT(dashboard::MANUAL_RECEIVER_WINDOW_MS, dashboard::ReceiverWindow::DEFAULT_WINDOW_MS);
}

TEST(ManualReceiverWindowPolicy, HoldBoundaryQualifiesAtOneSecond) {
  EXPECT_FALSE(dashboard::manualReceiverHoldMet(0U));
  EXPECT_FALSE(dashboard::manualReceiverHoldMet(999U));
  EXPECT_TRUE(dashboard::manualReceiverHoldMet(1000U));
  EXPECT_TRUE(dashboard::manualReceiverHoldMet(1500U));
}

TEST(ManualReceiverWindowPolicy, ArmedPowerHoldRoutesToTheManualInProcessReceiver) {
  // The feature: armed Agenda sleep + power-button wake + qualifying hold.
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::PowerButton, true, true),
            dashboard::AgendaBootRoute::ManualInProcessReceiver);
}

TEST(ManualReceiverWindowPolicy, ManualRouteIsDistinctFromTheScheduledRoute) {
  EXPECT_NE(dashboard::AgendaBootRoute::ManualInProcessReceiver, dashboard::AgendaBootRoute::InProcessReceiver);
}

TEST(ManualReceiverWindowPolicy, ShortPowerWakeStillReadsNormally) {
  // No qualifying hold: an armed Agenda sleep power wake behaves exactly as
  // before the manual window existed - the reader wakes without any 30 s delay.
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::PowerButton, true, false),
            dashboard::AgendaBootRoute::NormalReader);
  // The historical two-argument call keeps its meaning too.
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::PowerButton),
            dashboard::AgendaBootRoute::NormalReader);
}

TEST(ManualReceiverWindowPolicy, ScheduledTimerRoutesAreUnchanged) {
  // In-process available: the automatic timer wake still opens the scheduled
  // in-process receiver, never a manual window.
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::Timer, true, false),
            dashboard::AgendaBootRoute::InProcessReceiver);
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::Timer, true, true),
            dashboard::AgendaBootRoute::InProcessReceiver);
  // Partition build: still the isolated receiver route.
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::Timer, false, false),
            dashboard::AgendaBootRoute::Receiver);
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::Timer), dashboard::AgendaBootRoute::Receiver);
}

TEST(ManualReceiverWindowPolicy, UnarmedOrNonAgendaLongHoldStillReadsNormally) {
  // Never-armed (or non-Agenda) sleep: a long hold must not open a window.
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(false, dashboard::WakeSource::PowerButton, true, true),
            dashboard::AgendaBootRoute::NormalReader);
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(false, dashboard::WakeSource::Timer, true, true),
            dashboard::AgendaBootRoute::NormalReader);
}

TEST(ManualReceiverWindowPolicy, OtherWakeSourcesAreUnaffectedByAHold) {
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::Other, true, true),
            dashboard::AgendaBootRoute::NormalReader);
}

TEST(ManualReceiverWindowPolicy, HoldRequiresTheInProcessReceiver) {
  // The manual window runs through the already-linked in-process receiver. In a
  // build without one a long hold falls back to the normal reader wake;
  // switching partitions from a button press is out of scope.
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::PowerButton, false, true),
            dashboard::AgendaBootRoute::NormalReader);
}

TEST(ManualReceiverWindowPolicy, EmptyManualWindowSleepsToTheNextAgendaTick) {
  // The empty-manual-window tail is the same as the scheduled one: tear the BLE
  // stack down and return straight to Agenda sleep on the fixed grid. No reader
  // boot, no dashboard-replaced-by-home.
  EXPECT_EQ(dashboard::actionAfterInProcessWindow(dashboard::ReceiverResult::TimedOut),
            dashboard::InProcessWindowAction::SleepToNextTick);
  EXPECT_EQ(dashboard::actionAfterInProcessWindow(dashboard::ReceiverResult::None),
            dashboard::InProcessWindowAction::SleepToNextTick);
  EXPECT_EQ(dashboard::actionAfterInProcessWindow(dashboard::ReceiverResult::AwaitingWindow),
            dashboard::InProcessWindowAction::SleepToNextTick);
  // Only an accepted package earns the rest of the boot.
  EXPECT_EQ(dashboard::actionAfterInProcessWindow(dashboard::ReceiverResult::Accepted),
            dashboard::InProcessWindowAction::ContinueBoot);
}
