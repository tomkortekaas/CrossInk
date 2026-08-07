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

add_executable(DashboardBleProbeTest
  DashboardBleProbeTest.cpp
  ${REPO_ROOT}/src/dashboard_ble_probe/DashboardBleProbe.cpp
  ${REPO_ROOT}/src/dashboard_ble_probe/DashboardBleProtocol.cpp
)

target_include_directories(DashboardBleProbeTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/dashboard_ble_probe
)

target_link_libraries(DashboardBleProbeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DashboardBleProbeTest)
