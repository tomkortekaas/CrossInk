add_executable(DashboardSyncGateTest
  DashboardSyncGateTest.cpp
  ${REPO_ROOT}/src/dashboard_sync/DashboardSyncGate.cpp
)

target_include_directories(DashboardSyncGateTest PRIVATE
  ${REPO_ROOT}/src/dashboard_sync
)

target_link_libraries(DashboardSyncGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DashboardSyncGateTest)
