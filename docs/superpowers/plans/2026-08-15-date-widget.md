# Datumwidget — implementatieplan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Een derde widgettype dat de datum uit de RTC van de X3 leest en zichzelf tekent, met een keuze welk datumveld de tegel toont en een indeling die zich richt naar de tegelmaat.

**Architecture:** `WidgetType::Date = 3` draagt één payloadbyte (`field`) en geen tekst; de firmware haalt de datum uit `halClock`, kent de Nederlandse namen zelf en berekent weekdag en ISO-weeknummer met pure rekenkunde. De iPhone-app kiest alleen wélk veld getoond wordt. Het wire-formaat moet byte-identiek zijn tussen `DashboardWidgetGrid.cpp` (C++) en `WidgetGridPackage.swift` (Swift); dat wordt met een draaiende harness bewezen, niet aangenomen.

**Tech Stack:** C++17/20 (ESP32-C3, gtest via CMake), Swift 6 (SwiftPM + XCTest), SwiftUI.

**Spec:** `docs/superpowers/specs/2026-08-15-date-widget-design.md`

---

## Repo's en commando's

Twee repo's, allebei nodig:

- **Firmware:** `/Volumes/2TB/Development/Projects/CrossInk`, branch `feat/ble-handoff`
- **iPhone-app:** `/Volumes/2TB/Development/Projects/xteink-x3-dashboard-ios`, branch `feat/native-ios-app`

Firmware host-tests (61 slagen op HEAD vóór dit werk):

```bash
export PATH="$HOME/.platformio/packages/tool-cmake/bin:$PATH"
cmake -S test -B /tmp/crossink-test-build -DCMAKE_BUILD_TYPE=Release   # eenmalig
cmake --build /tmp/crossink-test-build --target BleHandoffRecordTest -j8
/tmp/crossink-test-build/ble_handoff_record/BleHandoffRecordTest
```

Firmware compileren voor het echte toestel:

```bash
pio run -e spike-ble-reader-x3
```

iOS-tests (212 slagen op HEAD vóór dit werk). `swift test` dekt het app-target níét, dus altijd allebei:

```bash
swift test
xcodebuild -project X3DashboardApp.xcodeproj -scheme X3DashboardApp \
  -destination 'generic/platform=iOS' build CODE_SIGNING_ALLOWED=NO
```

C++ formatteren na elke taak die C++ raakt:

```bash
find src test -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

## Bestandsindeling

| Bestand | Verantwoordelijkheid |
|---|---|
| `src/spikes/ble_handoff/DashboardDateFields.h/.cpp` (nieuw) | Pure datumrekenkunde en NL-naamtabellen. Geen renderer, geen SDK, geen klok. |
| `src/spikes/ble_handoff/DashboardWidgetGrid.h/.cpp` | `WidgetType::Date`, `DateField`, validatie, lengte, schrijven en lezen. |
| `src/spikes/ble_handoff/DashboardGridRenderer.cpp` | `renderDateWidget` en de indelingsladder. |
| `test/ble_handoff_record/DashboardDateFieldsTest.cpp` (nieuw) | Tests voor de rekenkunde. |
| `test/ble_handoff_record/DashboardWidgetGridTest.cpp` | Tests voor het wire-formaat. |
| `Sources/DashboardCore/WidgetGridPackage.swift` | Swift-kant van hetzelfde wire-formaat. |
| `Sources/DashboardCore/WidgetGridComposition.swift` | `WidgetSlotKind.date(field:)` en de vertaling naar een widget. |
| `X3DashboardApp/WidgetInspectorSheet.swift` | Veldkiezer in de editor. |
| `X3DashboardApp/WidgetCompositionView.swift` | Datumtegel in de preview. |

De datumrekenkunde krijgt bewust een eigen bestand: het zijn pure functies van `(jaar, maand, dag)` die zonder renderer, klok of SDK te testen zijn, en dat is precies waar de randgevallen zitten.

---

### Task 1: Datumrekenkunde en NL-naamtabellen

DeepSeek heeft hiervan een geverifieerde versie gebouwd in `/tmp/x3-date-helpers`, empirisch gevalideerd tegen het systeem over 2000-2049. Gebruik die als vertrekpunt, maar draai de tests hier opnieuw: dit plan vertrouwt geen enkel resultaat dat niet in deze repo is aangetoond.

**Files:**
- Create: `src/spikes/ble_handoff/DashboardDateFields.h`
- Create: `src/spikes/ble_handoff/DashboardDateFields.cpp`
- Test: `test/ble_handoff_record/DashboardDateFieldsTest.cpp`
- Modify: `test/ble_handoff_record/CMakeLists.txt`

- [ ] **Step 1: Schrijf de falende test**

Maak `test/ble_handoff_record/DashboardDateFieldsTest.cpp`:

```cpp
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

}  // namespace
```

Voeg het testbestand toe aan `test/ble_handoff_record/CMakeLists.txt`: zet `DashboardDateFieldsTest.cpp` in de `add_executable(BleHandoffRecordTest ...)`-lijst na `DashboardWidgetGridTest.cpp`, en `${REPO_ROOT}/src/spikes/ble_handoff/DashboardDateFields.cpp` in dezelfde lijst na de andere `${REPO_ROOT}`-bronnen.

- [ ] **Step 2: Draai de test en zie hem falen**

```bash
export PATH="$HOME/.platformio/packages/tool-cmake/bin:$PATH"
cmake -S test -B /tmp/crossink-test-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/crossink-test-build --target BleHandoffRecordTest -j8
```

Verwacht: compileerfout, `DashboardDateFields.h: No such file or directory`.

- [ ] **Step 3: Schrijf de header**

`src/spikes/ble_handoff/DashboardDateFields.h`:

```cpp
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
```

- [ ] **Step 4: Schrijf de implementatie**

`src/spikes/ble_handoff/DashboardDateFields.cpp`:

```cpp
#include "DashboardDateFields.h"

namespace dashboard {
namespace {

constexpr uint8_t kJanuary = 1u;
constexpr uint8_t kFebruary = 2u;
constexpr uint8_t kMarch = 3u;
constexpr uint8_t kMonthMin = 1u;
constexpr uint8_t kMonthMax = 12u;

constexpr bool isLeapYear(uint16_t year) {
  return (year % 4u == 0u && year % 100u != 0u) || (year % 400u == 0u);
}

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

  const int zeroBasedSunday = (adjustedYear + adjustedYear / 4 - adjustedYear / 100 + adjustedYear / 400 +
                               table[monthNumber - 1] + dayNumber) %
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
  static constexpr const char* const names[12] = {"januari", "februari", "maart",     "april",   "mei",      "juni",
                                                  "juli",    "augustus", "september", "oktober", "november", "december"};
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
```

Deze implementatie is al empirisch geverifieerd tegen Pythons kalender over 2000-2049 (18.263 dagen, nul verschillen) en compileert schoon met `-Wall -Wextra -Werror -Wconversion`. Stap 6 herhaalt die controle in deze repo, want een resultaat dat hier niet is aangetoond telt niet.

- [ ] **Step 5: Draai de tests en zie ze slagen**

```bash
cmake --build /tmp/crossink-test-build --target BleHandoffRecordTest -j8
/tmp/crossink-test-build/ble_handoff_record/BleHandoffRecordTest --gtest_filter='DashboardDateFields.*'
```

Verwacht: `[  PASSED  ] 5 tests.`

- [ ] **Step 6: Valideer empirisch tegen het systeem**

Dit is de stap die de echte fouten vangt. Vergelijk elke dag van 2000 tot en met 2049 met Pythons eigen kalender:

```bash
python3 - <<'EOF' > /tmp/expected-dates.txt
import datetime
d = datetime.date(2000, 1, 1)
end = datetime.date(2049, 12, 31)
while d <= end:
    iso = d.isocalendar()
    print(f"{d.year} {d.month} {d.day} {d.weekday()} {iso[0]} {iso[1]}")
    d += datetime.timedelta(days=1)
EOF
wc -l /tmp/expected-dates.txt
```

Verwacht: `18263 /tmp/expected-dates.txt`.

Maak een wegwerpharness `/tmp/date-parity.cpp`:

```cpp
#include <cstdio>
#include "DashboardDateFields.h"

int main() {
  std::FILE* file = std::fopen("/tmp/expected-dates.txt", "r");
  if (file == nullptr) { std::printf("cannot open expectations\n"); return 2; }
  int year = 0, month = 0, day = 0, weekday = 0, isoYear = 0, isoWeek = 0;
  long checked = 0, failed = 0;
  while (std::fscanf(file, "%d %d %d %d %d %d", &year, &month, &day, &weekday, &isoYear, &isoWeek) == 6) {
    const auto y = static_cast<uint16_t>(year);
    const auto m = static_cast<uint8_t>(month);
    const auto d = static_cast<uint8_t>(day);
    const int gotWeekday = static_cast<int>(dashboard::weekdayFromDate(y, m, d));
    const dashboard::IsoWeek gotIso = dashboard::isoWeekFromDate(y, m, d);
    if (gotWeekday != weekday || gotIso.year != isoYear || gotIso.week != isoWeek) {
      if (failed < 10) {
        std::printf("%04d-%02d-%02d expected wd=%d iso=%d-W%02d got wd=%d iso=%d-W%02d\n", year, month, day,
                    weekday, isoYear, isoWeek, gotWeekday, gotIso.year, gotIso.week);
      }
      ++failed;
    }
    ++checked;
  }
  std::fclose(file);
  std::printf("checked %ld days, %ld mismatches\n", checked, failed);
  return failed == 0 ? 0 : 1;
}
```

```bash
clang++ -std=c++17 -Wall -Wextra -Werror \
  -I src/spikes/ble_handoff \
  /tmp/date-parity.cpp src/spikes/ble_handoff/DashboardDateFields.cpp -o /tmp/date-parity
/tmp/date-parity
```

Verwacht: `checked 18263 days, 0 mismatches` en exit code 0. Bij mismatches: repareer de implementatie, niet de test.

- [ ] **Step 7: Formatteer en commit**

```bash
find src test -name "*.cpp" -o -name "*.h" | xargs clang-format -i
git add src/spikes/ble_handoff/DashboardDateFields.h src/spikes/ble_handoff/DashboardDateFields.cpp \
        test/ble_handoff_record/DashboardDateFieldsTest.cpp test/ble_handoff_record/CMakeLists.txt
git commit -m "feat: compute Dutch weekday, month and ISO week names from a date"
```

---

### Task 2: `WidgetType::Date` in het C++ wire-formaat

**Files:**
- Modify: `src/spikes/ble_handoff/DashboardWidgetGrid.h`
- Modify: `src/spikes/ble_handoff/DashboardWidgetGrid.cpp`
- Test: `test/ble_handoff_record/DashboardWidgetGridTest.cpp`

- [ ] **Step 1: Schrijf de falende tests**

Voeg onderaan `test/ble_handoff_record/DashboardWidgetGridTest.cpp`, binnen de bestaande anonieme namespace, toe:

```cpp
dashboard::Widget dateWidget(uint8_t column, uint8_t row, dashboard::DateField field, uint8_t columnSpan = 1,
                             uint8_t rowSpan = 1) {
  dashboard::Widget widget{};
  widget.type = dashboard::WidgetType::Date;
  widget.column = column;
  widget.row = row;
  widget.columnSpan = columnSpan;
  widget.rowSpan = rowSpan;
  widget.dateField = field;
  return widget;
}

TEST(DashboardWidgetGrid, DateWidgetCostsEightBytes) {
  dashboard::WidgetGridPackage package{};
  package.packageId = 7;
  package.widgets[0] = dateWidget(0, 0, dashboard::DateField::Auto, 2, 2);
  package.widgetCount = 1;

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);
  // 31-byte prefix + 8-byte date widget + 4-byte CRC.
  EXPECT_EQ(length, 43u);
}

TEST(DashboardWidgetGrid, DateWidgetRoundTripsEveryField) {
  const dashboard::DateField fields[] = {dashboard::DateField::Auto,  dashboard::DateField::Day,
                                         dashboard::DateField::Weekday, dashboard::DateField::Month,
                                         dashboard::DateField::Year,  dashboard::DateField::WeekNumber};
  for (const dashboard::DateField field : fields) {
    dashboard::WidgetGridPackage package{};
    package.packageId = 9;
    package.widgets[0] = dateWidget(1, 2, field);
    package.widgetCount = 1;

    dashboard::PackageBytes bytes{};
    size_t length = 0;
    ASSERT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);

    dashboard::WidgetGridPackage decoded{};
    ASSERT_EQ(dashboard::decodeWidgetGridPackage(bytes.data(), length, decoded), dashboard::Status::Ok);
    ASSERT_EQ(decoded.widgetCount, 1);
    EXPECT_EQ(decoded.widgets[0].type, dashboard::WidgetType::Date);
    EXPECT_EQ(decoded.widgets[0].dateField, field);
    EXPECT_EQ(decoded.widgets[0].column, 1);
    EXPECT_EQ(decoded.widgets[0].row, 2);
  }
}

TEST(DashboardWidgetGrid, DateWidgetRefusesUnknownField) {
  dashboard::WidgetGridPackage package{};
  package.packageId = 11;
  package.widgets[0] = dateWidget(0, 0, static_cast<dashboard::DateField>(6));
  package.widgetCount = 1;

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidArgument);
}

// The hand-derived vector the Swift side must reproduce byte for byte.
TEST(DashboardWidgetGrid, DateWidgetMatchesHandDerivedBytes) {
  dashboard::WidgetGridPackage package{};
  package.packageId = 0x01020304;
  package.generatedAt = 1000;
  package.validUntil = 2000;
  package.style = 0;
  package.widgets[0] = dateWidget(2, 3, dashboard::DateField::WeekNumber, 1, 1);
  package.widgets[0].style = dashboard::makeWidgetStyle(5, 2, 1);
  package.widgetCount = 1;

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);

  // The widget's own 8 bytes start right after the 31-byte prefix.
  const uint8_t expected[] = {
      3,          // type = Date
      2,          // column
      3,          // row
      1,          // columnSpan
      1,          // rowSpan
      0x05, 0x03, // style: iconId 5 | sizeRung 2 << 7 | emphasis 1 << 9 = 0x0305, little endian
      5,          // field = WeekNumber
  };
  for (size_t index = 0; index < sizeof(expected); ++index) {
    EXPECT_EQ(bytes[31 + index], expected[index]) << "byte " << index;
  }
}
```

- [ ] **Step 2: Draai de tests en zie ze falen**

```bash
cmake --build /tmp/crossink-test-build --target BleHandoffRecordTest -j8
```

Verwacht: compileerfout, `'DateField' is not a member of 'dashboard'`.

- [ ] **Step 3: Breid de header uit**

In `src/spikes/ble_handoff/DashboardWidgetGrid.h`, vervang de `WidgetType`-regel:

```cpp
enum class WidgetType : uint8_t { Kpi = 1, List = 2, Date = 3 };

// Which field of today's date a Date widget shows. `Auto` lets the renderer
// pick a layout from the tile's size; every other value pins the tile to one
// field at whatever size fits. The date itself never travels in the package —
// the X3 reads its own RTC — so this byte is a Date widget's entire payload.
enum class DateField : uint8_t { Auto = 0, Day = 1, Weekday = 2, Month = 3, Year = 4, WeekNumber = 5 };
constexpr uint8_t MAX_DATE_FIELD = 5;
```

Vervang in `struct Widget` het losse `uint8_t listIndex = 0;` door een union die de byte deelt. Een eigen byte voor `dateField` padt `Widget` van 42 naar 44, en `MAX_WIDGETS` daarvan is 1056 bytes — voorbij het 1 KB-budget dat `WidgetSlotsAreCheapEnoughToCoverTheWholeGrid` bewaakt. Een widget is nooit tegelijk een lijst en een datum, dus de byte delen kost niets:

```cpp
  // One byte whose meaning follows `type`: a List widget's index into the
  // package's `lists`, or a Date widget's field. A widget is never both, and
  // sharing the byte is not a micro-optimisation: giving Date its own byte
  // padded Widget from 42 to 44, and MAX_WIDGETS of those is 1056 bytes -
  // past the 1 KB budget WidgetSlotsAreCheapEnoughToCoverTheWholeGrid guards
  // so that widening the grid stays a policy decision rather than a memory one.
  union {
    uint8_t listIndex = 0;
    DateField dateField;
  };
```

De union laat een bestaande accolade-initialisatie in `test/ble_handoff_record/DashboardGridLayoutTest.cpp` waarschuwen over een ontbrekend veld. Voeg daar `, {}` toe aan de twee `dashboard::Widget{...}`-regels, zodat de build waarschuwingsvrij blijft.

- [ ] **Step 4: Breid validatie, lengte, schrijven en lezen uit**

In `src/spikes/ble_handoff/DashboardWidgetGrid.cpp`:

In `validateWidget()`, direct vóór de afsluitende `return Status::InvalidArgument;`:

```cpp
  if (widget.type == WidgetType::Date) {
    if (static_cast<uint8_t>(widget.dateField) > MAX_DATE_FIELD) return Status::InvalidArgument;
    return Status::Ok;
  }
```

In `widgetContentLength()`, direct onder de `Kpi`-regel:

```cpp
  if (widget.type == WidgetType::Date) return total + 1;
```

In `writeWidget()`, direct onder het `Kpi`-blok (dus na diens `return offset;`):

```cpp
  if (widget.type == WidgetType::Date) {
    out[offset] = static_cast<uint8_t>(widget.dateField);
    return offset + 1;
  }
```

In `readWidget()`, vervang de typecontrole:

```cpp
  const uint8_t rawType = bytes[offset];
  switch (rawType) {
    case static_cast<uint8_t>(WidgetType::Kpi):
    case static_cast<uint8_t>(WidgetType::List):
    case static_cast<uint8_t>(WidgetType::Date):
      break;
    default:
      return SIZE_MAX;
  }
```

en voeg direct ná `offset += 7;` toe, vóór het `Kpi`-blok:

```cpp
  if (widget.type == WidgetType::Date) {
    if (offset + 1 > size) return SIZE_MAX;
    // Only the structural read happens here; validateWidget() rejects a field
    // value above MAX_DATE_FIELD, the same split every other field uses.
    widget.dateField = static_cast<DateField>(bytes[offset]);
    return offset + 1;
  }
```

- [ ] **Step 5: Draai de tests en zie ze slagen**

```bash
cmake --build /tmp/crossink-test-build --target BleHandoffRecordTest -j8
/tmp/crossink-test-build/ble_handoff_record/BleHandoffRecordTest
```

Verwacht: alle tests slagen, waaronder de vier nieuwe `DashboardWidgetGrid.Date*`-tests. De renderer compileert nog niet mee in dit doel, dus de niet-uitputtende `switch` in `DashboardGridRenderer.cpp` valt hier nog niet op; die wordt in Task 4 gerepareerd.

- [ ] **Step 6: Formatteer en commit**

```bash
find src test -name "*.cpp" -o -name "*.h" | xargs clang-format -i
git add src/spikes/ble_handoff/DashboardWidgetGrid.h src/spikes/ble_handoff/DashboardWidgetGrid.cpp \
        test/ble_handoff_record/DashboardWidgetGridTest.cpp
git commit -m "feat: carry a date widget in the dashboard package"
```

---

### Task 3: `date`-widget in het Swift wire-formaat

Werk vanaf hier in `/Volumes/2TB/Development/Projects/xteink-x3-dashboard-ios`.

**Files:**
- Modify: `Sources/DashboardCore/WidgetGridPackage.swift`
- Test: `Tests/DashboardCoreTests/WidgetGridPackageTests.swift`

- [ ] **Step 1: Schrijf de falende tests**

Voeg toe aan `Tests/DashboardCoreTests/WidgetGridPackageTests.swift`:

```swift
func testDateWidgetCostsEightBytes() throws {
    let widget = DashboardWidget(
        column: 0, row: 0, columnSpan: 2, rowSpan: 2,
        iconId: 0, sizeRung: 0, emphasis: 0,
        content: .date(field: .auto)
    )
    let bytes = try WidgetGridPackage(packageId: 7, generatedAt: 1000, validUntil: 2000, style: 0, widgets: [widget])
        .encoded()
    // 31-byte prefix + 8-byte date widget + 4-byte CRC.
    XCTAssertEqual(bytes.count, 43)
}

func testDateWidgetRoundTripsEveryField() throws {
    for field in DashboardDateField.allCases {
        let widget = DashboardWidget(
            column: 1, row: 2, columnSpan: 1, rowSpan: 1,
            iconId: 0, sizeRung: 0, emphasis: 0,
            content: .date(field: field)
        )
        let bytes = try WidgetGridPackage(packageId: 9, generatedAt: 1000, validUntil: 2000, style: 0, widgets: [widget])
            .encoded()
        let decoded = try WidgetGridPackage.decode(bytes)
        XCTAssertEqual(decoded.widgets.count, 1)
        XCTAssertEqual(decoded.widgets[0].content, .date(field: field))
        XCTAssertEqual(decoded.widgets[0].column, 1)
        XCTAssertEqual(decoded.widgets[0].row, 2)
    }
}

func testDateWidgetRefusesUnknownFieldByte() throws {
    let widget = DashboardWidget(
        column: 0, row: 0, columnSpan: 1, rowSpan: 1,
        iconId: 0, sizeRung: 0, emphasis: 0,
        content: .date(field: .auto)
    )
    var bytes = try WidgetGridPackage(packageId: 11, generatedAt: 1000, validUntil: 2000, style: 0, widgets: [widget])
        .encoded()
    // Corrupt the field byte, then repair the CRC so the decoder reaches field validation.
    bytes[38] = 6
    let crc = CRC32.checksum(Array(bytes[0..<(bytes.count - 4)]))
    for index in 0..<4 {
        bytes[bytes.count - 4 + index] = UInt8(truncatingIfNeeded: crc >> (index * 8))
    }
    XCTAssertThrowsError(try WidgetGridPackage.decode(bytes)) { error in
        XCTAssertEqual(error as? DashboardPackageError, .invalidArgument)
    }
}

// The same hand-derived vector as DashboardWidgetGrid.DateWidgetMatchesHandDerivedBytes
// in the firmware tests. If these two ever disagree, the wire format has drifted.
func testDateWidgetMatchesHandDerivedBytes() throws {
    let widget = DashboardWidget(
        column: 2, row: 3, columnSpan: 1, rowSpan: 1,
        iconId: 5, sizeRung: 2, emphasis: 1,
        content: .date(field: .weekNumber)
    )
    let bytes = try WidgetGridPackage(
        packageId: 0x0102_0304, generatedAt: 1000, validUntil: 2000, style: 0, widgets: [widget]
    ).encoded()
    let expected: [UInt8] = [3, 2, 3, 1, 1, 0x05, 0x03, 5]
    XCTAssertEqual(Array(bytes[31..<(31 + expected.count)]), expected)
}
```

Als de initialisatorsignaturen van `DashboardWidget` of `WidgetGridPackage` in dit bestand afwijken, neem dan de vorm over die de bestaande KPI-tests in hetzelfde bestand gebruiken; de veldnamen hierboven volgen `WidgetGridPackage.swift` zoals het er nu staat.

- [ ] **Step 2: Draai de tests en zie ze falen**

```bash
swift test --filter WidgetGridPackageTests
```

Verwacht: compileerfout, `cannot infer contextual base in reference to member 'date'`.

- [ ] **Step 3: Breid het model uit**

In `Sources/DashboardCore/WidgetGridPackage.swift`:

Voeg aan `WidgetGridPackageLayout` toe, bij de andere grenzen:

```swift
    public static let maxDateField: UInt8 = 5
```

Breid `DashboardWidgetType` uit:

```swift
    case date = 3
```

Voeg het veldtype toe, direct boven `DashboardWidgetContent`:

```swift
/// Which field of today's date a date tile shows, mirroring
/// `dashboard::DateField`. `auto` lets the firmware pick a layout from the
/// tile's size; every other case pins the tile to one field. The date itself
/// is never sent — the X3 reads its own clock — so this is the whole payload.
public enum DashboardDateField: UInt8, Sendable, Codable, CaseIterable, Hashable {
    case auto = 0
    case day = 1
    case weekday = 2
    case month = 3
    case year = 4
    case weekNumber = 5
}
```

Breid `DashboardWidgetContent` uit met de case en de `type`-tak:

```swift
    case date(field: DashboardDateField)
```

```swift
        case .date: return .date
```

- [ ] **Step 4: Breid encode en decode uit**

In `DashboardWidget.encodedBytes()`, voeg een case toe aan de `switch content`:

```swift
        case .date(let field):
            return [
                DashboardWidgetType.date.rawValue, column, row, columnSpan, rowSpan,
                styleLow, styleHigh,
                field.rawValue,
            ]
```

In `RawWidget.RawKind`, voeg toe:

```swift
            case date(fieldByte: UInt8)
```

In `RawWidget.validated()`, voeg een case toe aan de `switch kind`:

```swift
            case .date(let fieldByte):
                guard let field = DashboardDateField(rawValue: fieldByte) else {
                    throw DashboardPackageError.invalidArgument
                }
                return DashboardWidget(
                    column: column, row: row, columnSpan: columnSpan, rowSpan: rowSpan,
                    iconId: iconId, sizeRung: sizeRung, emphasis: emphasis,
                    content: .date(field: field)
                )
```

In `readRawWidget()`, voeg een case toe aan de `switch rawType`:

```swift
        case DashboardWidgetType.date.rawValue:
            guard offset + 1 <= size else { throw DashboardPackageError.invalidLength }
            // Only the structural read here; the field's range is checked in
            // validated(), mirroring readWidget()/validateWidget() in C++.
            let raw = RawWidget(
                column: column, row: row, columnSpan: columnSpan, rowSpan: rowSpan, style: style,
                kind: .date(fieldByte: bytes[offset])
            )
            return (raw, offset + 1)
```

- [ ] **Step 5: Draai de tests en zie ze slagen**

```bash
swift test --filter WidgetGridPackageTests
```

Verwacht: alle tests slagen, waaronder de vier nieuwe.

- [ ] **Step 6: Commit**

```bash
git add Sources/DashboardCore/WidgetGridPackage.swift Tests/DashboardCoreTests/WidgetGridPackageTests.swift
git commit -m "feat: carry a date widget in the dashboard package"
```

---

### Task 4: Datumtegel renderen — gekozen veld

**Files:**
- Modify: `src/spikes/ble_handoff/DashboardGridRenderer.cpp`

Terug in `/Volumes/2TB/Development/Projects/CrossInk`.

De renderer heeft geen hosttests: hij hangt aan `GfxRenderer` en de fonts van het toestel. De verificatie is dat het bouwt en dat het op de X3 klopt. Houd de rekenkunde daarom in Task 1, waar hij wél getest is.

- [ ] **Step 1: Voeg de klokbron en de tekstpassing toe**

Voeg bovenaan `DashboardGridRenderer.cpp` bij de includes toe:

```cpp
#include <HalClock.h>

#include "DashboardDateFields.h"
```

Controleer eerst hoe de rest van de firmware de klok bereikt:

```bash
grep -rn "halClock" src/main.cpp | head -5
```

Neem exact dezelfde toegangsvorm over als daar staat (een singleton-macro of een globale referentie); verzin er geen nieuwe.

Voeg binnen de anonieme namespace toe, onder `TILE_STACK_GAP`:

```cpp
// The dashboard trusts the clock on the same terms as the rest of the firmware:
// CrossPointSettings.cpp refuses RTC dates before 2025 because the X3's
// hardware predates that migration, and a made-up date on a wall display is
// worse than an obviously empty one.
constexpr uint16_t MIN_TRUSTED_YEAR = 2025;

struct TodaysDate {
  bool valid = false;
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
};

TodaysDate readTodaysDate() {
  TodaysDate today{};
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (!halClock.getDateTime(year, month, day, hour, minute)) return today;
  if (year < MIN_TRUSTED_YEAR || !isValidDate(year, month, day)) return today;
  today.valid = true;
  today.year = year;
  today.month = month;
  today.day = day;
  return today;
}

// The largest rung at or below `maxRung` whose rendering of `text` still fits
// `maxWidth`. Returns the smallest rung when nothing fits; the caller then
// truncates, which is the same fallback drawTextCenteredInRect already uses.
int fittingFontId(const GfxRenderer& renderer, const char* text, const int maxWidth, const uint8_t maxRung,
                  const EpdFontFamily::Style style) {
  for (int rung = static_cast<int>(maxRung); rung > 0; --rung) {
    if (renderer.getTextWidth(VALUE_FONT_FOR_RUNG[rung], text, style) <= maxWidth) {
      return VALUE_FONT_FOR_RUNG[rung];
    }
  }
  return VALUE_FONT_FOR_RUNG[0];
}

// A weekday name that fits, falling back to the two-letter abbreviation rather
// than to a truncation like "donderda".
const char* fittingWeekday(const GfxRenderer& renderer, const Weekday weekday, const int fontId, const int maxWidth) {
  const char* full = weekdayName(weekday);
  if (renderer.getTextWidth(fontId, full, EpdFontFamily::REGULAR) <= maxWidth) return full;
  return weekdayAbbreviation(weekday);
}
```

- [ ] **Step 2: Teken een gekozen veld**

Voeg toe onder `renderKpiWidget`:

```cpp
// A tile pinned to one date field: the value as large as it fits, with a small
// label above it only where the number alone would be a riddle ("33").
void renderDateFieldWidget(GfxRenderer& renderer, const WidgetRect& rect, const Widget& widget,
                           const TodaysDate& today, const int padding) {
  const bool ink = !isInverted(widget);
  const int innerWidth = rect.width - 2 * padding;

  char value[16] = {};
  const char* label = "";
  switch (widget.dateField) {
    case DateField::Day:
      std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>(today.day));
      break;
    case DateField::Weekday:
      std::snprintf(value, sizeof(value), "%s",
                    fittingWeekday(renderer, weekdayFromDate(today.year, today.month, today.day),
                                   VALUE_FONT_FOR_RUNG[0], innerWidth));
      break;
    case DateField::Month:
      std::snprintf(value, sizeof(value), "%s", monthName(today.month));
      break;
    case DateField::Year:
      std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>(today.year));
      break;
    case DateField::WeekNumber:
      std::snprintf(value, sizeof(value), "%u",
                    static_cast<unsigned>(isoWeekFromDate(today.year, today.month, today.day).week));
      label = "week";
      break;
    case DateField::Auto:
      // Handled by renderDateAutoWidget; never reaches here.
      return;
  }

  const int labelAscender = renderer.getFontAscenderSize(LABEL_FONT_ID);
  const int labelHeight = label[0] != '\0' ? labelAscender + TILE_STACK_GAP : 0;
  const int valueFontId = fittingFontId(renderer, value, innerWidth, widgetSizeRung(widget.style),
                                        EpdFontFamily::BOLD);
  const int valueAscender = renderer.getFontAscenderSize(valueFontId);

  int y = std::max(padding, (rect.height - labelHeight - valueAscender) / 2);
  if (labelHeight > 0) {
    drawTextCenteredInRect(renderer, LABEL_FONT_ID, rect, label, EpdFontFamily::REGULAR, y, padding, ink);
    y += labelHeight;
  }
  drawTextCenteredInRect(renderer, valueFontId, rect, value, EpdFontFamily::BOLD, y, padding, ink);
}
```

- [ ] **Step 3: Teken de lege tegel bij een onbetrouwbare klok**

Voeg toe onder de vorige functie:

```cpp
// What a date tile shows when the RTC cannot be trusted. A dash reads as "no
// data" at arm's length; a wrong date does not.
void renderDatePlaceholder(GfxRenderer& renderer, const WidgetRect& rect, const bool ink, const int padding) {
  const int fontId = VALUE_FONT_FOR_RUNG[1];
  const int ascender = renderer.getFontAscenderSize(fontId);
  const int y = std::max(padding, (rect.height - ascender) / 2);
  drawTextCenteredInRect(renderer, fontId, rect, "—", EpdFontFamily::REGULAR, y, padding, ink);
}
```

- [ ] **Step 4: Sluit het aan op de rendering**

Voeg in `renderWidgetGrid()` de ontbrekende `switch`-tak toe, direct onder de `List`-tak:

```cpp
      case WidgetType::Date: {
        const TodaysDate today = readTodaysDate();
        fillTile(renderer, rects[index], widgetEmphasis(widget.style));
        if (!today.valid) {
          renderDatePlaceholder(renderer, rects[index], !isInverted(widget), padding);
        } else if (widget.dateField == DateField::Auto) {
          renderDateAutoWidget(renderer, rects[index], widget, today, padding);
        } else {
          renderDateFieldWidget(renderer, rects[index], widget, today, padding);
        }
        break;
      }
```

`renderDateAutoWidget` bestaat nog niet; Task 5 voegt hem toe. Voeg voor nu bovenaan de anonieme namespace een voorlopige versie toe die de compilatie sluitend maakt en al iets bruikbaars tekent:

```cpp
void renderDateAutoWidget(GfxRenderer& renderer, const WidgetRect& rect, const Widget& widget,
                          const TodaysDate& today, const int padding);
```

en een definitie onder `renderDateFieldWidget` die voorlopig het dagnummer tekent:

```cpp
void renderDateAutoWidget(GfxRenderer& renderer, const WidgetRect& rect, const Widget& widget,
                          const TodaysDate& today, const int padding) {
  Widget dayWidget = widget;
  dayWidget.dateField = DateField::Day;
  renderDateFieldWidget(renderer, rect, dayWidget, today, padding);
}
```

Voeg `#include <cstdio>` toe aan de includes voor `std::snprintf`.

- [ ] **Step 5: Bouw voor het toestel**

```bash
pio run -e spike-ble-reader-x3
```

Verwacht: build succeeded. Faalt de include van `HalClock.h` of de naam `halClock`, kijk dan opnieuw naar hoe `src/main.cpp` de klok bereikt en neem dat over.

- [ ] **Step 6: Formatteer en commit**

```bash
find src -name "*.cpp" -o -name "*.h" | xargs clang-format -i
git add src/spikes/ble_handoff/DashboardGridRenderer.cpp
git commit -m "feat: draw a date tile from the X3's own clock"
```

---

### Task 5: De indelingsladder voor `auto`

**Files:**
- Modify: `src/spikes/ble_handoff/DashboardGridRenderer.cpp`

De grenzen liggen tussen de maten die het raster echt oplevert: bij dichtheid "normaal" (padding 8) is een 1×1-tegel binnenwerks ongeveer 111×112 px en een 2×2-tegel ongeveer 239×240. De drempels 200 en 170 vallen daar ruim tussen, dus een tegel springt niet van indeling door een pixel verschil.

- [ ] **Step 1: Vervang de voorlopige `renderDateAutoWidget`**

```cpp
// Layout thresholds on the tile's inner size, not on columnSpan/rowSpan, so an
// unusual span like 4x1 or 3x2 lands somewhere sensible without a table of
// span combinations. At density "normal" a 1x1 tile is about 111x112 inner
// pixels and a 2x2 about 239x240, so both thresholds sit well clear of the
// sizes the grid actually produces.
constexpr int DATE_WIDE_THRESHOLD = 200;
constexpr int DATE_TALL_THRESHOLD = 170;

void renderDateAutoWidget(GfxRenderer& renderer, const WidgetRect& rect, const Widget& widget,
                          const TodaysDate& today, const int padding) {
  const bool ink = !isInverted(widget);
  const int innerWidth = rect.width - 2 * padding;
  const int innerHeight = rect.height - 2 * padding;
  const Weekday weekday = weekdayFromDate(today.year, today.month, today.day);
  const IsoWeek isoWeek = isoWeekFromDate(today.year, today.month, today.day);
  const uint8_t maxRung = widgetSizeRung(widget.style);
  const int labelAscender = renderer.getFontAscenderSize(LABEL_FONT_ID);

  char day[4] = {};
  std::snprintf(day, sizeof(day), "%u", static_cast<unsigned>(today.day));
  char week[12] = {};
  std::snprintf(week, sizeof(week), "week %u", static_cast<unsigned>(isoWeek.week));

  const bool wide = innerWidth >= DATE_WIDE_THRESHOLD;
  const bool tall = innerHeight >= DATE_TALL_THRESHOLD;

  if (wide && !tall) {
    // One line: "za 15 aug", with the week number under it when there is room.
    // This is the only layout with width to spare, so it is the only one that
    // honours the tile's iconId; elsewhere an icon would compete with the day
    // number for the same middle of the tile.
    char line[24] = {};
    std::snprintf(line, sizeof(line), "%s %u %s", weekdayAbbreviation(weekday),
                  static_cast<unsigned>(today.day), monthAbbreviation(today.month));
    const freeink::Icon* icon = iconFor(widgetIconId(widget.style), rect.height);
    const int iconWidth = icon != nullptr ? icon->w + TILE_STACK_GAP : 0;
    const int textWidth = innerWidth - iconWidth;
    const int lineFontId = fittingFontId(renderer, line, textWidth, maxRung, EpdFontFamily::BOLD);
    const int lineAscender = renderer.getFontAscenderSize(lineFontId);
    const bool showWeek = lineAscender + TILE_STACK_GAP + labelAscender <= innerHeight;
    const int blockHeight = lineAscender + (showWeek ? TILE_STACK_GAP + labelAscender : 0);
    int y = std::max(padding, (rect.height - blockHeight) / 2);
    if (icon != nullptr) {
      drawDashboardIcon(renderer, *icon, rect.x + padding, rect.y + std::max(padding, (rect.height - icon->h) / 2),
                        ink);
    }
    // The text block sits to the right of the icon, so it is centred in what is
    // left rather than in the whole tile.
    const WidgetRect textRect{rect.x + iconWidth, rect.y, rect.width - iconWidth, rect.height};
    drawTextCenteredInRect(renderer, lineFontId, textRect, line, EpdFontFamily::BOLD, y, padding, ink);
    if (showWeek) {
      y += lineAscender + TILE_STACK_GAP;
      drawTextCenteredInRect(renderer, LABEL_FONT_ID, textRect, week, EpdFontFamily::REGULAR, y, padding, ink);
    }
    return;
  }

  if (wide && tall) {
    // The full sheet: an inverted header with month and year, the day number
    // large, the weekday spelled out, and the week number as a footnote.
    char header[24] = {};
    std::snprintf(header, sizeof(header), "%s %u", monthName(today.month),
                  static_cast<unsigned>(today.year));
    const int headerHeight = labelAscender + 2 * TILE_STACK_GAP;
    renderer.fillRect(rect.x, rect.y, rect.width, headerHeight, ink);
    drawTextCenteredInRect(renderer, LABEL_FONT_ID, rect, header, EpdFontFamily::BOLD, TILE_STACK_GAP, padding,
                           !ink);

    const int dayFontId = fittingFontId(renderer, day, innerWidth, maxRung, EpdFontFamily::BOLD);
    const int dayAscender = renderer.getFontAscenderSize(dayFontId);
    const char* weekdayText = fittingWeekday(renderer, weekday, LABEL_FONT_ID, innerWidth);
    const int blockHeight = dayAscender + TILE_STACK_GAP + labelAscender + TILE_STACK_GAP + labelAscender;
    int y = headerHeight + std::max(padding, (rect.height - headerHeight - blockHeight) / 2);
    drawTextCenteredInRect(renderer, dayFontId, rect, day, EpdFontFamily::BOLD, y, padding, ink);
    y += dayAscender + TILE_STACK_GAP;
    drawTextCenteredInRect(renderer, LABEL_FONT_ID, rect, weekdayText, EpdFontFamily::REGULAR, y, padding, ink);
    y += labelAscender + TILE_STACK_GAP;
    drawTextCenteredInRect(renderer, LABEL_FONT_ID, rect, week, EpdFontFamily::REGULAR, y, padding, ink);
    return;
  }

  // Narrow, tall or short: weekday above the day number, with the abbreviated
  // month underneath only when the tile is tall enough to carry a third line.
  const char* weekdayText = fittingWeekday(renderer, weekday, LABEL_FONT_ID, innerWidth);
  const int dayFontId = fittingFontId(renderer, day, innerWidth, maxRung, EpdFontFamily::BOLD);
  const int dayAscender = renderer.getFontAscenderSize(dayFontId);
  const bool showMonth = tall && labelAscender + TILE_STACK_GAP + dayAscender + TILE_STACK_GAP + labelAscender <=
                                     innerHeight;
  const int blockHeight = labelAscender + TILE_STACK_GAP + dayAscender +
                          (showMonth ? TILE_STACK_GAP + labelAscender : 0);
  int y = std::max(padding, (rect.height - blockHeight) / 2);
  drawTextCenteredInRect(renderer, LABEL_FONT_ID, rect, weekdayText, EpdFontFamily::REGULAR, y, padding, ink);
  y += labelAscender + TILE_STACK_GAP;
  drawTextCenteredInRect(renderer, dayFontId, rect, day, EpdFontFamily::BOLD, y, padding, ink);
  if (showMonth) {
    y += dayAscender + TILE_STACK_GAP;
    drawTextCenteredInRect(renderer, LABEL_FONT_ID, rect, monthAbbreviation(today.month),
                           EpdFontFamily::REGULAR, y, padding, ink);
  }
}
```

Verwijder de voorlopige definitie uit Task 4 en de losse voorwaartse declaratie als de functie nu vóór `renderWidgetGrid` staat.

- [ ] **Step 2: Bouw voor het toestel**

```bash
pio run -e spike-ble-reader-x3
```

Verwacht: build succeeded, zonder waarschuwingen over ongebruikte variabelen.

- [ ] **Step 3: Formatteer en commit**

```bash
find src -name "*.cpp" -o -name "*.h" | xargs clang-format -i
git add src/spikes/ble_handoff/DashboardGridRenderer.cpp
git commit -m "feat: pick a date tile layout from the space it has"
```

---

### Task 6: Datumtegel in de compositie van de app

**Files:**
- Modify: `Sources/DashboardCore/WidgetGridComposition.swift`
- Test: `Tests/DashboardCoreTests/WidgetGridCompositionTests.swift`

- [ ] **Step 1: Schrijf de falende test**

Voeg toe aan `Tests/DashboardCoreTests/WidgetGridCompositionTests.swift`:

```swift
func testDateSlotBecomesADateWidget() throws {
    let composition = WidgetGridComposition(slots: [
        WidgetSlot(kind: .date(field: .weekNumber), column: 0, row: 0, columnSpan: 1, rowSpan: 1)
    ])
    let widgets = WidgetGridComposer.widgets(for: composition)
    XCTAssertEqual(widgets.count, 1)
    XCTAssertEqual(widgets[0].content, .date(field: .weekNumber))
}

func testDateSlotSurvivesCoding() throws {
    let slot = WidgetSlot(kind: .date(field: .auto), column: 1, row: 1, columnSpan: 2, rowSpan: 2)
    let data = try JSONEncoder().encode(slot)
    let decoded = try JSONDecoder().decode(WidgetSlot.self, from: data)
    XCTAssertEqual(decoded.kind, .date(field: .auto))
}
```

Als `WidgetGridComposer.widgets(for:)` in dit bestand met meer argumenten wordt aangeroepen (bijvoorbeeld een KPI-resolver of een Home Assistant-cache), neem dan de aanroepvorm over die de bestaande tests in dit bestand gebruiken en vul de extra argumenten met dezelfde lege waarden als daar.

- [ ] **Step 2: Draai de test en zie hem falen**

```bash
swift test --filter WidgetGridCompositionTests
```

Verwacht: compileerfout op `.date(field:)`.

- [ ] **Step 3: Breid het model uit**

In `Sources/DashboardCore/WidgetGridComposition.swift`, voeg een case toe aan `WidgetSlotKind`:

```swift
    /// Today's date, rendered by the firmware from the X3's own clock. Unlike
    /// every other slot kind this carries no data: only which field to show.
    case date(field: DashboardDateField)
```

Voeg in `WidgetGridComposer.widgets(...)` een case toe aan de `switch slot.kind`:

```swift
            case .date(let field):
                widgets.append(
                    DashboardWidget(
                        column: slot.column, row: slot.row,
                        columnSpan: slot.columnSpan, rowSpan: slot.rowSpan,
                        iconId: slot.iconId, sizeRung: slot.sizeRung, emphasis: slot.emphasis,
                        content: .date(field: field)
                    )
                )
```

`WidgetSlotKind` is een `Codable` enum met associated values, dus Swift genereert de coderingsvorm zelf; er is geen `CodingKeys`-aanpassing nodig zolang de bestaande cases dat ook niet hebben. Controleer dat met `grep -n "WidgetSlotKind" Sources/DashboardCore/WidgetGridComposition.swift` en volg wat er staat.

- [ ] **Step 4: Draai de tests en zie ze slagen**

```bash
swift test
```

Verwacht: alle tests slagen.

- [ ] **Step 5: Commit**

```bash
git add Sources/DashboardCore/WidgetGridComposition.swift Tests/DashboardCoreTests/WidgetGridCompositionTests.swift
git commit -m "feat: place a date tile from the composer"
```

---

### Task 7: Datumtegel in de editor

**Files:**
- Modify: `X3DashboardApp/WidgetInspectorSheet.swift`
- Modify: `X3DashboardApp/WidgetCompositionView.swift`

Deze taak heeft geen unit tests: het is UI die je in de simulator of op het toestel beoordeelt. Lees eerst hoe een bestaand tegeltype door beide bestanden loopt:

```bash
grep -n "agendaList" X3DashboardApp/WidgetInspectorSheet.swift X3DashboardApp/WidgetCompositionView.swift
```

- [ ] **Step 1: Voeg "Datum" toe als tegeltype in de inspector**

Waar de inspector het soort tegel laat kiezen, voeg "Datum" toe naast de bestaande keuzes. Kiest de gebruiker Datum, dan verschijnt eronder een veldkiezer:

```swift
Picker("Toont", selection: $dateField) {
    Text("Automatisch").tag(DashboardDateField.auto)
    Text("Dag").tag(DashboardDateField.day)
    Text("Weekdag").tag(DashboardDateField.weekday)
    Text("Maand").tag(DashboardDateField.month)
    Text("Jaar").tag(DashboardDateField.year)
    Text("Weeknummer").tag(DashboardDateField.weekNumber)
}
```

Met een voetnoot onder de kiezer, zodat "Automatisch" zichzelf uitlegt:

```swift
Text("Automatisch kiest de indeling op de grootte van de tegel.")
    .font(.footnote)
    .foregroundStyle(.secondary)
```

Schrijf de keuze terug als `.date(field: dateField)` op dezelfde manier waarop de inspector de andere tegeltypes terugschrijft.

- [ ] **Step 2: Teken de datumtegel in de preview**

Voeg in `WidgetCompositionView.swift` een tak toe waar de preview per `WidgetSlotKind` tekent. De preview benadert wat de firmware doet; hij hoeft niet exact te zijn, maar moet wel dezelfde ladder herkennen zodat de gebruiker ziet wat hij krijgt:

```swift
case .date(let field):
    DateTilePreview(field: field, size: tileSize)
```

Met een view die de vier indelingen benadert:

```swift
private struct DateTilePreview: View {
    let field: DashboardDateField
    let size: CGSize

    private var today: Date { Date() }

    var body: some View {
        // Same thresholds as DATE_WIDE_THRESHOLD / DATE_TALL_THRESHOLD in
        // DashboardGridRenderer.cpp, scaled to the preview's canvas the way the
        // other previews approximate the firmware.
        let wide = size.width >= 200
        let tall = size.height >= 170
        switch field {
        case .auto where wide && tall:
            VStack(spacing: 4) {
                Text(today.formatted(.dateTime.month(.wide).year()).uppercased())
                    .font(.caption2).bold()
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 4)
                    .background(Color.primary)
                    .foregroundStyle(Color(.systemBackground))
                Text(today.formatted(.dateTime.day())).font(.largeTitle).bold()
                Text(today.formatted(.dateTime.weekday(.wide))).font(.caption)
                Text("week \(weekOfYear)").font(.caption2)
            }
        case .auto where wide:
            VStack(spacing: 2) {
                Text(today.formatted(.dateTime.weekday(.abbreviated).day().month(.abbreviated))).bold()
                Text("week \(weekOfYear)").font(.caption2)
            }
        case .auto:
            VStack(spacing: 2) {
                Text(today.formatted(.dateTime.weekday(.wide))).font(.caption2)
                Text(today.formatted(.dateTime.day())).font(.title).bold()
                if tall { Text(today.formatted(.dateTime.month(.abbreviated))).font(.caption2) }
            }
        case .day: Text(today.formatted(.dateTime.day())).font(.largeTitle).bold()
        case .weekday: Text(today.formatted(.dateTime.weekday(.wide))).bold()
        case .month: Text(today.formatted(.dateTime.month(.wide))).bold()
        case .year: Text(today.formatted(.dateTime.year())).font(.title).bold()
        case .weekNumber:
            VStack(spacing: 2) {
                Text("week").font(.caption2)
                Text("\(weekOfYear)").font(.largeTitle).bold()
            }
        }
    }

    private var weekOfYear: Int {
        Calendar(identifier: .iso8601).component(.weekOfYear, from: today)
    }
}
```

- [ ] **Step 3: Bouw het app-target**

```bash
xcodebuild -project X3DashboardApp.xcodeproj -scheme X3DashboardApp \
  -destination 'generic/platform=iOS' build CODE_SIGNING_ALLOWED=NO
```

Verwacht: `** BUILD SUCCEEDED **`.

- [ ] **Step 4: Commit**

```bash
git add X3DashboardApp/WidgetInspectorSheet.swift X3DashboardApp/WidgetCompositionView.swift
git commit -m "feat: add a date tile to the dashboard editor"
```

---

### Task 8: Pariteit tussen de twee implementaties bewijzen

**Files:**
- Geen. Dit is een wegwerpharness in `/tmp`, net als bij eerdere sessies: hij hoort niet in de repo.

- [ ] **Step 1: Genereer de Swift-bytes**

Een executable target toevoegen zou `Package.swift` veranderen voor een wegwerpcontrole, dus de bytes komen uit een tijdelijke test. Voeg toe aan `Tests/DashboardCoreTests/WidgetGridPackageTests.swift`:

```swift
// Temporary: dumps the wire bytes for the C++/Swift parity check. Delete after.
func testDumpDateWireBytes() throws {
    var lines: [String] = []
    for field in DashboardDateField.allCases {
        let widget = DashboardWidget(
            column: 2, row: 3, columnSpan: 1, rowSpan: 1,
            iconId: 5, sizeRung: 2, emphasis: 1,
            content: .date(field: field)
        )
        let bytes = try WidgetGridPackage(
            packageId: 0x0102_0304, generatedAt: 1000, validUntil: 2000, style: 0, widgets: [widget]
        ).encoded()
        lines.append(bytes.map { String(format: "%02x", $0) }.joined(separator: " "))
    }
    try lines.joined(separator: "\n").appending("\n")
        .write(toFile: "/tmp/swift-date-bytes.txt", atomically: true, encoding: .utf8)
}
```

```bash
swift test --filter testDumpDateWireBytes
cat /tmp/swift-date-bytes.txt
```

Verwacht: zes regels van elk 43 hex-paren.

- [ ] **Step 2: Genereer de C++-bytes**

In de firmware-repo:

```bash
cat > /tmp/date-wire-dump.cpp <<'EOF'
#include <cstdio>
#include "DashboardWidgetGrid.h"

int main() {
  const dashboard::DateField fields[] = {dashboard::DateField::Auto,       dashboard::DateField::Day,
                                         dashboard::DateField::Weekday,    dashboard::DateField::Month,
                                         dashboard::DateField::Year,       dashboard::DateField::WeekNumber};
  for (const dashboard::DateField field : fields) {
    dashboard::WidgetGridPackage package{};
    package.packageId = 0x01020304;
    dashboard::Widget& widget = package.widgets[0];
    widget.type = dashboard::WidgetType::Date;
    widget.column = 2;
    widget.row = 3;
    widget.columnSpan = 1;
    widget.rowSpan = 1;
    widget.style = dashboard::makeWidgetStyle(5, 2, 1);
    widget.dateField = field;
    package.widgetCount = 1;

    dashboard::PackageBytes bytes{};
    size_t length = 0;
    if (dashboard::encodeWidgetGridPackage(package, bytes, length) != dashboard::Status::Ok) {
      std::printf("encode failed\n");
      return 2;
    }
    for (size_t index = 0; index < length; ++index) {
      std::printf("%02x%s", bytes[index], index + 1 == length ? "\n" : " ");
    }
  }
  return 0;
}
EOF
clang++ -std=c++20 -Wall -Wextra -I src/spikes/ble_handoff \
  /tmp/date-wire-dump.cpp \
  src/spikes/ble_handoff/DashboardWidgetGrid.cpp \
  src/spikes/ble_handoff/BleHandoffRecord.cpp \
  -o /tmp/date-wire-dump
/tmp/date-wire-dump > /tmp/cpp-date-bytes.txt
cat /tmp/cpp-date-bytes.txt
```

Ontbreekt er een symbool bij het linken, voeg dan het `.cpp`-bestand toe dat het definieert; `test/ble_handoff_record/CMakeLists.txt` noemt de volledige set bronnen die dit doel nodig heeft.

- [ ] **Step 3: Vergelijk**

```bash
diff /tmp/swift-date-bytes.txt /tmp/cpp-date-bytes.txt && echo "PARITEIT OK"
```

Verwacht: `PARITEIT OK`, met zes identieke regels van elk 43 bytes. Elk verschil is een echte bug in één van de twee implementaties — repareer die, verzoen de bestanden niet.

- [ ] **Step 4: Ruim de harness op**

```bash
rm -f /tmp/date-wire-dump /tmp/date-wire-dump.cpp /tmp/date-parity /tmp/date-parity.cpp
```

Verwijder ook de tijdelijke XCTest-methode uit stap 1. De blijvende bescherming zijn de handgeschreven bytevectors in beide testsuites, niet deze harness.

---

### Task 9: Changelog en documentatie

**Files:**
- Modify: `CHANGELOG.md` (firmware)
- Modify: `docs/superpowers/specs/2026-08-15-date-widget-design.md`

- [ ] **Step 1: Voeg een changelog-regel toe**

Onder de `### Added`-sectie van de bovenste versie in `CHANGELOG.md`:

```markdown
- Dashboardtegel die de datum toont, gelezen uit de klok van het toestel zelf. Je kiest in de iPhone-app welk veld de tegel laat zien — dag, weekdag, maand, jaar of weeknummer — of laat de indeling zich richten naar de grootte van de tegel.
```

- [ ] **Step 2: Zet de spec op geïmplementeerd**

Verander in de spec de statusregel:

```markdown
**Status:** geïmplementeerd
```

- [ ] **Step 3: Commit**

```bash
git add CHANGELOG.md docs/superpowers/specs/2026-08-15-date-widget-design.md
git commit -m "docs: record the date widget in the changelog"
```

---

### Task 10: Verificatie op de X3

Niets hiervan telt als af voordat de tegel op het echte paneel klopt. De iOS-simulator kan de BLE-overdracht niet draaien.

- [ ] **Step 1: Flash de firmware**

```bash
pio run -e spike-ble-reader-x3 -t upload
```

- [ ] **Step 2: Stel een dashboard samen met vier datumtegels**

Bouw in de app een dashboard met, naast een gewone KPI-tegel, vier datumtegels die de ladder uitputten: 1×1 op `auto`, 2×1 op `auto`, 1×2 op `auto` en 2×2 op `auto`. Installeer de app op het toestel:

```bash
xcodebuild -project X3DashboardApp.xcodeproj -scheme X3DashboardApp \
  -destination 'id=837B3AD6-71E0-5E45-8CF6-F309BE23C1B5' -derivedDataPath /tmp/x3dd build
```

- [ ] **Step 3: Verstuur en kijk**

Houd `Back + Power` ingedrukt bij het wekken van de X3 om de ontvanger te activeren, en verstuur het dashboard.

Verwacht op het paneel:
- Elke tegel toont de datum van vandaag, in de indeling die bij zijn maat hoort.
- Geen tekst valt buiten een tegel of overlapt een andere regel.
- De weekdag staat voluit waar hij past, en afgekort waar niet.
- Het weeknummer klopt met wat `date "+%V"` op de Mac zegt.

- [ ] **Step 4: Controleer de dagovergang**

Dit is de reden dat de tegel de RTC leest in plaats van de tekst mee te sturen. Laat het toestel staan zonder opnieuw te versturen en kijk de volgende dag: de tegel moet de nieuwe datum tonen. De ontvanger wordt tussen 07:00 en 22:00 elk kwartier wakker (`AgendaWakePolicy.h:14`), dus de eerste wake na 07:00 laat de omslag zien.

- [ ] **Step 5: Meld het resultaat**

Noteer in de handoff wat er op het paneel stond en of de dagovergang klopte. Een tegel die er goed uitziet maar niet omslaat, betekent dat de renderer de klok maar één keer leest of dat de wake niet opnieuw rendert — dat is een echte fout, geen schoonheidsfoutje.

---

## Volgorde en afhankelijkheden

Task 1 → 2 → 3 kunnen achter elkaar. Task 8 (pariteit) vereist 2 en 3. Task 4 vereist 1 en 2. Task 5 vereist 4. Task 6 vereist 3, Task 7 vereist 6. Task 9 en 10 komen als laatste.

Tasks 1, 2, 3 en 8 zijn het wire-formaat en de rekenkunde: dat is het deel dat aan DeepSeek is toebedeeld. Tasks 4, 5, 6 en 7 zijn de renderer en de UI.
