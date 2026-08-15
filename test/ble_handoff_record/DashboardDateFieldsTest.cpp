#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "DashboardDateFields.h"

namespace {

using dashboard::IsoWeek;
using dashboard::Weekday;

TEST(DashboardDateFields, WeekdayMatchesKnownDates) {
  EXPECT_EQ(dashboard::weekdayFromDate(2026, 8, 15), Weekday::Saturday);
  EXPECT_EQ(dashboard::weekdayFromDate(2026, 1, 1), Weekday::Thursday);
  EXPECT_EQ(dashboard::weekdayFromDate(2024, 2, 29), Weekday::Thursday);
  EXPECT_EQ(dashboard::weekdayFromDate(2000, 2, 29), Weekday::Tuesday);
  EXPECT_EQ(dashboard::weekdayFromDate(2100, 3, 1), Weekday::Monday);
}

TEST(DashboardDateFields, IsoWeekHandlesYearBoundaries) {
  // 31 december dat al in week 1 van het volgende jaar valt.
  EXPECT_EQ(dashboard::isoWeekFromDate(2019, 12, 30).week, 1);
  EXPECT_EQ(dashboard::isoWeekFromDate(2019, 12, 30).year, 2020);
  // 1 januari dat nog in de laatste week van het vorige jaar valt.
  EXPECT_EQ(dashboard::isoWeekFromDate(2021, 1, 1).week, 53);
  EXPECT_EQ(dashboard::isoWeekFromDate(2021, 1, 1).year, 2020);
  // Een jaar dat op donderdag begint heeft 1 januari in week 1.
  EXPECT_EQ(dashboard::isoWeekFromDate(2026, 1, 1).week, 1);
  EXPECT_EQ(dashboard::isoWeekFromDate(2026, 1, 1).year, 2026);
  // Het voorbeeld uit de spec.
  EXPECT_EQ(dashboard::isoWeekFromDate(2026, 8, 15).week, 33);
  EXPECT_EQ(dashboard::isoWeekFromDate(2026, 8, 15).year, 2026);
}

TEST(DashboardDateFields, ValidatesLeapYears) {
  EXPECT_TRUE(dashboard::isValidDate(2024, 2, 29));
  EXPECT_FALSE(dashboard::isValidDate(2023, 2, 29));
  EXPECT_TRUE(dashboard::isValidDate(2000, 2, 29));
  EXPECT_FALSE(dashboard::isValidDate(1900, 2, 29));
  EXPECT_FALSE(dashboard::isValidDate(2026, 13, 1));
  EXPECT_FALSE(dashboard::isValidDate(2026, 0, 1));
  EXPECT_FALSE(dashboard::isValidDate(2026, 4, 31));
  EXPECT_FALSE(dashboard::isValidDate(2026, 8, 0));
}

TEST(DashboardDateFields, NamesAreDutch) {
  EXPECT_STREQ(dashboard::weekdayName(Weekday::Monday), "maandag");
  EXPECT_STREQ(dashboard::weekdayName(Weekday::Sunday), "zondag");
  EXPECT_STREQ(dashboard::weekdayAbbreviation(Weekday::Saturday), "za");
  EXPECT_STREQ(dashboard::weekdayAbbreviation(Weekday::Wednesday), "wo");
  EXPECT_STREQ(dashboard::monthName(1), "januari");
  EXPECT_STREQ(dashboard::monthName(12), "december");
  EXPECT_STREQ(dashboard::monthAbbreviation(3), "mrt");
  EXPECT_STREQ(dashboard::monthAbbreviation(8), "aug");
}

TEST(DashboardDateFields, NamesRefuseOutOfRangeMonths) {
  EXPECT_STREQ(dashboard::monthName(0), "");
  EXPECT_STREQ(dashboard::monthName(13), "");
  EXPECT_STREQ(dashboard::monthAbbreviation(0), "");
  EXPECT_STREQ(dashboard::monthAbbreviation(13), "");
}

TEST(DashboardDateFields, NamesRefuseOutOfRangeWeekdays) {
  EXPECT_STREQ(dashboard::weekdayName(static_cast<Weekday>(7)), "");
  EXPECT_STREQ(dashboard::weekdayAbbreviation(static_cast<Weekday>(7)), "");
  EXPECT_STREQ(dashboard::weekdayName(static_cast<Weekday>(255)), "");
  EXPECT_STREQ(dashboard::weekdayAbbreviation(static_cast<Weekday>(255)), "");
}

// Pins the documented convention: weekdayFromDate has no value to spare for
// "bad input", so it returns Monday rather than reading past its table. Callers
// must validate first — isoWeekFromDate does, and so does the renderer.
TEST(DashboardDateFields, WeekdayFallsBackToMondayOnAnInvalidMonth) {
  EXPECT_EQ(dashboard::weekdayFromDate(2026, 0, 15), Weekday::Monday);
  EXPECT_EQ(dashboard::weekdayFromDate(2026, 13, 15), Weekday::Monday);
  // isoWeekFromDate, which does validate, reports an impossible week instead.
  EXPECT_EQ(dashboard::isoWeekFromDate(2026, 13, 15).week, 0);
  EXPECT_EQ(dashboard::isoWeekFromDate(2026, 2, 30).week, 0);
}

}  // namespace
