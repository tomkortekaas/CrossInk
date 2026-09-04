#include <gtest/gtest.h>

#include "NavHandoff.h"

namespace {

TEST(NavHandoffTest, MarkerStartsNavigatorWithoutAnotherCondition) {
  EXPECT_EQ(navigator::chooseNavBootRoute(true, false), navigator::NavLaunchDecision::RunNavigator);
}

TEST(NavHandoffTest, MissingMarkerReturnsToReaderDashboard) {
  EXPECT_EQ(navigator::chooseNavBootRoute(false, false), navigator::NavLaunchDecision::ReturnToReaderDashboard);
}

TEST(NavHandoffTest, DevelopmentOverrideCanStartWithoutMarker) {
  EXPECT_EQ(navigator::chooseNavBootRoute(false, true), navigator::NavLaunchDecision::RunNavigator);
}

TEST(NavHandoffTest, DisabledOverrideCannotStartWithoutMarker) {
  constexpr bool productionOverride = false;
  EXPECT_EQ(navigator::chooseNavBootRoute(false, productionOverride),
            navigator::NavLaunchDecision::ReturnToReaderDashboard);
}

TEST(NavHandoffTest, BackReleaseRequestsSafeReturnToDashboard) {
  EXPECT_EQ(navigator::chooseNavInputAction(true), navigator::NavInputAction::ReturnToDashboard);
}

TEST(NavHandoffTest, IdleInputKeepsNavigatorRunning) {
  EXPECT_EQ(navigator::chooseNavInputAction(false), navigator::NavInputAction::StayInNavigator);
}

}  // namespace
