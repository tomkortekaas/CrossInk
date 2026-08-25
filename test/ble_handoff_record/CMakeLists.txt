add_executable(BleHandoffRecordTest
  AgendaWakePolicyTest.cpp
  BleHandoffRecordTest.cpp
  DashboardGridLayoutTest.cpp
  DashboardGridLayoutV2Test.cpp
  DashboardWidgetGridTest.cpp
  DashboardWidgetGridV2Test.cpp
  DashboardV3Test.cpp
  DashboardDateFieldsTest.cpp
  DashboardTransferTest.cpp
  DashboardSlotSelectionTest.cpp
  ReceiverWindowTest.cpp
  DashboardArcTest.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/AgendaWakePolicy.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/BleHandoffRecord.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardGridLayout.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardGridLayoutV2.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardWidgetGrid.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardWidgetGridV2.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardV3.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardTransfer.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardSlotSelection.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/ReceiverWindow.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardDateFields.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardArc.cpp
)

target_include_directories(BleHandoffRecordTest PRIVATE
  ${REPO_ROOT}/src/spikes/ble_handoff
)

target_link_libraries(BleHandoffRecordTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BleHandoffRecordTest)
