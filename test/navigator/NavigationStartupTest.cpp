// Host tests for the pure navigator startup submission controller
// (src/spikes/navigator/NavigationStartup.h).
//
// Task 3 - strict TDD. The calm X3 navigator entry must submit exactly two
// navigator-owned frames: a quiet route-layout loading frame (compact status
// in the final layout, full-quality refresh) followed by one completed route
// frame after the fix/background resources are open. The pure controller
// must:
//   * emit LoadingLayout for the first routeLayoutReady() and CompletedLayout
//     for the first resourcesReady() after it;
//   * never admit an intermediate loading page (a second routeLayoutReady()
//     is None) and never admit any startup submission after completion;
//   * require the loading layout before a completed frame;
//   * keep explicit terminal failures (no route, no SD card, unreadable map,
//     unavailable Bluetooth) as displayable decisions that never run through
//     the calm frames.

#include <gtest/gtest.h>

#include "NavigationStartup.h"

namespace {

using navigator::NavigationStartup;
using navigator::NavigationStartupDecision;

TEST(NavigationStartupTest, NormalEntryEmitsExactlyLoadingThenCompleted) {
  NavigationStartup startup;
  EXPECT_FALSE(startup.failed());
  EXPECT_FALSE(startup.completed());
  EXPECT_EQ(startup.routeLayoutReady(), NavigationStartupDecision::LoadingLayout);
  EXPECT_FALSE(startup.completed());
  EXPECT_EQ(startup.resourcesReady(), NavigationStartupDecision::CompletedLayout);
  EXPECT_FALSE(startup.failed());
  EXPECT_TRUE(startup.completed());
}

TEST(NavigationStartupTest, NoStartupSubmissionAfterTheCompletedFrame) {
  NavigationStartup startup;
  startup.routeLayoutReady();
  startup.resourcesReady();
  // Later frames are ordinary navigation refreshes governed by the refresh
  // policy, never startup submissions: neither a loading page nor another
  // completed-layout decision may appear again.
  EXPECT_EQ(startup.routeLayoutReady(), NavigationStartupDecision::None);
  EXPECT_EQ(startup.resourcesReady(), NavigationStartupDecision::None);
}

TEST(NavigationStartupTest, LoadingLayoutIsAdmittedOnlyOnce) {
  NavigationStartup startup;
  EXPECT_EQ(startup.routeLayoutReady(), NavigationStartupDecision::LoadingLayout);
  // A repeated route-layout event (e.g. an intermediate "still loading" page)
  // must never produce a second submission.
  EXPECT_EQ(startup.routeLayoutReady(), NavigationStartupDecision::None);
}

TEST(NavigationStartupTest, CompletedLayoutRequiresTheLoadingFrameFirst) {
  NavigationStartup startup;
  // Resources becoming ready before the route layout is not a calm entry and
  // must not smuggle a completed frame in.
  EXPECT_EQ(startup.resourcesReady(), NavigationStartupDecision::None);
}

TEST(NavigationStartupTest, TerminalFailureStaysExplicitAndBlocksCalmFrames) {
  NavigationStartup startup;
  EXPECT_EQ(startup.fail(), NavigationStartupDecision::TerminalFailure);
  EXPECT_TRUE(startup.failed());
  EXPECT_EQ(startup.routeLayoutReady(), NavigationStartupDecision::None);
  EXPECT_EQ(startup.resourcesReady(), NavigationStartupDecision::None);
}

TEST(NavigationStartupTest, TerminalFailureAfterTheLoadingFrameBlocksTheCompletedFrame) {
  NavigationStartup startup;
  EXPECT_EQ(startup.routeLayoutReady(), NavigationStartupDecision::LoadingLayout);
  // An unreadable map / missing BLE reported after the loading frame is still
  // an explicit full-screen failure; no calm completed frame follows.
  EXPECT_EQ(startup.fail(), NavigationStartupDecision::TerminalFailure);
  EXPECT_EQ(startup.resourcesReady(), NavigationStartupDecision::None);
}

TEST(NavigationStartupTest, ResetBeginsANewCalmEntry) {
  NavigationStartup startup;
  startup.routeLayoutReady();
  startup.resourcesReady();
  startup.reset();
  EXPECT_FALSE(startup.completed());
  EXPECT_EQ(startup.routeLayoutReady(), NavigationStartupDecision::LoadingLayout);
  EXPECT_EQ(startup.resourcesReady(), NavigationStartupDecision::CompletedLayout);
}

}  // namespace
