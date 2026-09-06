// Host tests for the pure X3 navigator view/action controller
// (src/spikes/navigator/NavigationViewController.h/.cpp).
//
// Task 1 - strict TDD. The controller must:
//   * default every session to Overview;
//   * map released Up/Down to ToggleView, OK/Confirm to ManualRefresh and
//     Back to ReturnToDashboard through one centralized pure function, with
//     every other release mapping to None;
//   * return to Overview after two toggles and never persist view state;
//   * select the existing whole-route fit viewport for Overview (byte-
//     compatible with the route-fit path) even when a fix is available;
//   * select a north-up viewport centred on a valid fix whose horizontal
//     ground span is 250 m across the usable map rectangle within 5%;
//   * when GPS zoom is requested without a valid fix, keep the mode but
//     return the Overview fit viewport and expose waitingForGps - it never
//     invents a centre.

#include <gtest/gtest.h>

#include "InputManager.h"
#include "NavigationViewController.h"
#include "map/RouteViewport.h"
#include "route/RoutePackageV1.h"

namespace {

using navigator::CurrentPosition;
using navigator::GeoPoint;
using navigator::NavigationView;
using navigator::NavigationViewController;
using navigator::NavigatorAction;
using navigator::Rect;
using navigator::RouteIndex;
using navigator::RouteViewport;
using navigator::ScreenPoint;

// Local two-point route ~1 km apart in each axis near Amsterdam, encoded in
// the RouteIndex overview exactly as fitOverview reads it (E5 points).
RouteIndex sampleRoute() {
  RouteIndex route;
  route.overviewPointCount = 2;
  route.overview[0] = {523'676'0, 49'041'0};
  route.overview[1] = {523'776'0, 49'141'0};
  return route;
}

// A portrait map rectangle like the X3 overview (usable width < height).
constexpr Rect kMapRect{0, 0, 400, 500};

constexpr GeoPoint kFix{523'700'000, 49'090'000};  // valid E7 position

TEST(NavigationViewControllerTest, DefaultsToOverviewAtSessionStart) {
  NavigationViewController controller;
  EXPECT_EQ(controller.view(), NavigationView::Overview);
  NavigationViewController resetController;
  resetController.toggle();
  resetController.reset();
  EXPECT_EQ(resetController.view(), NavigationView::Overview);
}

TEST(NavigationViewControllerTest, UpAndDownMapToToggleView) {
  EXPECT_EQ(navigator::mapNavigatorButton(InputManager::BTN_UP), NavigatorAction::ToggleView);
  EXPECT_EQ(navigator::mapNavigatorButton(InputManager::BTN_DOWN), NavigatorAction::ToggleView);
}

TEST(NavigationViewControllerTest, ConfirmMapsToManualRefreshAndBackReturns) {
  EXPECT_EQ(navigator::mapNavigatorButton(InputManager::BTN_CONFIRM), NavigatorAction::ManualRefresh);
  EXPECT_EQ(navigator::mapNavigatorButton(InputManager::BTN_BACK), NavigatorAction::ReturnToDashboard);
}

TEST(NavigationViewControllerTest, UnrelatedReleasesMapToNoAction) {
  for (const uint8_t button : {InputManager::BTN_LEFT, InputManager::BTN_RIGHT, InputManager::BTN_POWER,
                               static_cast<uint8_t>(255), static_cast<uint8_t>(7)}) {
    EXPECT_EQ(navigator::mapNavigatorButton(button), NavigatorAction::None)
        << "button index " << static_cast<int>(button) << " must map to None";
  }
}

TEST(NavigationViewControllerTest, TwoTogglesReturnToOverview) {
  NavigationViewController controller;
  controller.toggle();
  EXPECT_EQ(controller.view(), NavigationView::GpsZoom);
  controller.toggle();
  EXPECT_EQ(controller.view(), NavigationView::Overview);
}

TEST(NavigationViewControllerTest, ToggleViewActionNeverChangesSessionModeDirectly) {
  // Toggling is the *controller's* job; the action mapping must stay pure so
  // a later button remap can never accidentally mutate state.
  EXPECT_EQ(navigator::mapNavigatorButton(InputManager::BTN_UP), NavigatorAction::ToggleView);
  EXPECT_EQ(navigator::mapNavigatorButton(InputManager::BTN_DOWN), NavigatorAction::ToggleView);
  EXPECT_EQ(navigator::mapNavigatorButton(InputManager::BTN_CONFIRM), NavigatorAction::ManualRefresh);
}

TEST(NavigationViewControllerTest, OverviewSelectsExistingWholeRouteFit) {
  const RouteIndex route = sampleRoute();
  const RouteViewport reference = RouteViewport::fitOverview(route, kMapRect, navigator::kNavigationViewPaddingPx);
  ASSERT_TRUE(reference.isValid());

  NavigationViewController controller;  // Overview
  const auto selection = controller.selectViewport(route, kMapRect, nullptr);
  EXPECT_FALSE(selection.waitingForGps);
  ASSERT_TRUE(selection.viewport.isValid());
  EXPECT_EQ(selection.viewport.mapRect().x, reference.mapRect().x);
  EXPECT_EQ(selection.viewport.mapRect().y, reference.mapRect().y);
  EXPECT_EQ(selection.viewport.mapRect().width, reference.mapRect().width);
  EXPECT_EQ(selection.viewport.mapRect().height, reference.mapRect().height);
  EXPECT_EQ(selection.viewport.centerLatitudeE7(), reference.centerLatitudeE7());
  EXPECT_EQ(selection.viewport.centerLongitudeE7(), reference.centerLongitudeE7());
  EXPECT_EQ(selection.viewport.paddingPx(), reference.paddingPx());
}

TEST(NavigationViewControllerTest, OverviewKeepsRouteFitEvenWhenFixIsAvailable) {
  // "Overview" is the whole route: a fresh fix never silently recentres the
  // map in this view (GPS zoom is the explicit centred mode).
  const RouteIndex route = sampleRoute();
  const CurrentPosition fix{kFix, 5, 0};
  NavigationViewController controller;  // Overview
  const auto selection = controller.selectViewport(route, kMapRect, &fix);
  EXPECT_FALSE(selection.waitingForGps);
  ASSERT_TRUE(selection.viewport.isValid());
  const RouteViewport reference = RouteViewport::fitOverview(route, kMapRect, navigator::kNavigationViewPaddingPx);
  EXPECT_EQ(selection.viewport.centerLatitudeE7(), reference.centerLatitudeE7());
  EXPECT_EQ(selection.viewport.centerLongitudeE7(), reference.centerLongitudeE7());
  // The fix is not the centre in Overview: it is a marker inside the fitted
  // whole-route view.
  EXPECT_NE(selection.viewport.centerLatitudeE7(), fix.point.latitudeE7);
  EXPECT_NE(selection.viewport.centerLongitudeE7(), fix.point.longitudeE7);
}

TEST(NavigationViewControllerTest, GpsZoomCentresValidFixNorthUp250MetersWithinFivePercent) {
  const RouteIndex route = sampleRoute();
  const CurrentPosition fix{kFix, 5, 0};
  NavigationViewController controller;
  controller.toggle();  // GPS zoom
  ASSERT_EQ(controller.view(), NavigationView::GpsZoom);

  const auto selection = controller.selectViewport(route, kMapRect, &fix);
  EXPECT_FALSE(selection.waitingForGps);
  const RouteViewport& viewport = selection.viewport;
  ASSERT_TRUE(viewport.isValid());

  // The valid fix is the centre of the viewport.
  EXPECT_EQ(viewport.centerLatitudeE7(), fix.point.latitudeE7);
  EXPECT_EQ(viewport.centerLongitudeE7(), fix.point.longitudeE7);
  const ScreenPoint projected = viewport.project(fix.point);
  EXPECT_EQ(projected.x, kMapRect.x + kMapRect.width / 2);
  EXPECT_EQ(projected.y, kMapRect.y + kMapRect.height / 2);

  // 250 m spans the usable horizontal (shorter) inner axis within 5%.
  const int innerW = kMapRect.width - 2 * navigator::kNavigationViewPaddingPx;
  const int spanPx = viewport.pixelsForMeters(navigator::kGpsZoomSpanMeters);
  EXPECT_GE(spanPx, innerW * 19 / 20);
  EXPECT_LE(spanPx, innerW * 21 / 20);

  // North-up: a point ~28 m north of the fix projects to a smaller y and one
  // ~28 m east to a larger x (clear of pixel rounding at this scale).
  const ScreenPoint north = viewport.project(GeoPoint{fix.point.latitudeE7 + 250, fix.point.longitudeE7});
  const ScreenPoint east = viewport.project(GeoPoint{fix.point.latitudeE7, fix.point.longitudeE7 + 250});
  EXPECT_LT(north.y, projected.y);
  EXPECT_GT(east.x, projected.x);
}

TEST(NavigationViewControllerTest, GpsZoomWithoutFixKeepsModeUsesOverviewFitAndFlagsWaiting) {
  const RouteIndex route = sampleRoute();
  NavigationViewController controller;
  controller.toggle();  // GPS zoom requested before any valid fix
  ASSERT_EQ(controller.view(), NavigationView::GpsZoom);

  const auto selection = controller.selectViewport(route, kMapRect, nullptr);
  EXPECT_TRUE(selection.waitingForGps) << "no fix must stay an explicit waiting state";
  EXPECT_EQ(controller.view(), NavigationView::GpsZoom) << "mode stays selected";
  ASSERT_TRUE(selection.viewport.isValid());
  const RouteViewport reference = RouteViewport::fitOverview(route, kMapRect, navigator::kNavigationViewPaddingPx);
  EXPECT_EQ(selection.viewport.centerLatitudeE7(), reference.centerLatitudeE7());
  EXPECT_EQ(selection.viewport.centerLongitudeE7(), reference.centerLongitudeE7());
  // It must never invent a centre: the fallback shows the whole fitted route,
  // not some default GPS point.
  EXPECT_NE(selection.viewport.centerLongitudeE7(), kFix.longitudeE7);
}

}  // namespace
