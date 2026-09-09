add_executable(BleHandoffRecordTest
  AgendaWakePolicyTest.cpp
  BleHandoffRecordTest.cpp
  DashboardV3Test.cpp
  DashboardV3GoldenTest.cpp
  DashboardV3LayoutTest.cpp
  DashboardV3RendererTest.cpp
  DashboardDateFieldsTest.cpp
  DashboardTransferTest.cpp
  DashboardSlotSelectionTest.cpp
  ReceiverWindowTest.cpp
  DashboardArcTest.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/AgendaWakePolicy.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/BleHandoffRecord.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardV3.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardV3Layout.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/DashboardV3Renderer.cpp
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
