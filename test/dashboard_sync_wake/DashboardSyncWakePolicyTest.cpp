#include <gtest/gtest.h>

#include "DashboardSyncWakePolicy.h"

namespace dashboard_sync {

TEST(DashboardSyncWakePolicy, TimerWakeUsesDashboardSyncRoute) {
  EXPECT_EQ(selectBootRoute(true, false), BootRoute::DashboardSync);
}

TEST(DashboardSyncWakePolicy, PowerPressedAlwaysSelectsNormalBoot) {
  EXPECT_EQ(selectBootRoute(false, true), BootRoute::NormalBoot);
  EXPECT_EQ(selectBootRoute(true, true), BootRoute::NormalBoot);
}

TEST(DashboardSyncWakePolicy, NoWakeReasonSelectsNormalBoot) {
  EXPECT_EQ(selectBootRoute(false, false), BootRoute::NormalBoot);
}

TEST(DashboardSyncWakePolicy, FullTruthTable) {
  EXPECT_EQ(selectBootRoute(false, false), BootRoute::NormalBoot);
  EXPECT_EQ(selectBootRoute(true, false), BootRoute::DashboardSync);
  EXPECT_EQ(selectBootRoute(false, true), BootRoute::NormalBoot);
  EXPECT_EQ(selectBootRoute(true, true), BootRoute::NormalBoot);
}

}  // namespace dashboard_sync
