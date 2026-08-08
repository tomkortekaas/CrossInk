#pragma once

#include <cstdint>

class VisibleTextPageLocator {
  const uint32_t targetOffset_;
  const bool preferFirstAtOffset_;
  uint16_t result_ = 0;

 public:
  VisibleTextPageLocator(const uint32_t targetOffset, const bool preferFirstAtOffset)
      : targetOffset_(targetOffset), preferFirstAtOffset_(preferFirstAtOffset) {}

  bool accept(const uint16_t page, const uint32_t pageStart) {
    if (pageStart > targetOffset_) return false;
    result_ = page;
    return !preferFirstAtOffset_ || pageStart != targetOffset_;
  }

  uint16_t result() const { return result_; }
};
