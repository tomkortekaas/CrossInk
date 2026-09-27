// Renders dashboard V3 on the host with the firmware's own Lexend fonts and
// writes a 528x792 PNG per scenario, plus a fit report.
//
// Usage: dashboard-v3-preview <output-directory>
//
// The renderer itself is the target code, unmodified: only the canvas is
// substituted. What this cannot prove is e-ink appearance — ghosting, contrast
// and the panel's own gamma still need a photograph. What it does prove is
// geometry and typography: whether a string fits its box, where the ellipsis
// lands, and whether a subsetted font turns that ellipsis into a black lozenge.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "DashboardV3.h"
#include "DashboardV3Layout.h"
#include "DashboardV3Quotes.h"
#include "DashboardV3Renderer.h"
#include "PreviewCanvas.h"
#include "PreviewFonts.h"
#include "fontIds.h"

namespace {

using dashboard::v3::DashboardV3Package;

template <size_t Size>
void setField(std::array<uint8_t, Size>& field, uint8_t& length, const char* value) {
  const size_t bounded = std::min(std::strlen(value), Size);
  std::memcpy(field.data(), value, bounded);
  length = static_cast<uint8_t>(bounded);
}

// 2026-08-22 19:13 UTC, so the header renders "ZA 22 AUG" like the mock-up.
constexpr uint64_t kMockupGeneratedAt = 1787771580ULL;
// 2026-09-13 11:15 UTC, the demonstration day for the new 8A design.
constexpr uint64_t kAgenda8AGeneratedAt = 1789298100ULL;
constexpr uint16_t kAgenda8AMinuteOfDay = 11 * 60 + 15;

// The scenario the mock-up describes: every section populated with the values
// from docs, at realistic Dutch string lengths.
DashboardV3Package mockupPackage() {
  DashboardV3Package package{};
  package.generatedAt = kMockupGeneratedAt;

  package.weather.currentCelsius = 17;
  package.weather.minimumCelsius = 13;
  package.weather.maximumCelsius = 20;
  package.weather.conditionIconId = 2;  // cloud
  package.weather.windKilometersPerHour = 8;
  package.weather.windDirection = 14;        // compass index, W
  package.weather.sunriseTodayMinute = 395;  // 06:35
  package.weather.sunsetTodayMinute = 1245;  // 20:45
  package.weather.sunriseTomorrowMinute = 397;

  // A real Buienradar nowcast, fetched at 15:20 on 2026-08-28: dry for just
  // over an hour, then light rain building to a downpour at the end. Recorded
  // values rather than invented ones, because the invented ones used to run to
  // 12 on a scale where 3 is already the heaviest band a shower reaches.
  const uint8_t buckets[dashboard::v3::RAIN_BUCKET_COUNT] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                             0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 3, 2};
  std::copy(std::begin(buckets), std::end(buckets), package.rain.begin());
  package.rainStartMinute = 15 * 60 + 20;
  package.heatingKnown = true;
  package.heatingAllowed = true;

  setField(package.traffic.destination, package.traffic.destinationLength, "NAAR WERK");
  package.traffic.travelMinutes = 39;
  package.traffic.nationalCongestionKilometers = 186;
  package.traffic.classification = 0;

  package.status.x3Battery = 30;
  package.status.vehicleBattery = 76;
  package.status.homeBattery = 83;
  package.status.steps = 7850;
  package.status.stepGoal = 10000;

  struct AgendaFixture {
    uint8_t dayOffset;
    uint16_t minute;
    const char* title;
    const char* detail;
  };
  const AgendaFixture agenda[] = {
      {0, 900, "Verjaardag Tom", "Tbos \xc2\xb7 vanaf 15:30"},
      {1, 510, "Siem DWS", ""},
      {1, 540, "Fiberforce Review", "Teams"},
      {1, 600, "Wandelen met Tom", ""},
      {1, 660, "Butterfly", "Maarssen"},
      {1, 720, "Lunch", "Maarssen"},
      {1, 840, "Weekly Innovation Lane", "Teams"},
      {1, 900, "Eva training hockey", ""},
  };
  package.agendaCount = static_cast<uint8_t>(std::size(agenda));
  for (size_t index = 0; index < std::size(agenda); ++index) {
    package.agenda[index].dayOffset = agenda[index].dayOffset;
    package.agenda[index].minuteOfDay = agenda[index].minute;
    setField(package.agenda[index].title, package.agenda[index].titleLength, agenda[index].title);
    setField(package.agenda[index].detail, package.agenda[index].detailLength, agenda[index].detail);
  }

  struct MarketFixture {
    const char* label;
    int16_t basisPoints;
  };
  const MarketFixture markets[] = {{"Portefeuille", 68}, {"All-World", 42}, {"AEX", 31}};
  package.marketCount = static_cast<uint8_t>(std::size(markets));
  for (size_t index = 0; index < std::size(markets); ++index) {
    setField(package.markets[index].label, package.markets[index].labelLength, markets[index].label);
    package.markets[index].changeBasisPoints = markets[index].basisPoints;
  }

  struct ChatFixture {
    const char* name;
    uint16_t unread;
    uint16_t minute;
  };
  const ChatFixture chats[] = {{"Chanel Kortekaas", 1, 498},  {"Richard Vaderman", 2, 1215},
                               {"Luke De Brouwer", 1, 1180},  {"CLUBHOUSE PADEL", 2, 497},
                               {"Singularity Sync", 41, 329}, {"Kwartet", 3, 1138},
                               {"Familie Kortekaas", 4, 955}};
  package.chatCount = static_cast<uint8_t>(std::size(chats));
  for (size_t index = 0; index < std::size(chats); ++index) {
    setField(package.chats[index].name, package.chats[index].nameLength, chats[index].name);
    package.chats[index].unreadCount = chats[index].unread;
    package.chats[index].lastMessageMinuteOfDay = chats[index].minute;
  }
  package.unreadTotal = 24;
  package.quoteId = 1;
  return package;
}

// One market row: the panel must not draw a heading over an empty list.
DashboardV3Package soloMarketPackage() {
  DashboardV3Package package = mockupPackage();
  package.marketCount = 1;
  return package;
}

// The leading figure missing: the block keeps its height so the sections below
// it do not jump when a quote drops out.
DashboardV3Package missingLeadMarketPackage() {
  DashboardV3Package package = mockupPackage();
  package.markets[0].changeBasisPoints = INT16_MIN;
  return package;
}

// Every optional source absent: the layout must hold its geometry and show
// dashes rather than collapsing or inventing values.
DashboardV3Package emptyPackage() {
  DashboardV3Package package{};
  package.generatedAt = kMockupGeneratedAt;
  return package;
}

// The longest content the wire format allows, to find clipping the mock-up
// scenario hides behind comfortable string lengths.
DashboardV3Package maximumPackage() {
  DashboardV3Package package = mockupPackage();
  setField(package.traffic.destination, package.traffic.destinationLength, "NAAR AMSTERDAM Z");
  package.traffic.travelMinutes = 188;
  package.traffic.nationalCongestionKilometers = 1860;
  package.status.steps = 19999;
  for (size_t index = 0; index < dashboard::v3::MAX_AGENDA_ROWS; ++index) {
    setField(package.agenda[index].title, package.agenda[index].titleLength, "Kwartaalreview Fiberforce Nederl");
    setField(package.agenda[index].detail, package.agenda[index].detailLength, "Maarssen \xc2\xb7 Teams call");
  }
  for (size_t index = 0; index < dashboard::v3::MAX_MARKETS; ++index) {
    setField(package.markets[index].label, package.markets[index].labelLength, "All-World ET");
    package.markets[index].changeBasisPoints = -1234;
  }
  for (size_t index = 0; index < dashboard::v3::MAX_CHATS; ++index) {
    setField(package.chats[index].name, package.chats[index].nameLength, "Familie Kortekaas");
    package.chats[index].unreadCount = 128;
  }
  package.unreadTotal = 999;
  return package;
}

// ---------------------------------------------------------------------------
// 8A scenarios
// ---------------------------------------------------------------------------

DashboardV3Package withAgendaDayTotals(DashboardV3Package package) {
  package.formatVersion = dashboard::v3::FORMAT_VERSION_DAY_TOTALS;
  for (size_t index = 0; index < package.agendaCount; ++index) {
    uint8_t total = 0;
    uint8_t allDay = 0;
    for (size_t other = 0; other < package.agendaCount; ++other) {
      if (package.agenda[other].dayOffset != package.agenda[index].dayOffset) continue;
      ++total;
      if (package.agenda[other].isAllDay) ++allDay;
    }
    package.agenda[index].dayTotalCount = total;
    package.agenda[index].dayAllDayCount = allDay;
  }
  return package;
}

// The demonstration day from the 8A design brief: Sunday 2026-09-13 11:15 UTC,
// every band populated with normal values, and one soft agenda block.
DashboardV3Package agenda8APackage() {
  DashboardV3Package package{};
  package.formatVersion = dashboard::v3::FORMAT_VERSION_DAY_TOTALS;
  package.generatedAt = kAgenda8AGeneratedAt;

  package.weather.currentCelsius = 13;
  package.weather.minimumCelsius = 9;
  package.weather.maximumCelsius = 17;
  package.weather.conditionIconId = 3;  // cloud_rain
  package.weather.windKilometersPerHour = 12;
  package.weather.windDirection = 4;  // O
  package.weather.sunriseTodayMinute = 7 * 60 + 12;
  package.weather.sunsetTodayMinute = 19 * 60 + 50;
  package.weather.sunriseTomorrowMinute = 7 * 60 + 13;

  const uint8_t buckets[dashboard::v3::RAIN_BUCKET_COUNT] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                             0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1};
  std::copy(std::begin(buckets), std::end(buckets), package.rain.begin());
  package.rainKnown = true;
  package.rainStartMinute = 11 * 60 + 15;
  package.heatingKnown = true;
  package.heatingAllowed = true;

  setField(package.traffic.destination, package.traffic.destinationLength, "NAAR WERK");
  package.traffic.travelMinutes = 43;
  package.traffic.nationalCongestionKilometers = 128;
  package.traffic.classification = 2;  // FILE

  package.status.x3Battery = 76;
  package.status.vehicleBattery = 49;
  package.status.homeBattery = 23;
  package.status.steps = 7850;
  package.status.stepGoal = 10000;

  package.portfolioChangeBasisPoints = 90;

  struct Agenda8AFixture {
    uint8_t dayOffset;
    uint16_t minute;
    uint16_t durationMinutes;
    const char* title;
    bool isSoftBlock;
  };
  const Agenda8AFixture agenda[] = {
      {0, 19 * 60, 120, "Verhalenhuis 3", false}, {1, 16 * 60, 60, "Theaterles", false},
      {1, 18 * 60 + 30, 90, "Padel", false},      {2, 8 * 60, 120, "Blok", true},
      {2, 8 * 60, 45, "Overleg", false},          {2, 8 * 60, 30, "Koffie", false},
      {2, 9 * 60 + 30, 60, "AI doc", false},
  };
  package.agendaCount = static_cast<uint8_t>(std::size(agenda));
  for (size_t index = 0; index < std::size(agenda); ++index) {
    package.agenda[index].dayOffset = agenda[index].dayOffset;
    package.agenda[index].minuteOfDay = agenda[index].minute;
    package.agenda[index].durationMinutes = agenda[index].durationMinutes;
    package.agenda[index].isSoftBlock = agenda[index].isSoftBlock;
    setField(package.agenda[index].title, package.agenda[index].titleLength, agenda[index].title);
  }
  // The demonstration day's first appointment is a Teams call, so the normal
  // scenario's focus row shows the location-carrying caption the panel draws:
  // "Teams · over 7u45".
  setField(package.agenda[0].detail, package.agenda[0].detailLength, "Teams");

  struct MarketFixture {
    const char* label;
    int16_t basisPoints;
  };
  const MarketFixture markets[] = {{"AEX", 50}, {"S&P", 30}, {"NDX", 70}, {"BTC", 120}};
  package.marketCount = static_cast<uint8_t>(std::size(markets));
  for (size_t index = 0; index < std::size(markets); ++index) {
    setField(package.markets[index].label, package.markets[index].labelLength, markets[index].label);
    package.markets[index].changeBasisPoints = markets[index].basisPoints;
  }

  package.moverCount = 3;
  setField(package.strongestMover.label, package.strongestMover.labelLength, "ASML");
  package.strongestMover.changeBasisPoints = 610;

  struct ChatFixture {
    const char* name;
    uint16_t unread;
  };
  const ChatFixture chats[] = {{"Chanel Kortekaas", 20}, {"P", 3}, {"Familie Kortekaas", 1}};
  package.chatCount = static_cast<uint8_t>(std::size(chats));
  for (size_t index = 0; index < std::size(chats); ++index) {
    setField(package.chats[index].name, package.chats[index].nameLength, chats[index].name);
    package.chats[index].unreadCount = chats[index].unread;
  }
  package.unreadTotal = 24;
  package.quoteId = 1;
  return withAgendaDayTotals(package);
}

// The same demonstration day, plus a Sunday all-day row and one unknown
// duration. The unknown duration keeps its ribbon mark; the all-day row has no
// interval at all.
DashboardV3Package agenda8AAllDayPackage() {
  DashboardV3Package package = agenda8APackage();
  for (int index = package.agendaCount; index > 1; --index) {
    package.agenda[index] = package.agenda[index - 1];
  }
  package.agenda[1] = dashboard::v3::AgendaRow{};
  package.agenda[1].dayOffset = 0;
  package.agenda[1].isAllDay = true;
  package.agenda[1].durationMinutes = UINT16_MAX;
  setField(package.agenda[1].title, package.agenda[1].titleLength, "Feestdag");
  ++package.agendaCount;

  // The original Monday 18:30 "Padel" row shifted from index 2 to index 3.
  package.agenda[3].durationMinutes = UINT16_MAX;
  return withAgendaDayTotals(package);
}

// Format 5 but no readings at all: every missing marker the wire format has.
DashboardV3Package empty8APackage() {
  DashboardV3Package package{};
  package.formatVersion = dashboard::v3::FORMAT_VERSION_DAY_TOTALS;
  package.generatedAt = kAgenda8AGeneratedAt;
  package.rainKnown = false;
  package.rainStartMinute = UINT16_MAX;
  package.heatingKnown = false;
  package.portfolioChangeBasisPoints = INT16_MIN;
  return withAgendaDayTotals(package);
}

// Six appointments today (focus row plus five rows) and seven tomorrow. Today fits
// in full; tomorrow gives up its final three rows to the standalone summary.
DashboardV3Package agenda8AFillPackage() {
  DashboardV3Package package = agenda8APackage();
  package.agendaCount = dashboard::v3::MAX_AGENDA_ROWS;
  static const char* const titles[] = {
      "AI doc doornemen SG", "Weekly Innovation Lane Meeting", "Butterfly", "Lunch", "Serious Lego Session",
      "Demo voorbereiden", "AI Policy", "blok", "Klantgesprek", "Roadmap", "Review", "Sport", "Avondeten"};
  for (size_t index = 0; index < std::size(titles); ++index) {
    dashboard::v3::AgendaRow& row = package.agenda[index];
    row = dashboard::v3::AgendaRow{};
    row.dayOffset = index < 6 ? 0 : 1;
    row.minuteOfDay = static_cast<uint16_t>((index < 6 ? 9 * 60 + 30 : 9 * 60) + (index % 6) * 60);
    row.durationMinutes = index == 7 ? 90 : 60;
    row.isSoftBlock = index == 7;
    setField(row.title, row.titleLength, titles[index]);
  }
  return withAgendaDayTotals(package);
}

// A half-populated format-5 package: enough to draw every band, with a missing
// value in each band that supports one.
DashboardV3Package missing8APackage() {
  DashboardV3Package package = agenda8APackage();
  for (size_t index = 0; index < package.agendaCount; ++index) {
    package.agenda[index].durationMinutes = UINT16_MAX;
  }
  package.agenda[0].isAllDay = true;

  package.markets[0].changeBasisPoints = INT16_MIN;
  package.markets[2].changeBasisPoints = INT16_MIN;
  package.moverCount = 0;
  package.strongestMover = dashboard::v3::MarketRow{};

  package.traffic.travelMinutes = UINT16_MAX;
  package.status.steps = UINT16_MAX;
  package.portfolioChangeBasisPoints = INT16_MIN;
  package.rainKnown = true;
  package.rainStartMinute = UINT16_MAX;
  package.heatingKnown = false;
  return withAgendaDayTotals(package);
}

// The demonstration day without the mover block.
DashboardV3Package noMovers8APackage() {
  DashboardV3Package package = agenda8APackage();
  package.moverCount = 0;
  package.strongestMover = dashboard::v3::MarketRow{};
  return withAgendaDayTotals(package);
}

// The demonstration day with the stove verdict withheld: the heating badge draws
// nothing at all rather than a struck-through flame or a word.
DashboardV3Package noStove8APackage() {
  DashboardV3Package package = agenda8APackage();
  package.heatingKnown = false;
  return withAgendaDayTotals(package);
}

// The demonstration day with the stove forbidden: the same flame plus a strong
// diagonal strike, and no text.
DashboardV3Package noHeating8APackage() {
  DashboardV3Package package = agenda8APackage();
  package.heatingAllowed = false;
  return withAgendaDayTotals(package);
}

// One, six and eleven appointments on a single day: the row counts the approval
// named, so the zebra pattern and the band's fit can be inspected at each.
DashboardV3Package appointmentCount8APackage(const size_t count) {
  DashboardV3Package package{};
  package.formatVersion = dashboard::v3::FORMAT_VERSION_DAY_TOTALS;
  package.generatedAt = kAgenda8AGeneratedAt;
  for (size_t index = 0; index < count; ++index) {
    const std::string title = "AFSPRAAK " + std::to_string(index + 1);
    package.agenda[index].dayOffset = 0;
    package.agenda[index].minuteOfDay = static_cast<uint16_t>(9 * 60 + index * 30);
    package.agenda[index].durationMinutes = 30;
    setField(package.agenda[index].title, package.agenda[index].titleLength, title.c_str());
  }
  package.agendaCount = static_cast<uint8_t>(count);
  return withAgendaDayTotals(package);
}

// The demonstration day with a full-width wire title in the focus row and in an
// ordinary row: the longest thing those two boxes ever have to set.
DashboardV3Package longTitle8APackage() {
  DashboardV3Package package = agenda8APackage();
  const char* const longTitle = "Kwartaalreview Fiberforce Nederl";
  setField(package.agenda[0].title, package.agenda[0].titleLength, longTitle);  // the focus row
  setField(package.agenda[1].title, package.agenda[1].titleLength, longTitle);  // Monday's first row
  return withAgendaDayTotals(package);
}

// A double booking: two appointments starting at the same minute, both of which
// must keep their own row and their own clock.
DashboardV3Package sameStart8APackage() {
  DashboardV3Package package{};
  package.formatVersion = dashboard::v3::FORMAT_VERSION_DAY_TOTALS;
  package.generatedAt = kAgenda8AGeneratedAt;
  struct Fixture {
    uint16_t minute;
    uint16_t durationMinutes;
    const char* title;
  };
  const Fixture rows[] = {{19 * 60, 60, "Eerste"}, {19 * 60, 30, "Tweede"}};
  for (size_t index = 0; index < std::size(rows); ++index) {
    package.agenda[index].dayOffset = 0;
    package.agenda[index].minuteOfDay = rows[index].minute;
    package.agenda[index].durationMinutes = rows[index].durationMinutes;
    setField(package.agenda[index].title, package.agenda[index].titleLength, rows[index].title);
  }
  package.agendaCount = static_cast<uint8_t>(std::size(rows));
  return withAgendaDayTotals(package);
}

// The wire maximums: longest labels and durations, biggest counts, and negative
// changes at the edge of the signed bar's range.
DashboardV3Package extreme8APackage() {
  DashboardV3Package package = agenda8APackage();

  package.agendaCount = dashboard::v3::MAX_AGENDA_ROWS;
  for (size_t index = 0; index < dashboard::v3::MAX_AGENDA_ROWS; ++index) {
    package.agenda[index] = dashboard::v3::AgendaRow{};
    package.agenda[index].dayOffset = static_cast<uint8_t>(index / 2);
    package.agenda[index].minuteOfDay = index % 2 == 0 ? 8 * 60 : 20 * 60;
    package.agenda[index].durationMinutes = 1440;
    setField(package.agenda[index].title, package.agenda[index].titleLength, "Kwartaalreview Fiberforce Nederl");
  }

  package.marketCount = dashboard::v3::MAX_MARKETS;
  for (size_t index = 0; index < dashboard::v3::MAX_MARKETS; ++index) {
    package.markets[index] = dashboard::v3::MarketRow{};
    setField(package.markets[index].label, package.markets[index].labelLength, "All-World ET");
    package.markets[index].changeBasisPoints = -1234;
  }

  package.moverCount = 8;
  package.strongestMover = dashboard::v3::MarketRow{};
  setField(package.strongestMover.label, package.strongestMover.labelLength, "All-World ET");
  package.strongestMover.changeBasisPoints = -9999;

  package.status.steps = 19999;
  setField(package.traffic.destination, package.traffic.destinationLength, "NAAR AMSTERDAM Z");
  package.traffic.travelMinutes = 188;
  package.traffic.nationalCongestionKilometers = 1860;
  package.unreadTotal = 999;
  package.portfolioChangeBasisPoints = -800;
  package.quoteId = 0;

  package.chatCount = dashboard::v3::MAX_CHATS;
  for (size_t index = 0; index < dashboard::v3::MAX_CHATS; ++index) {
    package.chats[index] = dashboard::v3::ChatRow{};
    setField(package.chats[index].name, package.chats[index].nameLength, "Familie Kortekaas");
    package.chats[index].unreadCount = 128;
  }
  return withAgendaDayTotals(package);
}

struct Scenario {
  const char* name;
  DashboardV3Package package;
  uint16_t minuteOfDay;
  bool normal = false;
};

// The font ladder in numbers: nominal name, ascender (which is the box height
// drawText reserves above the baseline), line height, and the width of a few
// strings the dashboard actually draws. Picking a rung by its nominal size is
// how V3 ended up with text that overflows every box it sits in.
// Every quote in the shared table, measured against the footer boxes it has to
// live in. A quote is chosen by the calendar day, so a single entry that is too
// long shows up as a truncated footer once every few days and nowhere else.
// Returns non-zero when any text or author overflows, so a table that cannot be
// drawn in full fails the preview run instead of only printing a count.
int reportQuoteFit(const dashboard::preview::FontBook& fonts) {
  constexpr int kQuoteWidth = 528 - 2 * 14;        // footer width minus padding
  constexpr int kAuthorWidth = kQuoteWidth - 140;  // shares its line with "ververst HH:MM"

  std::printf("\n=== quote table (%zu entries)\n", dashboard::v3::QUOTE_COUNT);
  int overflowing = 0;
  for (size_t index = 0; index < dashboard::v3::QUOTE_COUNT; ++index) {
    const dashboard::v3::Quote& quote = dashboard::v3::kQuotes[index];
    char author[64];
    std::snprintf(author, sizeof(author), "- %s", quote.author);
    // Mirrors the renderer's rung choice, so this reports what the panel draws
    // rather than what it would draw if every quote used the Body rung.
    const bool stepsDown = std::strlen(quote.text) > 44;
    const int quoteFontId = stepsDown ? LEXENDDECA_8_FONT_ID : LEXENDDECA_10_FONT_ID;
    const int textWidth = fonts.textWidth(quoteFontId, quote.text, EpdFontFamily::BOLD);
    const int authorWidth = fonts.textWidth(LEXENDDECA_8_FONT_ID, author, EpdFontFamily::REGULAR);
    const bool fits = textWidth <= kQuoteWidth && authorWidth <= kAuthorWidth;
    if (!fits) ++overflowing;
    std::printf("  %-4s id=%zu %-5s %4d/%d px  %4d/%d px  %s\n", fits ? "ok" : "OVER", index,
                stepsDown ? "micro" : "body", textWidth, kQuoteWidth, authorWidth, kAuthorWidth, quote.text);
  }
  std::printf("  %d of %zu quotes overflow their box\n", overflowing, dashboard::v3::QUOTE_COUNT);
  if (overflowing > 0) {
    std::fprintf(stderr, "quote fit: %d of %zu quotes overflow their box\n", overflowing, dashboard::v3::QUOTE_COUNT);
    return 1;
  }
  return 0;
}

void reportFontLadder(const dashboard::preview::FontBook& fonts) {
  struct Rung {
    const char* name;
    int fontId;
    EpdFontFamily::Style style;
  };
  const Rung ladder[] = {
      {"Micro (Lx8)", LEXENDDECA_8_FONT_ID, EpdFontFamily::BOLD},
      {"Lexend 9 bold", LEXENDDECA_9_FONT_ID, EpdFontFamily::BOLD},
      {"Lexend 10", LEXENDDECA_10_FONT_ID, EpdFontFamily::REGULAR},
      {"Lexend 10 bold", LEXENDDECA_10_FONT_ID, EpdFontFamily::BOLD},
      {"Lexend 12", LEXENDDECA_12_FONT_ID, EpdFontFamily::REGULAR},
      {"Lexend 12 bold", LEXENDDECA_12_FONT_ID, EpdFontFamily::BOLD},
      {"Lexend 14 bold", LEXENDDECA_14_FONT_ID, EpdFontFamily::BOLD},
      {"Lexend 16 bold", LEXENDDECA_16_FONT_ID, EpdFontFamily::BOLD},
      {"Lexend 18 bold", LEXENDDECA_18_FONT_ID, EpdFontFamily::BOLD},
      {"Lexend 20 bold", LEXENDDECA_20_FONT_ID, EpdFontFamily::BOLD},
      {"Lexend 18 dash", LEXENDDECA_18_BOLD_DASH_FONT_ID, EpdFontFamily::BOLD},
      {"Lexend 22 dash", LEXENDDECA_22_BOLD_DASH_FONT_ID, EpdFontFamily::BOLD},
      {"Lexend 28 dash", LEXENDDECA_28_BOLD_DASH_FONT_ID, EpdFontFamily::BOLD},
      {"Lexend 34 dash", LEXENDDECA_34_BOLD_DASH_FONT_ID, EpdFontFamily::BOLD},
  };

  std::printf("\n=== font ladder (528px wide panel)\n");
  std::printf("  %-15s %4s %4s  %5s %6s %7s %8s\n", "font", "asc", "line", "\"00:00\"", "\"100%\"", "\"STATUS\"",
              "\"Fiberforce Review\"");
  for (const Rung& rung : ladder) {
    std::printf("  %-15s %4d %4d  %5d %6d %7d %8d\n", rung.name, fonts.ascender(rung.fontId),
                fonts.lineHeight(rung.fontId), fonts.textWidth(rung.fontId, "00:00", rung.style),
                fonts.textWidth(rung.fontId, "100%", rung.style), fonts.textWidth(rung.fontId, "STATUS", rung.style),
                fonts.textWidth(rung.fontId, "Fiberforce Review", rung.style));
  }
}

template <size_t Size>
std::string copiedFieldString(const std::array<uint8_t, Size>& field, const uint8_t length) {
  return std::string(reinterpret_cast<const char*>(field.data()), std::min<size_t>(length, Size));
}

std::string uppercaseAsciiString(std::string text) {
  for (char& character : text) {
    if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
  }
  return text;
}

std::string signedPercentString(const int16_t basisPoints) {
  const int magnitude = std::abs(static_cast<int>(basisPoints));
  const int tenths = (magnitude + 5) / 10;
  char out[16];
  std::snprintf(out, sizeof(out), "%c%d,%d%%", basisPoints < 0 ? '-' : '+', tenths / 10, tenths % 10);
  return out;
}

std::string batteryPercentString(const uint8_t value) {
  if (value == UINT8_MAX) return "-";
  char out[8];
  std::snprintf(out, sizeof(out), "%u%%", static_cast<unsigned>(value));
  return out;
}

std::string stepsString(const uint16_t steps) {
  if (steps == UINT16_MAX) return "-";
  if (steps < 1000) return std::to_string(steps);
  char out[16];
  std::snprintf(out, sizeof(out), "%u.%03u", static_cast<unsigned>(steps / 1000), static_cast<unsigned>(steps % 1000));
  return out;
}

std::string minuteString(const uint16_t minute) {
  char out[8];
  std::snprintf(out, sizeof(out), "%02u:%02u", static_cast<unsigned>(minute / 60), static_cast<unsigned>(minute % 60));
  return out;
}

void report8AFocusRow(const DashboardV3Package& package, const dashboard::preview::FontBook& fonts) {
  const dashboard::v3::Dashboard8ARects bands = dashboard::v3::computeDashboard8ALayout(528, 792, {});
  const dashboard::v3::Agenda8ARects agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
  // The focus row is the first day group's first list entry, after its ribbon.
  const dashboard::v3::Rect rect{bands.agenda.x,
                                 agenda.content.y + agenda.headingHeight + agenda.ribbonHeight + agenda.ribbonGap,
                                 bands.agenda.width, agenda.heroHeight};
  const int left = rect.x + dashboard::v3::DASHBOARD8A_PAD;
  const int right = rect.x + rect.width - dashboard::v3::DASHBOARD8A_PAD;
  std::printf("  8A focus row ");
  if (package.agendaCount == 0) {
    std::printf("no agenda rows\n");
    return;
  }
  const dashboard::v3::AgendaRow& row = package.agenda[0];
  const std::string big = row.isAllDay ? "hele dag" : minuteString(row.minuteOfDay);
  const int measured = fonts.textWidth(LEXENDDECA_16_FONT_ID, big.c_str(), EpdFontFamily::BOLD);
  const int capped = std::min(std::max(8, measured), (right - left) / 2);
  const int ruleX = left + capped + 14;
  std::printf("time=%-8s measured=%dpx capped=%dpx hairlineX=%d\n", big.c_str(), measured, capped, ruleX);
  // The caption's own text is the renderer's decision, so the report only prints
  // the location the caption leads with; the picture shows the finished line.
  const std::string location(reinterpret_cast<const char*>(row.detail.data()),
                             std::min<size_t>(row.detailLength, dashboard::v3::MAX_AGENDA_DETAIL_BYTES));
  std::printf("               location=\"%s\"\n", location.c_str());
}

void report8AKpi(const DashboardV3Package& package, const dashboard::preview::FontBook& fonts) {
  const dashboard::v3::Dashboard8ARects bands = dashboard::v3::computeDashboard8ALayout(528, 792, {});
  constexpr int kPad = dashboard::v3::DASHBOARD8A_PAD;
  constexpr int bodyFontId = LEXENDDECA_10_FONT_ID;
  const auto measure = [&fonts](const std::string& value) {
    return fonts.textWidth(bodyFontId, value.c_str(), EpdFontFamily::BOLD);
  };

  struct KpiMeasure {
    const char* name;
    std::string value;
  };
  const std::string x3 = batteryPercentString(package.status.x3Battery);
  const std::string vehicle = batteryPercentString(package.status.vehicleBattery);
  const std::string home = batteryPercentString(package.status.homeBattery);
  const std::string portfolio =
      package.portfolioChangeBasisPoints == INT16_MIN ? "-" : signedPercentString(package.portfolioChangeBasisPoints);

  std::printf("  8A kpi       value boxes (Body bold)\n");
  const dashboard::v3::Rect leftColumn = bands.kpiLeft;
  // The left column draws no captions any more: icon, value and a 10 px bar
  // share one row, and the bars all start at the same x. The tags below name the
  // row's source, not a string on the panel.
  const KpiMeasure leftRows[] = {{"x3", x3}, {"car", vehicle}, {"home", home}, {"port", portfolio}};
  const int leftLeft = leftColumn.x + kPad;
  const int leftRight = leftColumn.x + leftColumn.width - kPad;
  const int leftValueX = leftLeft + 26;
  int reservedValueWidth = 0;
  for (const KpiMeasure& row : leftRows) reservedValueWidth = std::max(reservedValueWidth, measure(row.value));
  int barX = leftValueX + reservedValueWidth + 8;
  int barWidth = leftRight - barX;
  if (barWidth < 8) {
    barWidth = 8;
    barX = leftRight - barWidth;
  }
  for (const KpiMeasure& row : leftRows) {
    const int measured = measure(row.value);
    std::printf("  8A kpi L     %-4s value=%-10s measured=%3dpx reserved=%3dpx -> %s\n", row.name, row.value.c_str(),
                measured, reservedValueWidth, measured <= reservedValueWidth ? "fits" : "OVER");
  }
  std::printf("  8A kpi L     bars x=%d width=%dpx (all four aligned)\n", barX, barWidth);

  std::string travel = "-";
  if (package.traffic.travelMinutes != UINT16_MAX) {
    travel = std::to_string(package.traffic.travelMinutes) + " min";
  }
  std::string destination = "REISTIJD";
  if (package.traffic.destinationLength > 0) {
    destination =
        uppercaseAsciiString(copiedFieldString(package.traffic.destination, package.traffic.destinationLength));
  }
  std::string congestion = "-";
  if (package.traffic.nationalCongestionKilometers != UINT16_MAX) {
    congestion = std::to_string(package.traffic.nationalCongestionKilometers) + " km";
  }
  static const char* const kClassification[3] = {"NORMAAL", "DRUK", "FILE"};
  const char* classification =
      package.traffic.classification < 3 ? kClassification[package.traffic.classification] : "FILES";
  std::string chats = "-";
  if (package.chatCount > 0) {
    chats = copiedFieldString(package.chats[0].name, package.chats[0].nameLength);
    if (package.chatCount > 1) {
      chats += ", " + copiedFieldString(package.chats[1].name, package.chats[1].nameLength);
    }
  }
  const std::string unread = std::to_string(package.unreadTotal);
  const std::string steps = stepsString(package.status.steps);

  const dashboard::v3::Rect rightColumn = bands.kpiRight;
  const int rightLeft = rightColumn.x + kPad;
  const int rightRight = rightColumn.x + rightColumn.width - kPad;
  const int valueX = rightLeft + 26;
  const int maxBox = rightRight - valueX - 40;
  const KpiMeasure rightRows[] = {
      {destination.c_str(), travel}, {classification, congestion}, {chats.c_str(), unread}, {"STAPPEN", steps}};
  for (const KpiMeasure& row : rightRows) {
    const int measured = measure(row.value);
    const int box = std::clamp(measured, 16, maxBox);
    std::printf("  8A kpi R     %-7s value=%-14s measured=%3dpx box=%3dpx -> %s\n", row.name, row.value.c_str(),
                measured, box, measured <= box ? "fits" : "OVER");
  }
}

void report8AMarkets(const DashboardV3Package& package, const dashboard::preview::FontBook& fonts) {
  const dashboard::v3::Dashboard8ARects bands = dashboard::v3::computeDashboard8ALayout(528, 792, {});
  // The market line uses its own 10 px pad and 4 px gaps, not the standard 8A
  // 14 px pad. Keep these in step with MARKETS_8A_PAD / MARKETS_8A_GAP in
  // DashboardV3Renderer.cpp.
  constexpr int kPad = 10;
  constexpr int kGap = 4;
  constexpr int kLabelChangeGap = 3;
  const int left = bands.markets.x + kPad;
  const int right = bands.markets.x + bands.markets.width - kPad;
  const int width = right - left;
  std::printf("  8A markets   band=%dpx (%d..%d)\n", width, left, right);

  struct MarketMeasure {
    std::string label;
    std::string change;
    int labelWidth = 0;
    int changeWidth = 0;
  };
  // The four primary indices are a fixed row of gauges; a slot the package did
  // not fill still draws its label with a dash for the change.
  static constexpr const char* kIndexLabels[dashboard::v3::MAX_MARKETS] = {"AEX", "S&P", "NDX", "BTC"};
  const int indexCount = static_cast<int>(dashboard::v3::MAX_MARKETS);
  std::vector<MarketMeasure> blocks;
  blocks.reserve(static_cast<size_t>(indexCount));
  for (int index = 0; index < indexCount; ++index) {
    MarketMeasure block;
    block.label = kIndexLabels[index];
    const int16_t change = index < static_cast<int>(package.marketCount) ? package.markets[index].changeBasisPoints
                                                                         : INT16_MIN;
    block.change = change == INT16_MIN ? "-" : signedPercentString(change);
    block.labelWidth = fonts.textWidth(LEXENDDECA_8_FONT_ID, block.label.c_str(), EpdFontFamily::BOLD);
    block.changeWidth = fonts.textWidth(LEXENDDECA_8_FONT_ID, block.change.c_str(), EpdFontFamily::BOLD);
    blocks.push_back(std::move(block));
  }

  int used = 0;
  for (size_t index = 0; index < blocks.size(); ++index) {
    used += blocks[index].labelWidth + kLabelChangeGap + blocks[index].changeWidth;
    if (index > 0) used += kGap;
  }

  bool clamped = false;
  if (used > width && indexCount > 0) {
    const int blockWidth = (width - (indexCount - 1) * kGap) / indexCount;
    clamped = true;
    for (MarketMeasure& block : blocks) {
      block.changeWidth = std::min(block.changeWidth, std::max(0, blockWidth - kLabelChangeGap));
      block.labelWidth = std::max(0, blockWidth - kLabelChangeGap - block.changeWidth);
    }
    used = 0;
    for (size_t index = 0; index < blocks.size(); ++index) {
      used += blocks[index].labelWidth + kLabelChangeGap + blocks[index].changeWidth;
      if (index > 0) used += kGap;
    }
  }

  for (size_t index = 0; index < blocks.size(); ++index) {
    const MarketMeasure& block = blocks[index];
    const int blockWidth = block.labelWidth + kLabelChangeGap + block.changeWidth;
    std::printf("  8A markets   idx%zu %-12s %-8s label=%3dpx change=%3dpx block=%3dpx\n", index, block.label.c_str(),
                block.change.c_str(), block.labelWidth, block.changeWidth, blockWidth);
  }
  std::printf("  8A markets   indices used=%dpx%s\n", used, clamped ? " (equal blocks)" : "");

  const int change = package.strongestMover.changeBasisPoints;
  const bool hasMover = package.moverCount > 0 && package.strongestMover.labelLength > 0 && change != INT16_MIN &&
                        std::abs(static_cast<int>(change)) > 300;
  if (hasMover) {
    std::string moverLabel =
        uppercaseAsciiString(copiedFieldString(package.strongestMover.label, package.strongestMover.labelLength));
    std::string moverChange = signedPercentString(static_cast<int16_t>(change));
    std::string moverExtra;
    if (package.moverCount > 1) {
      moverExtra = "+" + std::to_string(package.moverCount - 1);
    }
    const int moverLabelWidth = fonts.textWidth(LEXENDDECA_8_FONT_ID, moverLabel.c_str(), EpdFontFamily::BOLD);
    const int moverChangeWidth = fonts.textWidth(LEXENDDECA_8_FONT_ID, moverChange.c_str(), EpdFontFamily::BOLD);
    const int moverExtraWidth =
        moverExtra.empty() ? 0 : fonts.textWidth(LEXENDDECA_8_FONT_ID, moverExtra.c_str(), EpdFontFamily::BOLD);
    const int dividerWidth = 2 * kGap + 1;
    const int moverWidth = moverLabelWidth + kLabelChangeGap + moverChangeWidth;
    const bool withExtra =
        moverExtraWidth > 0 && used + dividerWidth + moverWidth + kLabelChangeGap + moverExtraWidth <= width;
    const bool withMover = withExtra || used + dividerWidth + moverWidth <= width;
    if (withMover) {
      const int moverBlock = moverWidth + (withExtra ? kLabelChangeGap + moverExtraWidth : 0);
      used += dividerWidth + moverBlock;
      std::printf(
          "  8A markets   mover %-12s %-8s %-4s label=%3dpx change=%3dpx extra=%3dpx divider=%dpx block=%3dpx\n",
          moverLabel.c_str(), moverChange.c_str(), moverExtra.c_str(), moverLabelWidth, moverChangeWidth,
          moverExtraWidth, dividerWidth, moverBlock);
    } else {
      std::printf(
          "  8A markets   mover %-12s %-8s %-4s suppressed (needs %dpx more)\n", moverLabel.c_str(),
          moverChange.c_str(), moverExtra.c_str(),
          used + dividerWidth + moverWidth + (moverExtraWidth > 0 ? kLabelChangeGap + moverExtraWidth : 0) - width);
    }
  } else {
    std::printf("  8A markets   mover none\n");
  }

  std::printf("  8A markets   line used=%dpx available=%dpx -> %s\n", used, width, used <= width ? "fits" : "OVER");
}

// The rain band is one row: icon, outlook, the window's start clock, strip, end
// clock, and the single heating badge. This mirrors renderRain8A's placement so
// the report shows the strip width the panel actually gets and whether the
// outlook had to drop its leading clock.
void report8ARain(const DashboardV3Package& package, const dashboard::preview::FontBook& fonts) {
  const dashboard::v3::Dashboard8ARects bands = dashboard::v3::computeDashboard8ALayout(528, 792, {});
  const dashboard::v3::Rect rect = bands.rain;
  constexpr int kIcon = 24;
  constexpr int kIconGap = 6;
  constexpr int kItemGap = 8;
  constexpr int kStripMin = 96;
  // Keep in step with renderRain8A: RAIN_BUCKET_COUNT * RAIN_MINUTES_PER_BUCKET.
  constexpr int kWindowMinutes = 24 * 5;
  const int left = rect.x + dashboard::v3::DASHBOARD8A_PAD;
  const int right = rect.x + rect.width - dashboard::v3::DASHBOARD8A_PAD;

  const auto microWidth = [&fonts](const std::string& value) {
    return fonts.textWidth(LEXENDDECA_8_FONT_ID, value.c_str(), EpdFontFamily::BOLD);
  };
  char message[40];
  dashboard::v3::formatRainLine(package, message);
  const bool hasRainData = package.rainKnown && package.rainStartMinute != UINT16_MAX;

  // The heating badge reserves its 24 px slot whether or not the package carries
  // a verdict, so the strip and the clocks keep one place from package to package.
  const int heatingX = right - kIcon;
  std::string startLabel;
  std::string endLabel;
  int startLabelWidth = 0;
  int endLabelWidth = 0;
  if (hasRainData) {
    startLabel = minuteString(package.rainStartMinute);
    endLabel = minuteString(static_cast<uint16_t>((package.rainStartMinute + kWindowMinutes) % 1440));
    startLabelWidth = std::max(8, microWidth(startLabel));
    endLabelWidth = std::max(8, microWidth(endLabel));
  }
  const int endTimeX = heatingX - kItemGap - endLabelWidth;
  const int stripRight = endTimeX - kItemGap;
  const int messageX = left + kIcon + kIconGap;
  const int clockSlot = hasRainData ? startLabelWidth + kItemGap : 0;
  const int messageBudget = std::max(0, stripRight - kStripMin - kItemGap - clockSlot - messageX);
  const int messageWidth = std::max(8, microWidth(message));
  const std::string terse = std::strlen(message) > 6 && message[2] == ':' && message[5] == ' ' ? message + 6 : "";
  const bool useTerse = messageWidth > messageBudget && !terse.empty();
  const int drawnWidth = useTerse ? std::max(8, microWidth(terse)) : messageWidth;
  const int stripLeft = messageX + drawnWidth + kItemGap + clockSlot;
  const int stripWidth = hasRainData ? stripRight - stripLeft : 0;

  std::printf("  8A rain      outlook=\"%s\"%s width=%3dpx budget=%3dpx -> %s\n", message,
              useTerse ? " (clock dropped)" : "", messageWidth, messageBudget,
              messageWidth <= messageBudget ? "fits" : "shortened");
  if (hasRainData) {
    std::printf("  8A rain      window=%s..%s start=%3dpx end=%3dpx strip=%3dpx (min %dpx) -> %s\n", startLabel.c_str(),
                endLabel.c_str(), startLabelWidth, endLabelWidth, stripWidth, kStripMin,
                stripWidth >= 8 ? "fits" : "OVER");
  } else {
    std::printf("  8A rain      no forecast: outlook only, no strip\n");
  }
  const char* const heating = !package.heatingKnown    ? "unknown: nothing drawn"
                              : package.heatingAllowed ? "flame"
                                                       : "flame struck through";
  std::printf("  8A rain      heating badge=%s (one icon, no text)\n", heating);
}

void report8AMeasurements(const Scenario& scenario, const dashboard::preview::FontBook& fonts) {
  std::printf("  [8A measurements]\n");
  report8ARain(scenario.package, fonts);
  report8AFocusRow(scenario.package, fonts);
  report8AKpi(scenario.package, fonts);
  report8AMarkets(scenario.package, fonts);
}

int reportScenario(const Scenario& scenario, const std::string& outputDirectory,
                   const dashboard::preview::FontBook& fonts) {
  dashboard::preview::Framebuffer framebuffer(528, 792);
  dashboard::preview::PreviewCanvas canvas(framebuffer, fonts);
  dashboard::v3::renderDashboardV3(canvas, scenario.package, scenario.minuteOfDay);

  const std::string path = outputDirectory + "/dashboard-v3-" + scenario.name + ".png";
  if (!framebuffer.writePng(path)) {
    std::fprintf(stderr, "failed to write %s\n", path.c_str());
    return 1;
  }

  int truncatedCount = 0;
  int missingGlyphCount = 0;
  std::printf("\n=== %s -> %s\n", scenario.name, path.c_str());
  for (const auto& observation : canvas.observations()) {
    if (!observation.truncated && !observation.missingGlyph) continue;
    if (observation.truncated) ++truncatedCount;
    if (observation.missingGlyph) ++missingGlyphCount;
    std::printf("  %-12s %-34s -> %-34s %4dpx in %4dpx%s\n", observation.truncated ? "TRUNCATED" : "GLYPH",
                observation.requested.c_str(), observation.drawn.c_str(), observation.measuredWidth,
                observation.boundsWidth, observation.missingGlyph ? "  [MISSING GLYPH]" : "");
  }
  std::printf("  %zu strings, %d truncated, %d with a replacement box\n", canvas.observations().size(), truncatedCount,
              missingGlyphCount);
  int result = 0;
  if (scenario.normal) {
    if (truncatedCount == 0) {
      std::printf("  verdict: normal scenario has ZERO truncated strings\n");
    } else {
      std::printf("  verdict: FAIL normal scenario has %d truncated string%s\n", truncatedCount,
                  truncatedCount == 1 ? "" : "s");
      result = 1;
    }
  }
  if (scenario.package.formatVersion >= dashboard::v3::FORMAT_VERSION_8A) {
    report8AMeasurements(scenario, fonts);
  }
  return result;
}

}  // namespace

int main(const int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
    return 2;
  }
  const std::string outputDirectory = argv[1];
  const dashboard::preview::FontBook fonts;
  reportFontLadder(fonts);
  int status = reportQuoteFit(fonts);

  const std::vector<Scenario> scenarios = {
      {"mockup", mockupPackage(), 1153},  // 19:13, so the header shows tonight's sunset
      {"empty", emptyPackage(), 1153},
      {"maximum", maximumPackage(), 1153},
      {"before-sunrise", mockupPackage(), 300},  // 05:00
      {"solo-market", soloMarketPackage(), 1153},
      {"missing-lead-market", missingLeadMarketPackage(), 1153},
      {"agenda-8a", agenda8APackage(), kAgenda8AMinuteOfDay, true},
      {"agenda-8a-fill", agenda8AFillPackage(), kAgenda8AMinuteOfDay, true},
      {"agenda-8a-allday", agenda8AAllDayPackage(), kAgenda8AMinuteOfDay, true},
      {"agenda-8a-one", appointmentCount8APackage(1), kAgenda8AMinuteOfDay, true},
      {"agenda-8a-six", appointmentCount8APackage(6), kAgenda8AMinuteOfDay, true},
      {"agenda-8a-eleven", appointmentCount8APackage(11), kAgenda8AMinuteOfDay, true},
      {"agenda-8a-long-title", longTitle8APackage(), kAgenda8AMinuteOfDay, true},
      {"agenda-8a-same-start", sameStart8APackage(), kAgenda8AMinuteOfDay, true},
      {"agenda-8a-no-stove", noStove8APackage(), kAgenda8AMinuteOfDay, true},
      {"agenda-8a-no-heating", noHeating8APackage(), kAgenda8AMinuteOfDay, true},
      {"empty-8a", empty8APackage(), kAgenda8AMinuteOfDay},
      {"missing-8a", missing8APackage(), kAgenda8AMinuteOfDay},
      {"no-movers-8a", noMovers8APackage(), kAgenda8AMinuteOfDay, true},
      {"extreme-8a", extreme8APackage(), kAgenda8AMinuteOfDay},
  };

  for (const Scenario& scenario : scenarios) {
    status |= reportScenario(scenario, outputDirectory, fonts);
  }
  return status;
}
