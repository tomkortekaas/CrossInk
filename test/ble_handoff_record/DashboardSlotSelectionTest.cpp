#include <gtest/gtest.h>

#include "DashboardSlotSelection.h"

TEST(DashboardSlotSelection, EmptyStoreSelectsNothingAndWritesSlotZero) {
  const auto decision = dashboard::chooseSlots({}, {}, -1);
  EXPECT_EQ(decision.selected, -1);
  EXPECT_EQ(decision.writeTarget, 0);
  EXPECT_FALSE(decision.selectionNeedsRepair);
}

TEST(DashboardSlotSelection, SelectsHighestValidIdAndWritesOtherSlot) {
  const auto decision = dashboard::chooseSlots({true, 7}, {true, 9}, 1);
  EXPECT_EQ(decision.selected, 1);
  EXPECT_EQ(decision.writeTarget, 0);
  EXPECT_FALSE(decision.selectionNeedsRepair);
}

TEST(DashboardSlotSelection, FallsBackFromCorruptRecordedSlot) {
  const auto decision = dashboard::chooseSlots({true, 7}, {false, 99}, 1);
  EXPECT_EQ(decision.selected, 0);
  EXPECT_EQ(decision.writeTarget, 1);
  EXPECT_TRUE(decision.selectionNeedsRepair);
}

TEST(DashboardSlotSelection, RepairsMissingOrStaleSelectionUsingNewestValidSlot) {
  auto decision = dashboard::chooseSlots({true, 11}, {true, 10}, -1);
  EXPECT_EQ(decision.selected, 0);
  EXPECT_TRUE(decision.selectionNeedsRepair);
  decision = dashboard::chooseSlots({true, 11}, {true, 10}, 1);
  EXPECT_EQ(decision.selected, 0);
  EXPECT_TRUE(decision.selectionNeedsRepair);
}

TEST(DashboardSlotSelection, RejectsDuplicateAndOlderCandidate) {
  EXPECT_TRUE(dashboard::isNewerPackage({}, {}, 1));
  EXPECT_TRUE(dashboard::isNewerPackage({true, 7}, {false, 99}, 8));
  EXPECT_FALSE(dashboard::isNewerPackage({true, 7}, {true, 9}, 9));
  EXPECT_FALSE(dashboard::isNewerPackage({true, 7}, {true, 9}, 8));
}
