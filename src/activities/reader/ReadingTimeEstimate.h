#pragma once

#include <cstddef>
#include <cstdint>

// Shared "time left in this book" estimate, used by the Dashboard theme and the Library.
// Pure: takes the BookReadingStats fields it needs so it is host-testable.
namespace ReadingTimeEstimate {
// storedEstimateSeconds is BookReadingStats::estimatedTimeLeftSeconds (0 = unknown). Without it,
// extrapolates totalReadingSeconds over the remaining progress once at least 2 minutes were read.
bool secondsLeft(uint32_t storedEstimateSeconds, uint32_t totalReadingSeconds, float progressPercent,
                 uint32_t& seconds);
// "45 min", "3h", "3h 40m"; lessThanMinuteLabel below one minute.
void formatCompact(uint32_t seconds, const char* lessThanMinuteLabel, char* buf, size_t len);
}  // namespace ReadingTimeEstimate
