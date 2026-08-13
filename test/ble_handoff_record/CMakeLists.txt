add_executable(BleHandoffRecordTest
  BleHandoffRecordTest.cpp
  DashboardTransferTest.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/BleHandoffRecord.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardTransfer.cpp
)

target_include_directories(BleHandoffRecordTest PRIVATE
  ${REPO_ROOT}/src/spikes/ble_handoff
)

target_link_libraries(BleHandoffRecordTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BleHandoffRecordTest)
