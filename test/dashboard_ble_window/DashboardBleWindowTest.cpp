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

TEST(DashboardBleWindow, StopsWhenReceiveWindowExpires) {
  FakeTransport transport;
  DashboardBleWindow window(transport, 10000);

  window.startedAt(100);
  window.tick(10099);
  EXPECT_TRUE(window.isActive());
  EXPECT_EQ(transport.endCalls, 0);

  window.tick(10100);
  EXPECT_FALSE(window.isActive());
  EXPECT_EQ(transport.endCalls, 1);
}

TEST(DashboardBleWindow, StopsBeforeEarlyReaderEntry) {
  FakeTransport transport;
  DashboardBleWindow window(transport, 10000);

  window.startedAt(100);
  window.beforeReaderEnter();

  EXPECT_FALSE(window.isActive());
  EXPECT_EQ(transport.endCalls, 1);
}

TEST(DashboardBleWindow, ShutdownIsIdempotent) {
  FakeTransport transport;
  DashboardBleWindow window(transport, 10000);

  window.startedAt(100);
  window.beforeReaderEnter();
  window.beforeReaderEnter();
  window.tick(20000);

  EXPECT_EQ(transport.endCalls, 1);
}

TEST(DashboardBleWindow, DisabledWindowDoesNotTouchTransport) {
  FakeTransport transport;
  DashboardBleWindow window(transport, 10000);

  window.beforeReaderEnter();
  window.tick(20000);

  EXPECT_FALSE(window.isActive());
  EXPECT_EQ(transport.endCalls, 0);
}

TEST(DashboardBleWindow, FailedDeinitStillDoesNotRetryInMainLoop) {
  FakeTransport transport;
  transport.endResult = false;
  DashboardBleWindow window(transport, 10000);

  window.startedAt(100);
  EXPECT_FALSE(window.beforeReaderEnter());
  EXPECT_FALSE(window.isActive());
  window.tick(20000);

  EXPECT_EQ(transport.endCalls, 1);
}

TEST(DashboardBleWindow, WrapSafeNearUint32Rollover) {
  FakeTransport transport;
  DashboardBleWindow window(transport, 10000);

  constexpr uint32_t kStart = 0xFFFFFFF0u;              // 4,294,967,280
  window.startedAt(kStart);                              // deadline wraps to 9984
  window.tick(kStart + 9999u);                           // 1 ms before the deadline
  EXPECT_TRUE(window.isActive());
  EXPECT_EQ(transport.endCalls, 0);

  window.tick(kStart + 10000u);
  EXPECT_FALSE(window.isActive());
  EXPECT_EQ(transport.endCalls, 1);
}

}  // namespace
}  // namespace dashboard_sync
