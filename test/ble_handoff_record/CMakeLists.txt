add_executable(BleHandoffRecordTest
  AgendaWakePolicyTest.cpp
  BleHandoffRecordTest.cpp
  DashboardGridLayoutTest.cpp
  DashboardWidgetGridTest.cpp
  DashboardTransferTest.cpp
  DashboardSlotSelectionTest.cpp
  ReceiverWindowTest.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/AgendaWakePolicy.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/BleHandoffRecord.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardGridLayout.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardWidgetGrid.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardTransfer.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardSlotSelection.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/ReceiverWindow.cpp
)

target_include_directories(BleHandoffRecordTest PRIVATE
  ${REPO_ROOT}/src/spikes/ble_handoff
)

target_link_libraries(BleHandoffRecordTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BleHandoffRecordTest)
