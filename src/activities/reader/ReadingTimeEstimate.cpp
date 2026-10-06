#include "ReadingTimeEstimate.h"

#include <cstdio>

namespace ReadingTimeEstimate {
bool secondsLeft(const uint32_t storedEstimateSeconds, const uint32_t totalReadingSeconds,
                 const float progressPercent, uint32_t& seconds) {
  if (storedEstimateSeconds > 0) {
    seconds = storedEstimateSeconds;
    return true;
  }
  seconds = 0;
  if (progressPercent <= 0.0f || progressPercent >= 100.0f || totalReadingSeconds < 120) {
    return false;
  }
  const float progress = progressPercent / 100.0f;
  const float estimate = (static_cast<float>(totalReadingSeconds) * (1.0f - progress)) / progress;
  if (estimate <= 0.0f) {
    return false;
  }
  seconds = static_cast<uint32_t>(estimate + 0.5f);
  return seconds > 0;
}

void formatCompact(const uint32_t seconds, const char* lessThanMinuteLabel, char* buf, const size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", lessThanMinuteLabel);
    return;
  }
  const uint32_t minutes = (seconds + 30u) / 60u;
  if (minutes < 60) {
    snprintf(buf, len, "%lu min", static_cast<unsigned long>(minutes));
    return;
  }
  const uint32_t hours = minutes / 60u;
  const uint32_t remainder = minutes % 60u;
  if (remainder == 0) {
    snprintf(buf, len, "%luh", static_cast<unsigned long>(hours));
  } else {
    snprintf(buf, len, "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(remainder));
  }
}
}  // namespace ReadingTimeEstimate
