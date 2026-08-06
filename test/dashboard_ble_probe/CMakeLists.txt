add_executable(DashboardBleProtocolTest
  DashboardBleProtocolTest.cpp
  ${REPO_ROOT}/src/dashboard_ble_probe/DashboardBleProtocol.cpp
)

target_include_directories(DashboardBleProtocolTest PRIVATE
  ${REPO_ROOT}/src/dashboard_ble_probe
)

target_link_libraries(DashboardBleProtocolTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DashboardBleProtocolTest)
