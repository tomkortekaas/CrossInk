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
#include <cstring>
#include <string>
#include <vector>

#include "DashboardV3.h"
#include "DashboardV3Renderer.h"
#include "PreviewCanvas.h"
#include "PreviewFonts.h"
#include "DashboardV3Quotes.h"
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
  package.weather.windDirection = 14;  // compass index, W
  package.weather.sunriseTodayMinute = 395;   // 06:35
  package.weather.sunsetTodayMinute = 1245;   // 20:45
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
  const ChatFixture chats[] = {{"Chanel Kortekaas", 1, 498}, {"Richard Vaderman", 2, 1215},
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

struct Scenario {
  const char* name;
  DashboardV3Package package;
  uint16_t minuteOfDay;
};

// The font ladder in numbers: nominal name, ascender (which is the box height
// drawText reserves above the baseline), line height, and the width of a few
// strings the dashboard actually draws. Picking a rung by its nominal size is
// how V3 ended up with text that overflows every box it sits in.
// Every quote in the shared table, measured against the footer boxes it has to
// live in. A quote is chosen by the calendar day, so a single entry that is too
// long shows up as a truncated footer once every few days and nowhere else.
void reportQuoteFit(const dashboard::preview::FontBook& fonts) {
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
                fonts.textWidth(rung.fontId, "100%", rung.style),
                fonts.textWidth(rung.fontId, "STATUS", rung.style),
                fonts.textWidth(rung.fontId, "Fiberforce Review", rung.style));
  }
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
  std::printf("  %zu strings, %d truncated, %d with a replacement box\n", canvas.observations().size(),
              truncatedCount, missingGlyphCount);
  return 0;
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
  reportQuoteFit(fonts);

  const std::vector<Scenario> scenarios = {
      {"mockup", mockupPackage(), 1153},   // 19:13, so the header shows tonight's sunset
      {"empty", emptyPackage(), 1153},
      {"maximum", maximumPackage(), 1153},
      {"before-sunrise", mockupPackage(), 300},  // 05:00
      {"solo-market", soloMarketPackage(), 1153},
      {"missing-lead-market", missingLeadMarketPackage(), 1153},
  };

  int status = 0;
  for (const Scenario& scenario : scenarios) {
    status |= reportScenario(scenario, outputDirectory, fonts);
  }
  return status;
}
