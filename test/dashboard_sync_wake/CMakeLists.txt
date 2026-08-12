add_executable(DashboardSyncWakePolicyTest
  DashboardSyncWakePolicyTest.cpp
  ${REPO_ROOT}/src/dashboard_sync/DashboardSyncWakePolicy.cpp
)

target_include_directories(DashboardSyncWakePolicyTest PRIVATE
  ${REPO_ROOT}/src/dashboard_sync
)

target_link_libraries(DashboardSyncWakePolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DashboardSyncWakePolicyTest)
