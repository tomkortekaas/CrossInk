#include <gtest/gtest.h>
#include "HomeReceiveBoot.h"
#include "ReceiverWindow.h"

TEST(HomeReceiveBoot, OneShotRequestPreservesBackMapping) {
  for (uint8_t key = 0; key < 4; ++key) {
    uint32_t token = dashboard::homeReceiveToken(key);
    EXPECT_EQ(dashboard::consumeHomeReceiveToken(token, true), key);
    EXPECT_EQ(token, 0U);
    EXPECT_EQ(dashboard::consumeHomeReceiveToken(token, true), -1);
  }
}
TEST(HomeReceiveBoot, ColdBootPanicAndGarbageCannotOpenRadio) {
  uint32_t token = dashboard::homeReceiveToken(0);
  EXPECT_EQ(dashboard::consumeHomeReceiveToken(token, false), -1);
  EXPECT_EQ(token, 0U);
  for (uint32_t bad : {0U, 0xffffffffU, dashboard::homeReceiveToken(0) ^ 0x100U,
                       dashboard::homeReceiveToken(0) | 4U}) {
    EXPECT_EQ(dashboard::consumeHomeReceiveToken(bad, true), -1);
    EXPECT_EQ(bad, 0U);
  }
  EXPECT_EQ(dashboard::homeReceiveToken(4), 0U);
}
TEST(HomeReceiveBoot, SixtySecondsIsAHardDeadlineEvenAcrossWrap) {
  dashboard::ReceiverWindow w(UINT32_MAX - 999U, dashboard::HOME_RECEIVE_WINDOW_MS);
  EXPECT_EQ(w.actionAt(58999U, false), dashboard::ReceiverWindowAction::Listen);
  EXPECT_EQ(w.actionAt(59000U, false), dashboard::ReceiverWindowAction::ReturnTimedOut);
}
TEST(HomeReceiveBoot, BackCancelsButCompletedTransferKeepsAcknowledgement) {
  dashboard::ReceiverWindow w(0, dashboard::HOME_RECEIVE_WINDOW_MS);
  EXPECT_EQ(w.actionAt(10, false, true), dashboard::ReceiverWindowAction::ReturnCancelled);
  EXPECT_EQ(w.actionAt(10, true, true), dashboard::ReceiverWindowAction::ReturnAccepted);
}
