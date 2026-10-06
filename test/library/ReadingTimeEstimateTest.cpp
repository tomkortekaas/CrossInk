#include <cassert>
#include <cstring>

#include "activities/reader/ReadingTimeEstimate.h"

int main() {
  uint32_t s = 0;
  assert(ReadingTimeEstimate::secondsLeft(500, 0, 10.0f, s) && s == 500);    // stored estimate wins
  assert(ReadingTimeEstimate::secondsLeft(0, 3600, 50.0f, s) && s == 3600);  // extrapolated
  assert(!ReadingTimeEstimate::secondsLeft(0, 100, 50.0f, s));               // < 2 minutes read
  assert(!ReadingTimeEstimate::secondsLeft(0, 3600, 0.0f, s));
  assert(!ReadingTimeEstimate::secondsLeft(0, 3600, 100.0f, s));
  assert(!ReadingTimeEstimate::secondsLeft(0, 3600, -1.0f, s));  // unknown progress

  char buf[24];
  ReadingTimeEstimate::formatCompact(30, "<1 min", buf, sizeof(buf));
  assert(std::strcmp(buf, "<1 min") == 0);
  ReadingTimeEstimate::formatCompact(89, "<1 min", buf, sizeof(buf));
  assert(std::strcmp(buf, "1 min") == 0);
  ReadingTimeEstimate::formatCompact(2700, "<1 min", buf, sizeof(buf));
  assert(std::strcmp(buf, "45 min") == 0);
  ReadingTimeEstimate::formatCompact(10800, "<1 min", buf, sizeof(buf));
  assert(std::strcmp(buf, "3h") == 0);
  ReadingTimeEstimate::formatCompact(13200, "<1 min", buf, sizeof(buf));
  assert(std::strcmp(buf, "3h 40m") == 0);
  return 0;
}
