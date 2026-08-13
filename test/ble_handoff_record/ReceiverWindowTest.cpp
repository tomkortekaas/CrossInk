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
