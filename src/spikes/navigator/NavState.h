#pragma once

#include <cstddef>
#include <cstdint>

// Allocation-free navigation state model for the isolated X3 navigator env.
//
// Plain data only: no heap, no std::string, no dynamic allocation, so a state
// can live on the C3's small task stack and be re-rendered freely. The default
// values reproduce the legacy NavSplash example screen (Left, "180 M" on
// "DUINWEG", "4,2 KM  52 MIN"), so NavSplash::draw can stay a thin wrapper
// over NavScreenRenderer::draw(NavState{}).
namespace navigator {

enum class Maneuver : uint8_t {
  Straight = 0,
  Left,
  Right,
  SlightLeft,
  SlightRight,
  UTurn,
  Arrive,
  kCount,
};

enum class NavStatus : uint8_t {
  Navigating = 0,
  Recalculating,
  OffRoute,
  Arrived,
  kCount,
};

struct NavState {
  // Street/path name buffer including the NUL terminator. Names longer than
  // kStreetCapacity - 1 chars are truncated by setStreet().
  static constexpr uint8_t kStreetCapacity = 32;

  Maneuver maneuver = Maneuver::Left;
  uint16_t nextDistanceMeters = 180;
  uint32_t remainingDistanceMeters = 4200;
  uint16_t remainingMinutes = 52;
  NavStatus status = NavStatus::Navigating;
  char street[kStreetCapacity] = "DUINWEG";

  // Copies a street/path name into the fixed buffer with safe truncation and
  // ASCII normalization: lowercase letters become uppercase, allowed ASCII
  // (A-Z, 0-9, space, '-', '.', ',') passes through, and everything else is
  // replaced by a space so the tiny embedded glyph set never sees a byte it
  // cannot render. Always NUL-terminated; a null input clears the buffer.
  void setStreet(const char* text) {
    if (text == nullptr) {
      street[0] = '\0';
      return;
    }
    size_t i = 0;
    while (text[i] != '\0' && i + 1 < kStreetCapacity) {
      char c = text[i];
      if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
      } else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '.' || c == ',')) {
        c = ' ';
      }
      street[i] = c;
      ++i;
    }
    street[i] = '\0';
  }
};

// The whole model (including the 32-byte street buffer) must stay far below
// one typical task stack frame, and it contains no pointers or vtable.
static_assert(sizeof(NavState) < 96, "state model must stay compact");

}  // namespace navigator
