#include <gtest/gtest.h>

#include "ReceiverWindow.h"

TEST(ReceiverWindow, RemainsListeningBeforeHardDeadline) {
  dashboard::ReceiverWindow window(1'000U);
  EXPECT_EQ(window.actionAt(20'999U, false), dashboard::ReceiverWindowAction::Listen);
}

TEST(ReceiverWindow, ReturnsToReaderAtHardDeadlineWithoutTransfer) {
  dashboard::ReceiverWindow window(1'000U);
  EXPECT_EQ(window.actionAt(21'000U, false), dashboard::ReceiverWindowAction::ReturnTimedOut);
}

TEST(ReceiverWindow, AcceptedTransferWinsBeforeDeadline) {
  dashboard::ReceiverWindow window(1'000U);
  EXPECT_EQ(window.actionAt(4'000U, true), dashboard::ReceiverWindowAction::ReturnAccepted);
}

TEST(ReceiverWindow, AcceptedTransferRemainsDominantAtDeadline) {
  dashboard::ReceiverWindow window(1'000U);
  EXPECT_EQ(window.actionAt(21'000U, true), dashboard::ReceiverWindowAction::ReturnAccepted);
}

TEST(ReceiverWindow, DeadlineComparisonSurvivesMillisWraparound) {
  dashboard::ReceiverWindow window(UINT32_MAX - 9'999U);
  EXPECT_EQ(window.actionAt(9'999U, false), dashboard::ReceiverWindowAction::Listen);
  EXPECT_EQ(window.actionAt(10'000U, false), dashboard::ReceiverWindowAction::ReturnTimedOut);
}

// The window length is a constructor argument, not a fixed constant, so that
// `runReceiverWindow(ms)` does what its name promises. It was briefly the other
// way round: the parameter was accepted and then ignored, which meant a caller
// asking for a five-second window silently got twenty. A signature that lies is
// worse than one that never offered the choice.
TEST(ReceiverWindow, HonoursAWindowShorterThanTheDefault) {
  dashboard::ReceiverWindow window(1'000U, 5'000U);
  EXPECT_EQ(window.actionAt(5'999U, false), dashboard::ReceiverWindowAction::Listen);
  EXPECT_EQ(window.actionAt(6'000U, false), dashboard::ReceiverWindowAction::ReturnTimedOut);
}

// Omitting the length keeps the twenty seconds every existing caller relies on.
TEST(ReceiverWindow, DefaultsToTheTwentySecondWindow) {
  dashboard::ReceiverWindow window(1'000U);
  EXPECT_EQ(window.actionAt(20'999U, false), dashboard::ReceiverWindowAction::Listen);
  EXPECT_EQ(window.actionAt(21'000U, false), dashboard::ReceiverWindowAction::ReturnTimedOut);
}
