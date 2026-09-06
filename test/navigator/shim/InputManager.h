#pragma once

#include <cstdint>

// Host-only mirror of the FreeInk SDK InputManager button indices
// (freeink-sdk/libs/hardware/InputManager/include/InputManager.h). The real
// header pulls in Arduino/FreeRTOS and cannot be compiled by the host test
// suite; the navigator's pure input mapping (NavigationViewController.cpp)
// switches on these exact release indices, so the tests compile against the
// same names NavigatorMain passes on the device.
class InputManager {
 public:
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};
