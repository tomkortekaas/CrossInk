add_executable(NavigatorCoreTest
  NavHandoffTest.cpp
  NavSplashTest.cpp
  NavStateTest.cpp
  NavScreenRendererTest.cpp
  NavigatorRouteScreenTest.cpp
  RoutePackageV1Test.cpp
  RouteStoreTest.cpp
  StoredRouteTest.cpp
  RouteTransferTest.cpp
  RouteViewportTest.cpp
  RouteMapRendererTest.cpp
  ManeuverBandPlannerTest.cpp
  ${REPO_ROOT}/src/spikes/navigator/ManeuverBandPlanner.cpp
  ${REPO_ROOT}/src/spikes/navigator/route/RouteManeuverSelector.cpp
  ${REPO_ROOT}/src/spikes/navigator/NavHandoff.cpp
  ${REPO_ROOT}/src/spikes/navigator/NavSplash.cpp
  ${REPO_ROOT}/src/spikes/navigator/NavScreenRenderer.cpp
  ${REPO_ROOT}/src/spikes/navigator/route/RoutePackageV1.cpp
  ${REPO_ROOT}/src/spikes/navigator/route/RouteStore.cpp
  ${REPO_ROOT}/src/spikes/navigator/route/StoredRoute.cpp
  ${REPO_ROOT}/src/spikes/navigator/route/RouteTransfer.cpp
  ${REPO_ROOT}/src/spikes/navigator/map/GrayMap.cpp
  ${REPO_ROOT}/src/spikes/navigator/map/WalkMap.cpp
  ${REPO_ROOT}/src/spikes/navigator/map/WalkMapViewport.cpp
  ${REPO_ROOT}/src/spikes/navigator/map/RouteViewport.cpp
  ${REPO_ROOT}/src/spikes/navigator/map/RouteMapRenderer.cpp
)

target_include_directories(NavigatorCoreTest PRIVATE
  ${REPO_ROOT}/src/spikes/navigator
)

target_link_libraries(NavigatorCoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(NavigatorCoreTest)

# Focused view/action controller suite: session view state, pure released-
# button mapping and viewport selection for the X3 navigator (see
# src/spikes/navigator/NavigationViewController.h).
add_executable(NavigationViewControllerTest
  NavigationViewControllerTest.cpp
  ${REPO_ROOT}/src/spikes/navigator/NavigationViewController.cpp
  ${REPO_ROOT}/src/spikes/navigator/map/RouteViewport.cpp
)
target_include_directories(NavigationViewControllerTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/shim
  ${REPO_ROOT}/src/spikes/navigator
)
target_link_libraries(NavigationViewControllerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(NavigationViewControllerTest)

# Pure startup submission controller for the calm navigator entry (header-only;
# see src/spikes/navigator/NavigationStartup.h).
add_executable(NavigationStartupTest
  NavigationStartupTest.cpp
)
target_include_directories(NavigationStartupTest PRIVATE
  ${REPO_ROOT}/src/spikes/navigator
)
target_link_libraries(NavigationStartupTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(NavigationStartupTest)

# The BLE callback can receive several live fixes while e-paper refresh blocks
# the main loop. Keep the newest normal fix without weakening strict handling
# for control-frame collisions or malformed/oversized input.
add_executable(LiveFrameMailboxTest
  LiveFrameMailboxTest.cpp
)
target_include_directories(LiveFrameMailboxTest PRIVATE
  ${REPO_ROOT}/src/spikes/navigator
)
target_link_libraries(LiveFrameMailboxTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(LiveFrameMailboxTest)

add_executable(LiveNavigationSessionTest
  LiveNavigationSessionTest.cpp
  ${REPO_ROOT}/src/spikes/navigator/LiveNavigationSession.cpp
)
target_include_directories(LiveNavigationSessionTest PRIVATE
  ${REPO_ROOT}/src/spikes/navigator
)
target_link_libraries(LiveNavigationSessionTest PRIVATE
  crosspoint_test_common
)
add_test(NAME LiveNavigationSession COMMAND LiveNavigationSessionTest)

# Refresh scheduling for the live walk (fixed cadences, movement throttle,
# manual/forced refresh and immediate viewport-change Full refresh). Plain
# assert-style suite kept as its own executable like the policy unit it tests.
add_executable(NavigationRefreshPolicyTest
  NavigationRefreshPolicyTest.cpp
  ${REPO_ROOT}/src/spikes/navigator/NavigationRefreshPolicy.cpp
  ${REPO_ROOT}/src/spikes/navigator/map/RouteViewport.cpp
)
target_include_directories(NavigationRefreshPolicyTest PRIVATE
  ${REPO_ROOT}/src/spikes/navigator
)
target_link_libraries(NavigationRefreshPolicyTest PRIVATE
  crosspoint_test_common
)
add_test(NAME NavigationRefreshPolicy COMMAND NavigationRefreshPolicyTest)

# Focused Task 7 suite: fixed-state next-maneuver selection from a validated
# RouteIndex. Kept as its own small target so the state machine can be run
# (and linked) in isolation from the renderers, then runs again via ctest.
add_executable(RouteManeuverSelectorTest
  RouteManeuverSelectorTest.cpp
  ${REPO_ROOT}/src/spikes/navigator/route/RouteManeuverSelector.cpp
)
target_include_directories(RouteManeuverSelectorTest PRIVATE
  ${REPO_ROOT}/src/spikes/navigator
)
target_link_libraries(RouteManeuverSelectorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(RouteManeuverSelectorTest)

# Standalone acceptance tests use real assertions (also enabled in Release),
# independent of the converter and without a replacement GoogleTest shim.
set(NAV_SRC ${REPO_ROOT}/src/spikes/navigator)
add_executable(WalkMapAcceptanceTest WalkMapTest.cpp ${NAV_SRC}/map/WalkMap.cpp)
target_include_directories(WalkMapAcceptanceTest PRIVATE ${NAV_SRC}/map)
add_test(NAME WalkMapAcceptance COMMAND WalkMapAcceptanceTest ${REPO_ROOT}/test/fixtures/walkmap-v1.bin)
add_executable(WalkMapViewportTest WalkMapViewportTest.cpp ${NAV_SRC}/map/WalkMapViewport.cpp ${NAV_SRC}/map/RouteViewport.cpp)
target_include_directories(WalkMapViewportTest PRIVATE ${NAV_SRC}/map)
add_test(NAME WalkMapViewport COMMAND WalkMapViewportTest)
add_executable(WalkMapBackgroundTest WalkMapBackgroundTest.cpp
  ${NAV_SRC}/NavScreenRenderer.cpp ${NAV_SRC}/NavSplash.cpp
  ${NAV_SRC}/map/RouteViewport.cpp ${NAV_SRC}/map/RouteMapRenderer.cpp
  ${NAV_SRC}/map/GrayMap.cpp ${NAV_SRC}/map/WalkMap.cpp ${NAV_SRC}/map/WalkMapViewport.cpp
  ${NAV_SRC}/route/RoutePackageV1.cpp)
target_include_directories(WalkMapBackgroundTest PRIVATE ${NAV_SRC})
add_test(NAME WalkMapBackground COMMAND WalkMapBackgroundTest ${REPO_ROOT}/test/fixtures/walkmap-route.bin)

add_executable(WalkMapSdSourceTest
  sd_map_adapter/WalkMapSdSourceTest.cpp
  ${REPO_ROOT}/src/spikes/navigator/map/WalkMapSdSource.cpp
)
target_compile_definitions(WalkMapSdSourceTest PRIVATE ARDUINO=1)
target_include_directories(WalkMapSdSourceTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/sd_map_adapter/shim
  ${REPO_ROOT}/src/spikes/navigator/map
)
add_test(NAME WalkMapSdSourceTest COMMAND WalkMapSdSourceTest)

add_executable(WalkMapDetailTest WalkMapDetailTest.cpp ${NAV_SRC}/map/WalkMap.cpp)
target_include_directories(WalkMapDetailTest PRIVATE ${NAV_SRC}/map)
add_test(NAME WalkMapDetail COMMAND WalkMapDetailTest)

add_executable(GrayMapTest GrayMapTest.cpp ${NAV_SRC}/map/GrayMap.cpp
  ${NAV_SRC}/map/RouteViewport.cpp ${NAV_SRC}/map/WalkMapViewport.cpp)
target_include_directories(GrayMapTest PRIVATE ${NAV_SRC})
add_test(NAME GrayMap COMMAND GrayMapTest)
