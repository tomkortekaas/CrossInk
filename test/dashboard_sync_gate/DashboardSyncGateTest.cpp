#include <cstdint>
#include <gtest/gtest.h>

#include "DashboardSyncGate.h"

namespace dashboard_sync {

TEST(DashboardSyncGate, StopsAfterSettleWhenConnected) {
  GateState gate(/*windowMs=*/10000, /*settleMs=*/750);
  gate.startedAt(100);
  gate.connectedAt(600);
  EXPECT_FALSE(gate.shouldStop(1349));
  EXPECT_TRUE(gate.shouldStop(1350));
}

TEST(DashboardSyncGate, TimeoutExactlyAtWindowDeadline) {
  GateState gate(10000, 750);
  gate.startedAt(100);
  EXPECT_FALSE(gate.shouldStop(10099));
  EXPECT_TRUE(gate.shouldStop(10100));
}

TEST(DashboardSyncGate, WindowDeadlineWinsOverLateConnection) {
  GateState gate(kGateWindowMs, kConnectedSettleMs);
  gate.startedAt(100);
  gate.connectedAt(10099);  // 1 ms before the window closes at 10100
  EXPECT_FALSE(gate.shouldStop(10099));
  EXPECT_TRUE(gate.shouldStop(10100));  // settle would end at 10849, deadline wins
}

TEST(DashboardSyncGate, RestartResetsConnectionState) {
  GateState gate(kGateWindowMs, kConnectedSettleMs);
  gate.startedAt(100);
  gate.connectedAt(600);
  gate.startedAt(1000);
  EXPECT_FALSE(gate.shouldStop(1749));  // settle from old run no longer applies
  EXPECT_FALSE(gate.shouldStop(1750));
}

TEST(DashboardSyncGate, WrapSafeNearUint32RolloverForWindow) {
  constexpr uint32_t kStart = 0xFFFFFFF0u;                  // 4,294,967,280
  constexpr uint32_t kDeadline = kStart + kGateWindowMs;    // wraps to 9984
  GateState gate(kGateWindowMs, kConnectedSettleMs);
  gate.startedAt(kStart);
  EXPECT_FALSE(gate.shouldStop(kDeadline - 1));
  EXPECT_TRUE(gate.shouldStop(kDeadline));
}

TEST(DashboardSyncGate, WrapSafeNearUint32RolloverForSettle) {
  constexpr uint32_t kStart = 0xFFFFFFF0u;                  // 4,294,967,280
  constexpr uint32_t kConnect = kStart + 510u;              // wraps to 494
  constexpr uint32_t kSettleDeadline = kConnect + kConnectedSettleMs;  // wraps to 1244
  GateState gate(kGateWindowMs, kConnectedSettleMs);
  gate.startedAt(kStart);
  gate.connectedAt(kConnect);
  EXPECT_FALSE(gate.shouldStop(kSettleDeadline - 1));
  EXPECT_TRUE(gate.shouldStop(kSettleDeadline));
}

TEST(DashboardSyncGate, ConnectedOutcomeRequestsTeardown) {
  GateState gate(10000, 750);
  gate.startedAt(100);
  gate.connectedAt(600);
  EXPECT_EQ(gate.outcomeAt(1350), GateOutcome::Connected);
  EXPECT_TRUE(gate.shouldStop(1350));
}

TEST(DashboardSyncGate, TimeoutOutcomeRequestsTeardown) {
  GateState gate(10000, 750);
  gate.startedAt(100);
  EXPECT_EQ(gate.outcomeAt(10100), GateOutcome::Timeout);
  EXPECT_TRUE(gate.shouldStop(10100));
}

}  // namespace dashboard_sync
