#include <gtest/gtest.h>

#include "VisibleTextPageLocator.h"

TEST(VisibleTextPageLocator, ReopenChoosesFirstPageSharingSavedOffset) {
  VisibleTextPageLocator locator(12, true);

  EXPECT_TRUE(locator.accept(0, 0));
  EXPECT_FALSE(locator.accept(1, 12));
  EXPECT_EQ(locator.result(), 1);
}

TEST(VisibleTextPageLocator, NormalLookupChoosesLastPageNotAfterOffset) {
  VisibleTextPageLocator locator(12, false);

  EXPECT_TRUE(locator.accept(0, 0));
  EXPECT_TRUE(locator.accept(1, 12));
  EXPECT_TRUE(locator.accept(2, 12));
  EXPECT_FALSE(locator.accept(3, 18));
  EXPECT_EQ(locator.result(), 2);
}
