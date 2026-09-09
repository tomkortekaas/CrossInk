// Wiring tests for the fixed instruction band above the overview map.
//
// RouteManeuverSelector's own semantics (monotonic advance, 12 m hysteresis,
// terminal arrival) are covered by RouteManeuverSelectorTest and are not
// repeated here. What is tested here is the part that was missing on hardware:
// that a band is planned at all, and that its countdown is derived from the
// same trust decision the footer uses.
#include "ManeuverBandPlanner.h"

#include <gtest/gtest.h>

#include "NavScreenRenderer.h"
#include "map/RouteMapRenderer.h"
#include "map/RouteViewport.h"

using namespace navigator;

namespace {

constexpr uint32_t kTotalMeters = 1000;

// A validated index is a plain struct, so the maneuver list can be filled
// directly; decoding a package would only add noise to a wiring test.
RouteIndex routeWithManeuvers() {
  RouteIndex route;
  route.routeId = 0x1234;
  route.totalDistanceMeters = kTotalMeters;
  route.estimatedMinutes = 12;
  route.maneuverCount = 2;
  route.maneuvers[0].type = 2;  // Right
  route.maneuvers[0].distanceFromStartMeters = 300;
  route.maneuvers[1].type = 6;  // Arrive
  route.maneuvers[1].distanceFromStartMeters = 800;
  return route;
}

// A fix the footer trusts: on the route (well inside max(40 m, 2x accuracy))
// and carrying phone-tracked progress.
CurrentPosition trustedFixAt(uint32_t distanceFromStartMeters) {
  CurrentPosition position;
  position.accuracyMeters = 10;
  position.hasRouteProgress = true;
  position.distanceFromStartMeters = distanceFromStartMeters;
  return position;
}

RouteProximity onRoute() {
  RouteProximity proximity;
  proximity.valid = true;
  proximity.distanceMeters = 5;
  return proximity;
}

}  // namespace

TEST(ManeuverBandPlannerTest, ARouteWithoutManeuversGetsNoBand) {
  RouteIndex route;
  route.routeId = 7;
  route.totalDistanceMeters = kTotalMeters;
  route.maneuverCount = 0;

  ManeuverBandPlanner planner;
  EXPECT_FALSE(ManeuverBandPlanner::reservesBand(route));
  const auto plan = planner.plan(route, nullptr, RouteProximity{});
  EXPECT_FALSE(plan.visible);
}

// The band must exist before the first fix arrives, or the map rectangle
// would change height the moment GPS appears and the frame would jump.
TEST(ManeuverBandPlannerTest, TheBandIsReservedWithoutAFixAndCountsFromTheRouteStart) {
  const RouteIndex route = routeWithManeuvers();
  ManeuverBandPlanner planner;
  EXPECT_TRUE(ManeuverBandPlanner::reservesBand(route));

  const auto plan = planner.plan(route, nullptr, RouteProximity{});
  EXPECT_TRUE(plan.visible);
  EXPECT_FALSE(plan.progressTrusted);
  EXPECT_EQ(plan.maneuver, Maneuver::Right);
  EXPECT_EQ(plan.distanceMeters, 300);
}

TEST(ManeuverBandPlannerTest, ATrustedFixAdvancesPastAPassedManeuver) {
  const RouteIndex route = routeWithManeuvers();
  ManeuverBandPlanner planner;

  const CurrentPosition before = trustedFixAt(280);
  const auto approaching = planner.plan(route, &before, onRoute());
  EXPECT_TRUE(approaching.progressTrusted);
  EXPECT_EQ(approaching.maneuver, Maneuver::Right);
  EXPECT_EQ(approaching.distanceMeters, 20);

  // 320 m is more than the fixed 12 m hysteresis past the 300 m turn.
  const CurrentPosition after = trustedFixAt(320);
  const auto passed = planner.plan(route, &after, onRoute());
  EXPECT_EQ(passed.maneuver, Maneuver::Arrive);
  EXPECT_EQ(passed.distanceMeters, 480);
}

// The off-route guard is the whole reason the planner defers to
// chooseFooterMetrics: a fix too far from the route may not move the
// selection, however confident its own progress number is.
TEST(ManeuverBandPlannerTest, AnOffRouteFixNeverAdvancesTheSelection) {
  const RouteIndex route = routeWithManeuvers();
  ManeuverBandPlanner planner;

  CurrentPosition wandered = trustedFixAt(900);  // past both maneuvers
  RouteProximity farAway = onRoute();
  farAway.distanceMeters = 41;  // just outside max(40 m, 2 x 10 m)

  const auto plan = planner.plan(route, &wandered, farAway);
  EXPECT_TRUE(plan.visible);
  EXPECT_FALSE(plan.progressTrusted) << "41 m is outside the footer's off-route guard";
  EXPECT_EQ(plan.maneuver, Maneuver::Right) << "an off-route fix must not skip the turn";
  EXPECT_EQ(plan.distanceMeters, 300);

  // One metre closer and the same fix is inside the guard again.
  farAway.distanceMeters = 40;
  const auto trusted = planner.plan(route, &wandered, farAway);
  EXPECT_TRUE(trusted.progressTrusted);
  EXPECT_EQ(trusted.maneuver, Maneuver::Arrive);
}

// The property that makes the reuse worth it: the band counts down against
// exactly the distance the footer reports as walked, so the two rows of the
// same frame can never contradict each other.
TEST(ManeuverBandPlannerTest, TheCountdownAgreesWithTheFooterRemainingDistance) {
  const RouteIndex route = routeWithManeuvers();
  ManeuverBandPlanner planner;
  const CurrentPosition fix = trustedFixAt(250);
  const RouteProximity proximity = onRoute();

  const auto plan = planner.plan(route, &fix, proximity);
  const NavFooterMetrics metrics = NavScreenRenderer::chooseFooterMetrics(&fix, proximity, route);
  ASSERT_EQ(metrics.mode, NavMetricMode::Remaining);

  const uint32_t walkedPerFooter = route.totalDistanceMeters - metrics.distanceMeters;
  EXPECT_EQ(walkedPerFooter + plan.distanceMeters, route.maneuvers[0].distanceFromStartMeters);
}

TEST(ManeuverBandPlannerTest, ResetReturnsTheSelectionToTheFirstManeuver) {
  const RouteIndex route = routeWithManeuvers();
  ManeuverBandPlanner planner;

  const CurrentPosition fix = trustedFixAt(320);
  ASSERT_EQ(planner.plan(route, &fix, onRoute()).maneuver, Maneuver::Arrive);

  planner.reset();
  const auto plan = planner.plan(route, nullptr, RouteProximity{});
  EXPECT_EQ(plan.maneuver, Maneuver::Right);
  EXPECT_EQ(plan.distanceMeters, 300);
}
