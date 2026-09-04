add_executable(AgendaWakePolicyManualWindowTest
  ManualReceiverWindowTest.cpp
  HomeReceiveBootTest.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/ReceiverWindow.cpp
  ${REPO_ROOT}/src/spikes/ble_handoff/AgendaWakePolicy.cpp
)

target_include_directories(AgendaWakePolicyManualWindowTest PRIVATE
  ${REPO_ROOT}/src/spikes/ble_handoff
)

target_link_libraries(AgendaWakePolicyManualWindowTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AgendaWakePolicyManualWindowTest)
