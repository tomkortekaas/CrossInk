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
