#include <gtest/gtest.h>

#include "DashboardBleWindow.h"

namespace dashboard_sync {
namespace {

struct ExpirySink {
  int calls = 0;
};

void recordExpiry(void* context) { static_cast<ExpirySink*>(context)->calls++; }

TEST(DashboardBleWindow, ExpiryRequestsRestart) {
  ExpirySink expiry;
  DashboardBleWindow window(10000, 750);
  window.setExpiryCallback(recordExpiry, &expiry);
  window.startedAt(100);

  EXPECT_TRUE(window.tick(10099));
  EXPECT_TRUE(window.isActive());
  EXPECT_EQ(expiry.calls, 0);

  EXPECT_FALSE(window.tick(10100));
  EXPECT_FALSE(window.isActive());
  EXPECT_EQ(expiry.calls, 1);
}

TEST(DashboardBleWindow, ReaderEntryRequestsRestartWithoutTransportTeardown) {
  ExpirySink expiry;
  DashboardBleWindow window(10000, 750);
  window.setExpiryCallback(recordExpiry, &expiry);
  window.startedAt(100);

  window.beforeReaderEnter();

  EXPECT_FALSE(window.isActive());
  EXPECT_EQ(expiry.calls, 1);
}

TEST(DashboardBleWindow, AcceptedFrameSettlesBeforeRestart) {
  ExpirySink expiry;
  DashboardBleWindow window(10000, 750);
  window.setExpiryCallback(recordExpiry, &expiry);
  window.startedAt(100);
  window.acceptedAt(600);

  EXPECT_TRUE(window.tick(1349));
  EXPECT_TRUE(window.isActive());
  EXPECT_EQ(expiry.calls, 0);

  EXPECT_FALSE(window.tick(1350));
  EXPECT_FALSE(window.isActive());
  EXPECT_EQ(expiry.calls, 1);
}

TEST(DashboardBleWindow, AcceptedFrameDoesNotExtendOriginalDeadline) {
  ExpirySink expiry;
  DashboardBleWindow window(10000, 750);
  window.setExpiryCallback(recordExpiry, &expiry);
  window.startedAt(100);
  window.acceptedAt(10000);

  EXPECT_FALSE(window.tick(10100));
  EXPECT_FALSE(window.isActive());
  EXPECT_EQ(expiry.calls, 1);
}

TEST(DashboardBleWindow, FirstAcceptedFrameOwnsSettleDeadline) {
  ExpirySink expiry;
  DashboardBleWindow window(10000, 750);
  window.setExpiryCallback(recordExpiry, &expiry);
  window.startedAt(100);
  window.acceptedAt(600);
  window.acceptedAt(900);

  EXPECT_FALSE(window.tick(1350));
  EXPECT_EQ(expiry.calls, 1);
}

TEST(DashboardBleWindow, DisabledWindowIsIdempotent) {
  ExpirySink expiry;
  DashboardBleWindow window(10000, 750);
  window.setExpiryCallback(recordExpiry, &expiry);

  window.beforeReaderEnter();
  EXPECT_TRUE(window.tick(20000));

  EXPECT_FALSE(window.isActive());
  EXPECT_EQ(expiry.calls, 0);
}

TEST(DashboardBleWindow, RestartRequestIsIdempotent) {
  ExpirySink expiry;
  DashboardBleWindow window(10000, 750);
  window.setExpiryCallback(recordExpiry, &expiry);
  window.startedAt(100);

  window.beforeReaderEnter();
  window.beforeReaderEnter();
  EXPECT_TRUE(window.tick(20000));

  EXPECT_EQ(expiry.calls, 1);
}

TEST(DashboardBleWindow, WrapSafeNearUint32Rollover) {
  ExpirySink expiry;
  DashboardBleWindow window(10000, 750);
  window.setExpiryCallback(recordExpiry, &expiry);

  constexpr uint32_t kStart = 0xFFFFFFF0u;
  window.startedAt(kStart);
  EXPECT_TRUE(window.tick(kStart + 9999u));
  EXPECT_TRUE(window.isActive());
  EXPECT_EQ(expiry.calls, 0);

  EXPECT_FALSE(window.tick(kStart + 10000u));
  EXPECT_FALSE(window.isActive());
  EXPECT_EQ(expiry.calls, 1);
}

TEST(DashboardBleWindow, AcceptedSettleIsWrapSafeNearUint32Rollover) {
  ExpirySink expiry;
  DashboardBleWindow window(10000, 750);
  window.setExpiryCallback(recordExpiry, &expiry);

  constexpr uint32_t kAccepted = 0xFFFFFFF0u;
  window.startedAt(kAccepted - 100u);
  window.acceptedAt(kAccepted);
  EXPECT_TRUE(window.tick(kAccepted + 749u));
  EXPECT_FALSE(window.tick(kAccepted + 750u));
  EXPECT_EQ(expiry.calls, 1);
}

}  // namespace
}  // namespace dashboard_sync
