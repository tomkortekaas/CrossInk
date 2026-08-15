#pragma once

#include <cstdint>

namespace dashboard {

// Pure date arithmetic for the dashboard's date widget. No <ctime>, no time_t,
// no timezone database: the X3's RTC hands us calendar fields directly
// (HalClock::getDateTime) and everything below is a function of those fields.
// Keeping this free of the renderer and the SDK is what makes the edge cases —
// leap years, ISO week-year rollover — testable on the host.

enum class Weekday : uint8_t { Monday = 0, Tuesday, Wednesday, Thursday, Friday, Saturday, Sunday };

// ISO 8601 week. `year` is the ISO week-year, which is not always the calendar
// year: 2019-12-30 is week 1 of 2020, and 2021-01-01 is week 53 of 2020.
struct IsoWeek {
  uint16_t year;
  uint8_t week;
};

Weekday weekdayFromDate(uint16_t year, uint8_t month, uint8_t day);
IsoWeek isoWeekFromDate(uint16_t year, uint8_t month, uint8_t day);
bool isValidDate(uint16_t year, uint8_t month, uint8_t day);

// Dutch names, held in static constexpr tables so they live in flash rather
// than DRAM (CLAUDE.md resource rule 3). Out-of-range input returns "" rather
// than reading past a table: callers draw an empty string, which is visibly
// wrong without being memory-unsafe.
const char* weekdayName(Weekday weekday);
const char* weekdayAbbreviation(Weekday weekday);
const char* monthName(uint8_t month);
const char* monthAbbreviation(uint8_t month);

}  // namespace dashboard
