#include "DashboardDateFields.h"

namespace dashboard {
namespace {

constexpr uint8_t kJanuary = 1u;
constexpr uint8_t kFebruary = 2u;
constexpr uint8_t kMarch = 3u;
constexpr uint8_t kMonthMin = 1u;
constexpr uint8_t kMonthMax = 12u;

constexpr bool isLeapYear(uint16_t year) { return (year % 4u == 0u && year % 100u != 0u) || (year % 400u == 0u); }

// Callers must range-check `month` first; isValidDate() is the only caller and
// does exactly that.
uint8_t daysInMonth(uint16_t year, uint8_t month) {
  static constexpr uint8_t lengths[12] = {31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u};
  uint8_t length = lengths[month - kMonthMin];
  if (month == kFebruary && isLeapYear(year)) {
    length = static_cast<uint8_t>(29u);
  }
  return length;
}

uint16_t dayOfYear(uint16_t year, uint8_t month, uint8_t day) {
  static constexpr uint16_t daysBefore[12] = {0u, 31u, 59u, 90u, 120u, 151u, 181u, 212u, 243u, 273u, 304u, 334u};
  uint16_t result = daysBefore[month - kMonthMin];
  result = static_cast<uint16_t>(result + day);
  if (month > kFebruary && isLeapYear(year)) {
    ++result;
  }
  return result;
}

// A year has 53 ISO weeks when it starts on a Thursday, or on a Wednesday in a
// leap year — those are the only two ways a 53rd Thursday fits.
uint8_t isoWeeksInYear(uint16_t year) {
  const Weekday jan1 = weekdayFromDate(year, kJanuary, static_cast<uint8_t>(1u));
  if (jan1 == Weekday::Thursday) return static_cast<uint8_t>(53u);
  if (jan1 == Weekday::Wednesday && isLeapYear(year)) return static_cast<uint8_t>(53u);
  return static_cast<uint8_t>(52u);
}

}  // namespace

Weekday weekdayFromDate(uint16_t year, uint8_t month, uint8_t day) {
  // Sakamoto's algorithm. Input is normally a valid date; an invalid month is
  // mapped to a defined value so the lookup table is never read out of bounds.
  static constexpr int table[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};

  if (month < kMonthMin || month > kMonthMax) {
    return Weekday::Monday;
  }

  int adjustedYear = static_cast<int>(year);
  const int monthNumber = static_cast<int>(month);
  const int dayNumber = static_cast<int>(day);

  if (monthNumber < static_cast<int>(kMarch)) {
    --adjustedYear;
  }

  const int zeroBasedSunday =
      (adjustedYear + adjustedYear / 4 - adjustedYear / 100 + adjustedYear / 400 + table[monthNumber - 1] + dayNumber) %
      7;

  // Sakamoto returns 0=Sunday..6=Saturday. The dashboard enum starts at Monday,
  // so shift the zero point.
  const int zeroBasedMonday = (zeroBasedSunday + 6) % 7;
  return static_cast<Weekday>(zeroBasedMonday);
}

IsoWeek isoWeekFromDate(uint16_t year, uint8_t month, uint8_t day) {
  if (!isValidDate(year, month, day)) {
    return IsoWeek{0u, 0u};
  }

  const Weekday weekday = weekdayFromDate(year, month, day);
  const uint16_t dayNumber = dayOfYear(year, month, day);

  // ISO weekdays are Monday=1..Sunday=7.
  const int isoWeekday = static_cast<int>(weekday) + 1;

  int week = (static_cast<int>(dayNumber) - isoWeekday + 10) / 7;
  uint16_t isoYear = year;

  // Week 0 means the date belongs to the last week of the previous year; a week
  // past this year's count means it is already week 1 of the next.
  if (week < 1) {
    isoYear = static_cast<uint16_t>(year - 1u);
    week = static_cast<int>(isoWeeksInYear(isoYear));
  } else if (week > static_cast<int>(isoWeeksInYear(year))) {
    isoYear = static_cast<uint16_t>(year + 1u);
    week = 1;
  }

  return IsoWeek{isoYear, static_cast<uint8_t>(week)};
}

bool isValidDate(uint16_t year, uint8_t month, uint8_t day) {
  if (year < 1u) return false;
  if (month < kMonthMin || month > kMonthMax) return false;
  if (day < 1u) return false;
  return day <= daysInMonth(year, month);
}

const char* weekdayName(Weekday weekday) {
  static constexpr const char* const names[7] = {"maandag", "dinsdag",  "woensdag", "donderdag",
                                                 "vrijdag", "zaterdag", "zondag"};
  const uint8_t index = static_cast<uint8_t>(weekday);
  if (index >= 7u) return "";
  return names[index];
}

const char* weekdayAbbreviation(Weekday weekday) {
  static constexpr const char* const names[7] = {"ma", "di", "wo", "do", "vr", "za", "zo"};
  const uint8_t index = static_cast<uint8_t>(weekday);
  if (index >= 7u) return "";
  return names[index];
}

const char* monthName(uint8_t month) {
  static constexpr const char* const names[12] = {"januari",   "februari", "maart",    "april",
                                                  "mei",       "juni",     "juli",     "augustus",
                                                  "september", "oktober",  "november", "december"};
  if (month < kMonthMin || month > kMonthMax) return "";
  return names[month - kMonthMin];
}

const char* monthAbbreviation(uint8_t month) {
  static constexpr const char* const names[12] = {"jan", "feb", "mrt", "apr", "mei", "jun",
                                                  "jul", "aug", "sep", "okt", "nov", "dec"};
  if (month < kMonthMin || month > kMonthMax) return "";
  return names[month - kMonthMin];
}

}  // namespace dashboard
