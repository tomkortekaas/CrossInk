#include <gtest/gtest.h>

#include "DashboardBleWindow.h"

namespace dashboard_sync {
namespace {

class FakeTransport final : public BleWindowTransport {
 public:
  bool end() override {
    ++endCalls;
    return endResult;
  }

  int endCalls = 0;
  bool endResult = true;
};

struct ExpirySink {
  int calls = 0;
};

void recordExpiry(void* context) { static_cast<ExpirySink*>(context)->calls++; }

TEST(DashboardBleWindow, ExpiryRequestsRestartWithoutEndingTransport) {
  FakeTransport transport;
  ExpirySink expiry;
  DashboardBleWindow window(transport, 10000);
  window.setExpiryCallback(recordExpiry, &expiry);

  window.startedAt(100);
  EXPECT_TRUE(window.tick(10099));
  EXPECT_TRUE(window.isActive());
  EXPECT_EQ(expiry.calls, 0);
  EXPECT_EQ(transport.endCalls, 0);

  EXPECT_FALSE(window.tick(10100));
  EXPECT_FALSE(window.isActive());
  EXPECT_EQ(expiry.calls, 1);
  EXPECT_EQ(transport.endCalls, 0);
}

TEST(DashboardBleWindow, StopsBeforeEarlyReaderEntry) {
  FakeTransport transport;
  ExpirySink expiry;
  DashboardBleWindow window(transport, 10000);
  window.setExpiryCallback(recordExpiry, &expiry);

  window.startedAt(100);
  window.beforeReaderEnter();

  EXPECT_FALSE(window.isActive());
  EXPECT_EQ(expiry.calls, 0);
  EXPECT_EQ(transport.endCalls, 1);
}

TEST(DashboardBleWindow, ShutdownIsIdempotent) {
  FakeTransport transport;
  ExpirySink expiry;
  DashboardBleWindow window(transport, 10000);
  window.setExpiryCallback(recordExpiry, &expiry);

  window.startedAt(100);
  window.beforeReaderEnter();
  window.beforeReaderEnter();
  window.tick(20000);

  EXPECT_EQ(expiry.calls, 0);
  EXPECT_EQ(transport.endCalls, 1);
}

TEST(DashboardBleWindow, DisabledWindowDoesNotTouchTransport) {
  FakeTransport transport;
  ExpirySink expiry;
  DashboardBleWindow window(transport, 10000);
  window.setExpiryCallback(recordExpiry, &expiry);

  window.beforeReaderEnter();
  window.tick(20000);

  EXPECT_FALSE(window.isActive());
  EXPECT_EQ(expiry.calls, 0);
  EXPECT_EQ(transport.endCalls, 0);
}

TEST(DashboardBleWindow, FailedDeinitStillDoesNotRetryInMainLoop) {
  FakeTransport transport;
  ExpirySink expiry;
  transport.endResult = false;
  DashboardBleWindow window(transport, 10000);
  window.setExpiryCallback(recordExpiry, &expiry);

  window.startedAt(100);
  EXPECT_FALSE(window.beforeReaderEnter());
  EXPECT_FALSE(window.isActive());
  window.tick(20000);

  EXPECT_EQ(expiry.calls, 0);
  EXPECT_EQ(transport.endCalls, 1);
}

TEST(DashboardBleWindow, WrapSafeNearUint32Rollover) {
  FakeTransport transport;
  ExpirySink expiry;
  DashboardBleWindow window(transport, 10000);
  window.setExpiryCallback(recordExpiry, &expiry);

  constexpr uint32_t kStart = 0xFFFFFFF0u;              // 4,294,967,280
  window.startedAt(kStart);                              // deadline wraps to 9984
  EXPECT_TRUE(window.tick(kStart + 9999u));              // 1 ms before the deadline
  EXPECT_TRUE(window.isActive());
  EXPECT_EQ(expiry.calls, 0);
  EXPECT_EQ(transport.endCalls, 0);

  EXPECT_FALSE(window.tick(kStart + 10000u));
  EXPECT_FALSE(window.isActive());
  EXPECT_EQ(expiry.calls, 1);
  EXPECT_EQ(transport.endCalls, 0);
}

}  // namespace
}  // namespace dashboard_sync
