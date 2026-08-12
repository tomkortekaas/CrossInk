add_executable(DashboardBleWindowTest
  DashboardBleWindowTest.cpp
  ${REPO_ROOT}/src/dashboard_sync/DashboardBleWindow.cpp
)

target_include_directories(DashboardBleWindowTest PRIVATE
  ${REPO_ROOT}/src/dashboard_sync
)

target_link_libraries(DashboardBleWindowTest PRIVATE
  GTest::gtest_main
  crosspoint_test_common
)

gtest_discover_tests(DashboardBleWindowTest)
