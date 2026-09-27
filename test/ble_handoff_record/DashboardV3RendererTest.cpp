#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "DashboardV3Quotes.h"
#include "DashboardV3Renderer.h"

namespace {

struct Operation {
  enum class Kind { Fill, Line, Rect, Text, Icon, Shade } kind;
  dashboard::v3::Rect bounds;
  std::string text;
  bool black = true;
  uint8_t iconId = 0;
  dashboard::v3::TextAlign align = dashboard::v3::TextAlign::Left;
  dashboard::v3::FontRole font = dashboard::v3::FontRole::Body;
  bool bold = false;
  bool dithered = false;
};

/// The nominal advance model, in the same shape as the renderer's own fallback:
/// a digit is ten pixels at the Micro rung, a capital twelve, a comma four and
/// a percent sign eighteen. The test canvas measures with these numbers so the
/// 8A layout runs down its measured path without the host needing font bitmaps,
/// and so the widths asserted here are independent of the renderer's estimate.
int nominalWidth(const dashboard::v3::FontRole role, const char* text) {
  int units = 0;
  for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text); *cursor != '\0'; ++cursor) {
    const unsigned char character = *cursor;
    if (character >= 0x80U) {
      units += 13;
      while ((cursor[1] & 0xC0U) == 0x80U) ++cursor;
    } else if (character >= '0' && character <= '9') {
      units += 10;
    } else if (character >= 'A' && character <= 'Z') {
      units += 12;
    } else if (character >= 'a' && character <= 'z') {
      units += 9;
    } else if (character == '%') {
      units += 18;
    } else if (character == '&') {
      units += 13;
    } else if (character == ' ') {
      units += 4;
    } else if (character == ',' || character == '.' || character == ':' || character == ';') {
      units += 4;
    } else if (character == '-') {
      units += 6;
    } else {
      units += 9;
    }
  }
  switch (role) {
    case dashboard::v3::FontRole::Micro: return units;
    case dashboard::v3::FontRole::Small: return units * 112 / 100;
    case dashboard::v3::FontRole::Body: return units * 124 / 100;
    case dashboard::v3::FontRole::Heading: return units * 150 / 100;
    case dashboard::v3::FontRole::Value: return units * 175 / 100;
    case dashboard::v3::FontRole::Hero: return units * 200 / 100;
  }
  return units;
}

class RecordingCanvas final : public dashboard::v3::DashboardV3Canvas {
 public:
  int width() const override { return 528; }
  int height() const override { return 792; }

  void fill(const dashboard::v3::Rect rect, const bool black) override {
    operations.push_back({Operation::Kind::Fill, rect, {}, black});
  }

  void line(const int x1, const int y1, const int x2, const int y2, const bool black) override {
    operations.push_back({Operation::Kind::Line,
                          {std::min(x1, x2), std::min(y1, y2), std::abs(x2 - x1) + 1, std::abs(y2 - y1) + 1},
                          {}, black});
  }

  void rect(const dashboard::v3::Rect rect, const bool black) override {
    operations.push_back({Operation::Kind::Rect, rect, {}, black});
  }

  void text(const dashboard::v3::TextSpec& spec, const char* value) override {
    operations.push_back(
        {Operation::Kind::Text, spec.bounds, value, spec.black, 0, spec.align, spec.font, spec.bold, spec.dithered});
  }

  /// `canMeasure = false` mimics a canvas without font metrics, which is the
  /// only case the renderer's character-count fallback runs in.
  int measureText(const dashboard::v3::TextSpec& spec, const char* value) const override {
    if (!canMeasure || value == nullptr) return -1;
    return nominalWidth(spec.font, value);
  }

  void icon(const uint8_t iconId, const dashboard::v3::Rect bounds, const bool black) override {
    operations.push_back({Operation::Kind::Icon, bounds, {}, black, iconId});
  }

  void shade(const dashboard::v3::Rect bounds, const dashboard::v3::Shade level) override {
    if (level == dashboard::v3::Shade::None) return;
    operations.push_back({Operation::Kind::Shade, bounds, {}, true});
    shades.push_back(level);
    shadeOps.emplace_back(bounds, level);
  }

  std::vector<dashboard::v3::Shade> shades;
  /// The same operations as `shades`, paired with the rectangle they covered so
  /// a test can tell an agenda row's stripe from a ribbon interval or a rain
  /// segment without re-deriving the geometry.
  std::vector<std::pair<dashboard::v3::Rect, dashboard::v3::Shade>> shadeOps;
  bool canMeasure = true;

  std::vector<Operation> operations;
};

TEST(DashboardV3Renderer, MinimalPackageKeepsEveryOperationInsideTheFixedCanvas) {
  RecordingCanvas canvas;
  dashboard::v3::DashboardV3Package package{};

  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  ASSERT_FALSE(canvas.operations.empty());
  for (const auto& operation : canvas.operations) {
    EXPECT_GE(operation.bounds.x, 0);
    EXPECT_GE(operation.bounds.y, 0);
    EXPECT_LE(operation.bounds.x + operation.bounds.width, canvas.width());
    EXPECT_LE(operation.bounds.y + operation.bounds.height, canvas.height());
  }

  ASSERT_EQ(canvas.operations.front().kind, Operation::Kind::Fill);
  EXPECT_EQ(canvas.operations.front().bounds, (dashboard::v3::Rect{0, 0, 528, 792}));
  EXPECT_FALSE(canvas.operations.front().black);

  bool hasBlackHeader = false;
  bool hasAgenda = false;
  bool hasStatus = false;
  for (const auto& operation : canvas.operations) {
    hasBlackHeader |= operation.kind == Operation::Kind::Fill && operation.black &&
                      operation.bounds == dashboard::v3::Rect{0, 0, 528, 77};
    hasAgenda |= operation.kind == Operation::Kind::Text && operation.text == "KOMENDE AFSPRAKEN";
    hasStatus |= operation.kind == Operation::Kind::Text && operation.text == "STATUS";
  }
  EXPECT_TRUE(hasBlackHeader);
  EXPECT_TRUE(hasAgenda);
  EXPECT_TRUE(hasStatus);
}

// --- Shared helpers for the extended recording tests -------------------------

const Operation* findTextOperation(const RecordingCanvas& canvas, const std::string& value) {
  for (const auto& operation : canvas.operations) {
    if (operation.kind == Operation::Kind::Text && operation.text == value) {
      return &operation;
    }
  }
  return nullptr;
}

const Operation* findIconOperation(const RecordingCanvas& canvas, const uint8_t iconId) {
  for (const auto& operation : canvas.operations) {
    if (operation.kind == Operation::Kind::Icon && operation.iconId == iconId) {
      return &operation;
    }
  }
  return nullptr;
}

void expectOperationsInsideCanvas(const RecordingCanvas& canvas) {
  for (const auto& operation : canvas.operations) {
    EXPECT_GE(operation.bounds.x, 0);
    EXPECT_GE(operation.bounds.y, 0);
    EXPECT_LE(operation.bounds.x + operation.bounds.width, canvas.width());
    EXPECT_LE(operation.bounds.y + operation.bounds.height, canvas.height());
  }
}

template <size_t Size>
void copyText(std::array<uint8_t, Size>& destination, uint8_t& length, const char* source) {
  size_t index = 0;
  while (index < Size && source[index] != '\0') {
    destination[index] = static_cast<uint8_t>(source[index]);
    ++index;
  }
  length = static_cast<uint8_t>(index);
}

dashboard::v3::DashboardV3Package sunSelectionPackage() {
  // Distinct, literal-expected sun times so the selected value is unambiguous.
  dashboard::v3::DashboardV3Package package{};
  package.weather.sunriseTodayMinute = 6 * 60 + 30;    // 06:30
  package.weather.sunsetTodayMinute = 21 * 60 + 15;    // 21:15
  package.weather.sunriseTomorrowMinute = 6 * 60 + 31; // 06:31
  return package;
}

dashboard::v3::DashboardV3Package maximumContentPackage() {
  dashboard::v3::DashboardV3Package package{};

  // A decoded V3 package always carries a timestamp; use a fixed one so the
  // header date column renders a real date (2026-08-25, a Tuesday).
  package.generatedAt = 1787616000ULL;
  package.weather.currentCelsius = 21;
  package.weather.minimumCelsius = 17;
  package.weather.maximumCelsius = 24;
  package.weather.conditionIconId = 3;
  package.weather.windKilometersPerHour = 18;
  package.weather.windDirection = 4;
  package.weather.sunriseTodayMinute = 6 * 60 + 30;
  package.weather.sunsetTodayMinute = 21 * 60 + 15;
  package.weather.sunriseTomorrowMinute = 6 * 60 + 31;

  for (uint8_t& bucket : package.rain) bucket = 15;
  // The rain band's absolute clock. Tests that exercise a specific headline
  // override this; everything else just needs a valid two-hour window.
  package.rainStartMinute = 12 * 60;

  package.heatingKnown = true;
  package.heatingAllowed = true;

  package.traffic.travelMinutes = 23;
  package.traffic.nationalCongestionKilometers = 42;
  package.traffic.classification = 2;
  copyText(package.traffic.destination, package.traffic.destinationLength, "UTRECHT");

  package.status.x3Battery = 88;
  package.status.vehicleBattery = 65;
  package.status.homeBattery = 42;
  package.status.steps = 7850;
  package.status.stepGoal = 10000;

  package.unreadTotal = 12;
  // Deliberately not id 0: a fixture on the default value would still pass if
  // the renderer ignored the id entirely.
  package.quoteId = 1;

  // One entry per declared row: a short list would leave the tail null and
  // crash the moment a ceiling is raised.
  static const char* const agendaTitles[dashboard::v3::MAX_AGENDA_ROWS] = {
      "AFSPRAAK 1", "AFSPRAAK 2", "AFSPRAAK 3", "AFSPRAAK 4",
      "AFSPRAAK 5", "AFSPRAAK 6", "AFSPRAAK 7", "AFSPRAAK 8",
      "AFSPRAAK 9", "AFSPRAAK 10", "AFSPRAAK 11", "AFSPRAAK 12", "AFSPRAAK 13"};
  static const char* const agendaDetails[dashboard::v3::MAX_AGENDA_ROWS] = {
      "DETAIL 1", "DETAIL 2", "DETAIL 3", "DETAIL 4",
      "DETAIL 5", "DETAIL 6", "DETAIL 7", "DETAIL 8",
      "DETAIL 9", "DETAIL 10", "DETAIL 11", "DETAIL 12", "DETAIL 13"};
  static_assert(std::size(agendaTitles) == dashboard::v3::MAX_AGENDA_ROWS, "one title per declared row");
  static_assert(std::size(agendaDetails) == dashboard::v3::MAX_AGENDA_ROWS, "one detail per declared row");
  for (size_t index = 0; index < dashboard::v3::MAX_AGENDA_ROWS; ++index) {
    package.agenda[index].dayOffset = static_cast<uint8_t>(index);
    package.agenda[index].minuteOfDay = static_cast<uint16_t>(9 * 60 + index);
    copyText(package.agenda[index].title, package.agenda[index].titleLength, agendaTitles[index]);
    copyText(package.agenda[index].detail, package.agenda[index].detailLength, agendaDetails[index]);
  }
  package.agendaCount = static_cast<uint8_t>(dashboard::v3::MAX_AGENDA_ROWS);

  // Three rows even though the wire cap is four: the legacy bands are tight
  // enough that a fourth market row costs a WhatsApp row, and every expectation
  // below was written against this three-row column. The format-4 ceiling is
  // exercised by the 8A tests instead.
  static const char* const marketLabels[] = {"AEX", "DOW", "DAX"};
  for (size_t index = 0; index < std::size(marketLabels); ++index) {
    copyText(package.markets[index].label, package.markets[index].labelLength, marketLabels[index]);
    package.markets[index].changeBasisPoints = static_cast<int16_t>(100 + static_cast<int16_t>(index) * 25);
  }
  package.marketCount = static_cast<uint8_t>(std::size(marketLabels));

  static const char* const chatNames[dashboard::v3::MAX_CHATS] = {"PAPA",   "MAMA",  "WERK", "SIEM",
                                                                  "FAMILIE", "LUKE", "TBOS"};
  static_assert(std::size(chatNames) == dashboard::v3::MAX_CHATS, "one name per declared chat row");
  for (size_t index = 0; index < dashboard::v3::MAX_CHATS; ++index) {
    copyText(package.chats[index].name, package.chats[index].nameLength, chatNames[index]);
    package.chats[index].unreadCount = static_cast<uint16_t>(index + 1);
    package.chats[index].lastMessageMinuteOfDay = static_cast<uint16_t>(10 * 60 + index);
  }
  package.chatCount = static_cast<uint8_t>(dashboard::v3::MAX_CHATS);

  return package;
}

// --- Sun selection -----------------------------------------------------------

TEST(DashboardV3Renderer, SunColumnBeforeSunriseShowsTodaySunriseAndZonOp) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, sunSelectionPackage(), /*minuteOfDay=*/5 * 60);

  const Operation* sunTime = findTextOperation(canvas, "06:30");
  const Operation* caption = findTextOperation(canvas, "ZON OP");
  ASSERT_NE(sunTime, nullptr);
  ASSERT_NE(caption, nullptr);
  EXPECT_GE(sunTime->bounds.x, 3 * canvas.width() / 4);
  EXPECT_GE(caption->bounds.x, 3 * canvas.width() / 4);
}

TEST(DashboardV3Renderer, SunColumnDuringDaylightShowsTodaySunsetAndZonOnder) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, sunSelectionPackage(), /*minuteOfDay=*/12 * 60);

  const Operation* sunTime = findTextOperation(canvas, "21:15");
  const Operation* caption = findTextOperation(canvas, "ZON ONDER");
  ASSERT_NE(sunTime, nullptr);
  ASSERT_NE(caption, nullptr);
  EXPECT_GE(sunTime->bounds.x, 3 * canvas.width() / 4);
  EXPECT_GE(caption->bounds.x, 3 * canvas.width() / 4);
}

TEST(DashboardV3Renderer, SunColumnAfterSunsetShowsTomorrowSunriseAndMorgenOp) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, sunSelectionPackage(), /*minuteOfDay=*/22 * 60);

  const Operation* sunTime = findTextOperation(canvas, "06:31");
  const Operation* caption = findTextOperation(canvas, "MORGEN OP");
  ASSERT_NE(sunTime, nullptr);
  ASSERT_NE(caption, nullptr);
  EXPECT_GE(sunTime->bounds.x, 3 * canvas.width() / 4);
  EXPECT_GE(caption->bounds.x, 3 * canvas.width() / 4);
}

// --- Header hierarchy ------------------------------------------------------
//
// The header has no date field of its own; the only date the package carries
// is the shared-prefix generatedAt timestamp. The renderer must show that
// date in a compact Dutch form instead of the literal "DATUM" placeholder,
// and must never invent a weather description the package does not carry.

TEST(DashboardV3Renderer, HeaderShowsPackageDateInProminentDutchForm) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.generatedAt = 1787616000ULL;  // 2026-08-25 00:00 UTC -> dinsdag 25 augustus
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  const Operation* date = findTextOperation(canvas, "DI 25 AUG");
  ASSERT_NE(date, nullptr);
  EXPECT_LT(date->bounds.x, 132) << "the date must sit in the first header column";
  EXPECT_FALSE(date->black) << "header text stays white on the black band";
  EXPECT_EQ(findIconOperation(canvas, 42), nullptr) << "the date no longer collides with a calendar icon";
}

TEST(DashboardV3Renderer, HeaderWithoutTimestampShowsDashesInsteadOfADate) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.generatedAt = 0;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  const Operation* date = findTextOperation(canvas, "-");
  ASSERT_NE(date, nullptr);
  EXPECT_LT(date->bounds.x, 132);
  EXPECT_EQ(findTextOperation(canvas, "25"), nullptr);
}

TEST(DashboardV3Renderer, HeaderEstablishesFourIconColumnsForDateWeatherWindSun) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  constexpr int columnWidth = 528 / 4;
  const Operation* condition = findIconOperation(canvas, 3);
  const Operation* wind = findIconOperation(canvas, 6);
  const Operation* sunset = findIconOperation(canvas, 12);  // daylight -> today's sunset
  ASSERT_NE(condition, nullptr);
  ASSERT_NE(wind, nullptr);
  ASSERT_NE(sunset, nullptr);
  EXPECT_EQ(findIconOperation(canvas, 42), nullptr);
  EXPECT_GE(condition->bounds.x, columnWidth);
  EXPECT_LT(condition->bounds.x, 2 * columnWidth);
  EXPECT_GE(wind->bounds.x, 2 * columnWidth);
  EXPECT_LT(wind->bounds.x, 3 * columnWidth);
  EXPECT_GE(sunset->bounds.x, 3 * columnWidth);

  EXPECT_NE(findTextOperation(canvas, "KM/U O"), nullptr);
  EXPECT_NE(findTextOperation(canvas, "ZON ONDER"), nullptr);
}

TEST(DashboardV3Renderer, WeatherColumnShowsOnlySupportedFields) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  // The package carries no free-text weather description, so the weather
  // column may draw only the condition icon, the current temperature and the
  // min/max range - never a made-up word like "ZONNIG".
  std::vector<std::string> weatherTexts;
  for (const auto& operation : canvas.operations) {
    if (operation.kind == Operation::Kind::Text && operation.bounds.x >= 132 && operation.bounds.x < 264) {
      weatherTexts.push_back(operation.text);
    }
  }
  // The min/max range moved under the date, where the mock-up puts it. The
  // weather column keeps its subject label, which is still never an invented
  // condition word.
  const std::vector<std::string> expected = {"21°", "WEER"};
  EXPECT_EQ(weatherTexts, expected);
}

TEST(DashboardV3Renderer, WeatherColumnFallsBackToSubjectLabelWithoutRange) {
  auto package = maximumContentPackage();
  package.weather.minimumCelsius = INT8_MIN;
  package.weather.maximumCelsius = INT8_MIN;
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  const Operation* caption = findTextOperation(canvas, "WEER");
  ASSERT_NE(caption, nullptr);
  EXPECT_GE(caption->bounds.x, 132);
  EXPECT_LT(caption->bounds.x, 264);
}

TEST(DashboardV3Renderer, SunColumnIconFollowsTheSelectedSunEvent) {
  RecordingCanvas canvasBefore;
  dashboard::v3::renderDashboardV3(canvasBefore, sunSelectionPackage(), /*minuteOfDay=*/5 * 60);
  const Operation* sunrise = findIconOperation(canvasBefore, 11);
  ASSERT_NE(sunrise, nullptr);
  EXPECT_GE(sunrise->bounds.x, 3 * canvasBefore.width() / 4);

  RecordingCanvas canvasDay;
  dashboard::v3::renderDashboardV3(canvasDay, sunSelectionPackage(), /*minuteOfDay=*/12 * 60);
  const Operation* sunset = findIconOperation(canvasDay, 12);
  ASSERT_NE(sunset, nullptr);
  EXPECT_GE(sunset->bounds.x, 3 * canvasDay.width() / 4);

  RecordingCanvas canvasAfter;
  dashboard::v3::renderDashboardV3(canvasAfter, sunSelectionPackage(), /*minuteOfDay=*/22 * 60);
  const Operation* tomorrowSunrise = findIconOperation(canvasAfter, 11);
  ASSERT_NE(tomorrowSunrise, nullptr);
  EXPECT_GE(tomorrowSunrise->bounds.x, 3 * canvasAfter.width() / 4);
}

// --- Maximum-content fixture -------------------------------------------------

TEST(DashboardV3Renderer, MaximumContentPackageKeepsEveryOperationInsideTheCanvas) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  ASSERT_FALSE(canvas.operations.empty());
  expectOperationsInsideCanvas(canvas);
}

// --- Black header inversion --------------------------------------------------

TEST(DashboardV3Renderer, HeaderFillIsBlackAndHeaderTextIsWhite) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  const dashboard::v3::Rect header{0, 0, canvas.width(), 77};
  bool headerFillBlack = false;
  int headerTextCount = 0;
  for (const auto& operation : canvas.operations) {
    if (operation.kind == Operation::Kind::Fill && operation.black && operation.bounds == header) {
      headerFillBlack = true;
    }
    const bool insideHeader = operation.kind == Operation::Kind::Text &&
                              operation.bounds.x >= header.x && operation.bounds.y >= header.y &&
                              operation.bounds.x + operation.bounds.width <= header.x + header.width &&
                              operation.bounds.y + operation.bounds.height <= header.y + header.height;
    if (insideHeader) {
      ++headerTextCount;
      EXPECT_FALSE(operation.black) << "header text \"" << operation.text << "\" must be white";
    }
  }
  EXPECT_TRUE(headerFillBlack);
  EXPECT_GT(headerTextCount, 0);
}

TEST(DashboardV3Renderer, MaximumContentDrawsEveryAgendaMarketAndChatRow) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  for (const char* title : {"AFSPRAAK 1", "AFSPRAAK 2", "AFSPRAAK 3", "AFSPRAAK 4", "AFSPRAAK 5"}) {
    EXPECT_NE(findTextOperation(canvas, title), nullptr) << title;
  }
  for (const char* market : {"AEX", "DOW", "DAX"}) {
    EXPECT_NE(findTextOperation(canvas, market), nullptr) << market;
  }
  for (const char* chat : {"PAPA", "MAMA", "WERK"}) {
    EXPECT_NE(findTextOperation(canvas, chat), nullptr) << chat;
  }

  bool hasMessageIcon = false;
  for (const auto& operation : canvas.operations) {
    hasMessageIcon |= operation.kind == Operation::Kind::Icon && operation.bounds.x >= 270;
  }
  EXPECT_TRUE(hasMessageIcon);
  expectOperationsInsideCanvas(canvas);
}

TEST(DashboardV3Renderer, EmptyChatsLeaveWhatsAppSectionOutWithoutMovingMarkets) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.chatCount = 0;
  package.unreadTotal = 0;

  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  // Comparing the two renders states the invariant directly. The old absolute
  // threshold only held for the row heights the status column happened to have.
  RecordingCanvas withChats;
  dashboard::v3::renderDashboardV3(withChats, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  EXPECT_EQ(findTextOperation(canvas, "WHATSAPP"), nullptr);
  ASSERT_NE(findTextOperation(canvas, "AEX"), nullptr);
  ASSERT_NE(findTextOperation(withChats, "AEX"), nullptr);
  EXPECT_EQ(findTextOperation(canvas, "AEX")->bounds.y, findTextOperation(withChats, "AEX")->bounds.y)
      << "dropping the chat section must not move the markets above it";
}

// --- Traffic hierarchy ------------------------------------------------------
//
// Two subjects: the commute (destination + travel minutes) dominates the left
// half, the national jam (congestion kilometres) dominates the right half.
// The classification byte maps to the three labels the V3 design spec names
// (NORMAAL / DRUK / FILE, matching the Swift wire enum normal=0, busy=1,
// jammed=2). The renderer must draw one of those and never invent another.

TEST(DashboardV3Renderer, TrafficPutsCommuteMinutesDominantLeftAndJamDistanceDominantRight) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  const Operation* minutes = findTextOperation(canvas, "23 min");
  const Operation* jamKm = findTextOperation(canvas, "42 km");
  ASSERT_NE(minutes, nullptr);
  ASSERT_NE(jamKm, nullptr);
  EXPECT_LT(minutes->bounds.x + minutes->bounds.width, 528 / 2) << "commute minutes stay in the left half";
  EXPECT_GE(jamKm->bounds.x, 528 / 2) << "jam distance sits in the right half";

  EXPECT_NE(findTextOperation(canvas, "UTRECHT"), nullptr);
  EXPECT_NE(findTextOperation(canvas, "FILES NEDERLAND"), nullptr);

  // The commute now uses the car icon the mock-up shows instead of a map pin;
  // findIconOperation returns the first match, which is this traffic one.
  const Operation* pin = findIconOperation(canvas, 25);
  const Operation* cone = findIconOperation(canvas, 27);
  ASSERT_NE(pin, nullptr);
  ASSERT_NE(cone, nullptr);
  EXPECT_LT(pin->bounds.x, 264);
  EXPECT_GE(cone->bounds.x, 264);
}

TEST(DashboardV3Renderer, TrafficClassificationByteUsesOnlyTheThreeDocumentedLabels) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.traffic.classification = 2;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  std::vector<std::string> trafficTexts;
  for (const auto& operation : canvas.operations) {
    const auto traffic = dashboard::v3::computeDashboardV3Layout(528, 792, {}).traffic;
    if (operation.kind == Operation::Kind::Text && operation.bounds.y >= traffic.y &&
        operation.bounds.y < traffic.y + traffic.height) {
      trafficTexts.push_back(operation.text);
    }
  }
  const std::vector<std::string> expected = {"UTRECHT", "23 min", "FILES NEDERLAND", "42 km", "FILE"};
  EXPECT_EQ(trafficTexts, expected);
}

TEST(DashboardV3Renderer, MissingTrafficDoesNotDrawLargeDashBlocks) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.traffic = {};
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  for (const auto& operation : canvas.operations) {
    const auto traffic = dashboard::v3::computeDashboardV3Layout(528, 792, {}).traffic;
    if (operation.kind != Operation::Kind::Text || operation.bounds.y < traffic.y ||
        operation.bounds.y >= traffic.y + traffic.height) {
      continue;
    }
    EXPECT_NE(operation.font, dashboard::v3::FontRole::Hero)
        << "missing traffic must not become a large black dash";
  }
}

// --- Status hierarchy -------------------------------------------------------

TEST(DashboardV3Renderer, StatusRowsUseIconsAndProgressBarsForBatteries) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  struct ExpectedRow {
    uint8_t iconId;
    const char* name;
    const char* percent;
    uint8_t value;
  };
  const ExpectedRow rows[] = {
      {17, "X3", "88%", 88}, {25, "IONIQ 5", "65%", 65}, {20, "THUISACCU", "42%", 42}};
  // bodyRight.y + top padding + the STATUS heading; the row pitch is the Micro
  // ascender (17) + gap (6) + bar (8) + spacing (10).
  const int firstRowY = dashboard::v3::computeDashboardV3Layout(528, 792, {}).bodyRight.y + 10 + 21 + 10;
  const int rowPitch = 41;
  for (size_t index = 0; index < 3; ++index) {
    const int rowY = firstRowY + static_cast<int>(index) * rowPitch;
    const Operation* icon = nullptr;
    for (const auto& operation : canvas.operations) {
      if (operation.kind == Operation::Kind::Icon && operation.iconId == rows[index].iconId &&
          operation.bounds.x >= 270) {
        icon = &operation;
      }
    }
    ASSERT_NE(icon, nullptr);
    EXPECT_GE(icon->bounds.x, 270);
    EXPECT_NE(findTextOperation(canvas, rows[index].name), nullptr);
    EXPECT_NE(findTextOperation(canvas, rows[index].percent), nullptr);

    const Operation* track = nullptr;
    const Operation* fill = nullptr;
    for (const auto& operation : canvas.operations) {
      if (operation.bounds.y < rowY || operation.bounds.y >= rowY + rowPitch) {
        continue;
      }
      if (operation.kind == Operation::Kind::Rect) {
        track = &operation;
      }
      if (operation.kind == Operation::Kind::Fill && operation.bounds.x >= 270) {
        fill = &operation;
      }
    }
    ASSERT_NE(track, nullptr) << "battery row needs a bar track";
    ASSERT_NE(fill, nullptr) << "battery row needs a filled fraction";
    EXPECT_GE(track->bounds.x, 270);
    EXPECT_EQ(fill->bounds.width, (track->bounds.width - 2) * rows[index].value / 100);
    EXPECT_GE(fill->bounds.x, track->bounds.x);
    EXPECT_LE(fill->bounds.x + fill->bounds.width, track->bounds.x + track->bounds.width);
  }

  EXPECT_GE(findTextOperation(canvas, "THUISACCU")->bounds.width, 100);
}

TEST(DashboardV3Renderer, StepsUseTheSameMeterRowAsTheBatteryRows) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  // Steps are a fourth meter, not a block of their own: same icon column, same
  // label rung, same value slot, same bar. Only the value differs - a count
  // rather than a percentage - because the goal is context and the count is not.
  const Operation* steps = findTextOperation(canvas, "STAPPEN");
  const Operation* stepsValue = findTextOperation(canvas, "7.850");
  const Operation* x3 = findTextOperation(canvas, "X3");
  const Operation* x3Value = findTextOperation(canvas, "88%");
  const Operation* footprints = findIconOperation(canvas, 47);
  ASSERT_NE(steps, nullptr);
  ASSERT_NE(stepsValue, nullptr);
  ASSERT_NE(x3, nullptr);
  ASSERT_NE(x3Value, nullptr);
  ASSERT_NE(footprints, nullptr);

  EXPECT_EQ(steps->bounds.x, x3->bounds.x) << "labels share one column";
  EXPECT_EQ(steps->bounds.width, x3->bounds.width);
  EXPECT_EQ(steps->font, x3->font) << "labels share one rung";
  EXPECT_EQ(stepsValue->bounds.x, x3Value->bounds.x) << "values share one slot";
  EXPECT_EQ(stepsValue->bounds.width, x3Value->bounds.width);
  EXPECT_EQ(stepsValue->font, x3Value->font);

  // Fourth row on the same pitch as the first three.
  const int pitch = steps->bounds.y - x3->bounds.y;
  EXPECT_EQ(pitch % 3, 0) << "steps sit a whole number of rows below X3";
  EXPECT_GT(pitch, 0);

  // And it carries a bar like the others.
  bool hasTrack = false;
  for (const auto& operation : canvas.operations) {
    hasTrack |= operation.kind == Operation::Kind::Rect && operation.bounds.x >= 270 &&
                operation.bounds.y > steps->bounds.y && operation.bounds.y < steps->bounds.y + 30;
  }
  EXPECT_TRUE(hasTrack) << "the steps row needs the same progress bar as a battery row";
}

// The phone sends a quote id, never the text, and computes it as
// `dayNumber % quotes.count` - zero based, with no "absent" value. If the two
// tables drift apart by one entry, every footer quietly renders the wrong
// quote, which nothing else in the system would notice. The full entry-by-entry
// comparison against the Swift table lives in
// scripts/validate_dashboard_v3_quotes.py --firmware; these checks pin the
// count, the widened-id boundaries and a few spot entries.

TEST(DashboardV3Quotes, TableHoldsAFullYearAndTheWidenedIdsResolve) {
  ASSERT_EQ(dashboard::v3::QUOTE_COUNT, 365u) << "the phone selects the id as dayNumber % 365";
  // 255 is the last id a one-byte wire field could carry; 256 and 364 only
  // exist because the field widened to a little-endian uint16_t.
  for (const uint16_t id : {0, 255, 256, 364}) {
    const dashboard::v3::Quote* quote = dashboard::v3::quoteForId(id);
    ASSERT_NE(quote, nullptr) << "id " << id << " must resolve";
    EXPECT_NE(quote->text[0], '\0') << "id " << id << " has empty text";
    EXPECT_NE(quote->author[0], '\0') << "id " << id << " has an empty author";
  }

  // The first eight entries keep their order - old packages addressed them by
  // the same ids - and the last entry proves the table's tail came across.
  const struct {
    uint16_t id;
    const char* text;
    const char* author;
  } spotChecks[] = {
      {0, "Verbeelding is belangrijker dan kennis.", "Albert Einstein"},
      {7, "Kennis spreekt, maar wijsheid luistert.", "Jimi Hendrix"},
      {8, "Attention is the rarest gift you can give.", "Anonymous"},
      {364, "The second best time to plant a tree is now.", "English proverb"},
  };
  for (const auto& spot : spotChecks) {
    const dashboard::v3::Quote* quote = dashboard::v3::quoteForId(spot.id);
    ASSERT_NE(quote, nullptr) << "id " << spot.id << " must resolve";
    EXPECT_STREQ(quote->text, spot.text) << "at id " << spot.id;
    EXPECT_STREQ(quote->author, spot.author) << "at id " << spot.id;
  }
}

TEST(DashboardV3Quotes, IdsPastTheEndOfTheTableDrawNothing) {
  const dashboard::v3::Quote* first = dashboard::v3::quoteForId(0);
  ASSERT_NE(first, nullptr) << "id 0 is a real quote, not an absent sentinel";
  EXPECT_STREQ(first->author, "Albert Einstein");
  // A newer phone may carry a longer table; an unknown id must draw nothing
  // rather than wrap around onto the wrong quote.
  EXPECT_EQ(dashboard::v3::quoteForId(static_cast<uint16_t>(dashboard::v3::QUOTE_COUNT)), nullptr);
  EXPECT_EQ(dashboard::v3::quoteForId(UINT16_MAX), nullptr);
}

// A dry forecast and a missing one look identical in the buckets, so the wire
// flag is the only thing that separates them. Getting this backwards put
// "GEEN REGENINFO" over a real, dry two hours on hardware.

TEST(DashboardV3Renderer, AllZeroRainWithTheFlagSetReadsAsDryNotAsMissing) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.rainKnown = true;
  package.rainStartMinute = 12 * 60;
  package.rain.fill(0);
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  EXPECT_EQ(findTextOperation(canvas, "GEEN REGENINFO"), nullptr);
  EXPECT_NE(findTextOperation(canvas, "DROOG TOT 14:00"), nullptr);
}

TEST(DashboardV3Renderer, UnknownRainSaysSoAndDrawsNoStrip) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.rainKnown = false;
  package.rain.fill(0);
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  EXPECT_NE(findTextOperation(canvas, "GEEN REGENINFO"), nullptr);
  // No empty outlined strip and no window clock: both read as a broken widget
  // rather than as absent information.
  const auto rain = dashboard::v3::computeDashboardV3Layout(528, 792, {}).rain;
  for (const auto& operation : canvas.operations) {
    if (operation.bounds.y < rain.y || operation.bounds.y >= rain.y + rain.height) continue;
    EXPECT_NE(operation.kind, Operation::Kind::Rect) << "no empty strip outline";
  }
  EXPECT_EQ(findTextOperation(canvas, "10:20"), nullptr) << "no window clock without a window";
}

// The rain nibble is an intensity band, not a rescaled Buienradar byte. The
// phone and this renderer must both use the same table, or a drizzle and a
// downpour collapse into the same half-tone (the bug this guards against):
//   0 = dry, 1 = light, 2 = moderate, 3 = heavy, 4..15 = heavy as well.
std::vector<dashboard::v3::Shade> rainShadeLevels(const RecordingCanvas& canvas) {
  const dashboard::v3::Rect rain = dashboard::v3::computeDashboardV3Layout(528, 792, {}).rain;
  std::vector<dashboard::v3::Shade> levels;
  size_t shadeIndex = 0;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Shade) continue;
    const dashboard::v3::Shade level = canvas.shades[shadeIndex++];
    if (operation.bounds.y >= rain.y && operation.bounds.y + operation.bounds.height <= rain.y + rain.height) {
      levels.push_back(level);
    }
  }
  return levels;
}

TEST(DashboardV3Renderer, RainShowerSpansQuarterHalfAndSolid) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.rainKnown = true;
  // A shower building from dry through drizzle, steady rain and a heavy core:
  // the strip must not collapse that arc onto a single half-tone.
  package.rain.fill(0);
  for (size_t index = 6; index < 12; ++index) package.rain[index] = 1;
  for (size_t index = 12; index < 18; ++index) package.rain[index] = 2;
  for (size_t index = 18; index < dashboard::v3::RAIN_BUCKET_COUNT; ++index) package.rain[index] = 3;

  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  const std::vector<dashboard::v3::Shade> levels = rainShadeLevels(canvas);
  ASSERT_FALSE(levels.empty());
  EXPECT_NE(std::find(levels.begin(), levels.end(), dashboard::v3::Shade::Quarter), levels.end());
  EXPECT_NE(std::find(levels.begin(), levels.end(), dashboard::v3::Shade::Half), levels.end());
  EXPECT_NE(std::find(levels.begin(), levels.end(), dashboard::v3::Shade::Solid), levels.end());
}

TEST(DashboardV3Renderer, RainNibbleMapsDirectlyToShadeLevels) {
  const auto renderLevels = [](const uint8_t nibble) {
    RecordingCanvas canvas;
    auto package = maximumContentPackage();
    package.rainKnown = true;
    package.rain.fill(nibble);
    dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);
    return rainShadeLevels(canvas);
  };

  const std::vector<dashboard::v3::Shade> light = renderLevels(1);
  ASSERT_FALSE(light.empty());
  EXPECT_TRUE(std::all_of(light.begin(), light.end(), [](const dashboard::v3::Shade level) {
    return level == dashboard::v3::Shade::Quarter;
  })) << "nibble 1 (light rain) must render as Quarter";

  const std::vector<dashboard::v3::Shade> moderate = renderLevels(2);
  ASSERT_FALSE(moderate.empty());
  EXPECT_TRUE(std::all_of(moderate.begin(), moderate.end(), [](const dashboard::v3::Shade level) {
    return level == dashboard::v3::Shade::Half;
  })) << "nibble 2 (moderate rain) must render as Half";

  const std::vector<dashboard::v3::Shade> heavy = renderLevels(3);
  ASSERT_FALSE(heavy.empty());
  EXPECT_TRUE(std::all_of(heavy.begin(), heavy.end(), [](const dashboard::v3::Shade level) {
    return level == dashboard::v3::Shade::Solid;
  })) << "nibble 3 (heavy rain) must render as Solid";
}

// The rain headline is an absolute clock time anchored to rainStartMinute, not
// the panel's own clock: the package can be a quarter hour old by the time it
// is drawn. The word for a shower that starts later follows the peak of the
// contiguous wet run, exactly like RainForecast.outlook in the phone.

TEST(DashboardV3Renderer, RainComingLaterWithHeavyPeakNamesTheStartAndSeverity) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.rainKnown = true;
  package.rainStartMinute = 9 * 60;  // 09:00
  package.rain.fill(0);
  package.rain[4] = 3;               // first wet bucket, peak of the run
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  EXPECT_NE(findTextOperation(canvas, "09:20 HEVIGE REGEN"), nullptr);
}

TEST(DashboardV3Renderer, RainComingLaterWithOnlyLightRainStaysLight) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.rainKnown = true;
  package.rainStartMinute = 10 * 60;  // 10:00
  package.rain.fill(0);
  package.rain[3] = 1;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  EXPECT_NE(findTextOperation(canvas, "10:15 LICHTE REGEN"), nullptr);
}

TEST(DashboardV3Renderer, LaterHeavierShowerDoesNotInfluenceTheFirstHeadline) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.rainKnown = true;
  package.rainStartMinute = 11 * 60;  // 11:00
  package.rain.fill(0);
  package.rain[2] = 1;  // first shower: light, then a dry bucket
  package.rain[4] = 3;  // second shower: heavy, but a later bui
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  EXPECT_NE(findTextOperation(canvas, "11:10 LICHTE REGEN"), nullptr);
  EXPECT_EQ(findTextOperation(canvas, "HEVIGE REGEN"), nullptr);
}

TEST(DashboardV3Renderer, RainStoppingNamesTheFirstDryBucket) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.rainKnown = true;
  package.rainStartMinute = 12 * 60;  // 12:00
  package.rain.fill(0);
  for (size_t index = 0; index < 6; ++index) package.rain[index] = 2;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  EXPECT_NE(findTextOperation(canvas, "12:30 DROOG"), nullptr);
}

TEST(DashboardV3Renderer, AllDryNamesTheWindowEnd) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.rainKnown = true;
  package.rainStartMinute = 13 * 60;  // 13:00
  package.rain.fill(0);
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  EXPECT_NE(findTextOperation(canvas, "DROOG TOT 15:00"), nullptr);
}

TEST(DashboardV3Renderer, AllWetSaysRainContinuesPastTheWindowEnd) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.rainKnown = true;
  package.rainStartMinute = 14 * 60;  // 14:00
  package.rain.fill(2);
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  EXPECT_NE(findTextOperation(canvas, "REGEN TOT NA 16:00"), nullptr);
}

TEST(DashboardV3Renderer, RainHeadlineWrapsAcrossMidnight) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.rainKnown = true;
  package.rainStartMinute = 23 * 60 + 50;  // 23:50
  package.rain.fill(0);
  package.rain[4] = 3;                     // T + 5 * 4 = 00:10 the next day
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  EXPECT_NE(findTextOperation(canvas, "00:10 HEVIGE REGEN"), nullptr);
}

TEST(DashboardV3Renderer, RainStripLabelsShowThePackageWindowNotThePanelClock) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.rainKnown = true;
  package.rainStartMinute = 15 * 60;  // 15:00
  package.rain.fill(1);
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  EXPECT_NE(findTextOperation(canvas, "15:00"), nullptr) << "left label is the first bucket's clock";
  EXPECT_NE(findTextOperation(canvas, "17:00"), nullptr) << "right label is two hours later";
  EXPECT_EQ(findTextOperation(canvas, "NU 12:00"), nullptr) << "the panel clock no longer labels the strip";
  EXPECT_EQ(findTextOperation(canvas, "NU 15:00"), nullptr) << "the word NU is a lie on a stale package";
}

// The RTC runs UTC and the wire carries no timezone, so the package timestamp
// and the device's own clock are in different frames. Drawing them side by side
// without reconciling put "ververst 06:20" under a band reading "NU 08:20".

TEST(DashboardV3Renderer, RefreshTimeIsDrawnInTheDevicesLocalTime) {
  auto package = maximumContentPackage();
  package.generatedAt = 1787616000ULL + 6 * 3600 + 20 * 60;  // 06:20 UTC

  RecordingCanvas utc;
  dashboard::v3::renderDashboardV3(utc, package, /*minuteOfDay=*/380, dashboard::v3::UTC_OFFSET_Q_UTC);
  EXPECT_NE(findTextOperation(utc, "ververst 06:20"), nullptr);

  RecordingCanvas cest;  // offset 56 == UTC+2
  dashboard::v3::renderDashboardV3(cest, package, /*minuteOfDay=*/500, 56);
  EXPECT_NE(findTextOperation(cest, "ververst 08:20"), nullptr);
  EXPECT_EQ(findTextOperation(cest, "ververst 06:20"), nullptr);
}

TEST(DashboardV3Renderer, HeaderDateFollowsLocalTimeAcrossMidnight) {
  auto package = maximumContentPackage();
  // 22:30 UTC on 25 August is already 00:30 on 26 August in CEST.
  package.generatedAt = 1787616000ULL - 3600 - 30 * 60;

  RecordingCanvas utc;
  dashboard::v3::renderDashboardV3(utc, package, /*minuteOfDay=*/1350, dashboard::v3::UTC_OFFSET_Q_UTC);
  EXPECT_NE(findTextOperation(utc, "DI 25 AUG"), nullptr);

  RecordingCanvas cest;
  dashboard::v3::renderDashboardV3(cest, package, /*minuteOfDay=*/30, 56);
  EXPECT_NE(findTextOperation(cest, "WO 26 AUG"), nullptr)
      << "after local midnight the header must not still show yesterday";
}

TEST(DashboardV3Renderer, ChatClockNeverTruncates) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  // 20:40 is among the widest clocks at the Micro rung; it used to lose its
  // minutes to a 44 px box and render as "20:...".
  package.chats[0].lastMessageMinuteOfDay = 20 * 60 + 40;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  const Operation* clock = findTextOperation(canvas, "20:40");
  ASSERT_NE(clock, nullptr) << "the chat clock must survive intact";
  EXPECT_GE(clock->bounds.width, 48) << "a clock box must hold the widest HH:MM at the Micro rung";
}

TEST(DashboardV3Renderer, WhatsAppHeadingCarriesNoMessageIcon) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  ASSERT_NE(findTextOperation(canvas, "WHATSAPP"), nullptr);
  EXPECT_EQ(findIconOperation(canvas, 65), nullptr)
      << "the heading already names the source; the glyph only crowded the unread count";
}

TEST(DashboardV3Renderer, FourDigitCongestionStepsDownARungInsteadOfTruncating) {
  RecordingCanvas canvas;
  auto package = maximumContentPackage();
  package.traffic.nationalCongestionKilometers = 1860;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  const Operation* value = findTextOperation(canvas, "1860 km");
  ASSERT_NE(value, nullptr) << "a truncated number would read as a smaller jam";
  EXPECT_EQ(value->font, dashboard::v3::FontRole::Value)
      << "four digits drop one rung so they still fit beside the classification badge";

  RecordingCanvas threeDigits;
  dashboard::v3::renderDashboardV3(threeDigits, maximumContentPackage(), /*minuteOfDay=*/12 * 60);
  const Operation* normal = findTextOperation(threeDigits, "42 km");
  ASSERT_NE(normal, nullptr);
  EXPECT_EQ(normal->font, dashboard::v3::FontRole::Hero) << "the usual case keeps the Hero rung";
}

// --- Font-safe placeholders ------------------------------------------------
//
// The real Lexend firmware font renders U+2014 (em dash) as a replacement
// diamond, so every unknown-value placeholder and the footer author must use
// the font-safe ASCII hyphen instead. The optional PBM glyphs are much
// narrower than production Lexend, so only a byte-level check can prove this.

TEST(DashboardV3Renderer, MinimalPackageEmitsOnlyFontSafeAsciiPlaceholders) {
  RecordingCanvas canvas;
  dashboard::v3::DashboardV3Package package{};
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  ASSERT_FALSE(canvas.operations.empty());
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Text) {
      continue;
    }
    for (const unsigned char byte : operation.text) {
      EXPECT_GE(static_cast<unsigned>(byte), 0x20U)
          << "control byte in \"" << operation.text << "\"";
      EXPECT_LE(static_cast<unsigned>(byte), 0x7EU)
          << "non-ASCII byte in \"" << operation.text
          << "\" (U+2014 renders as a diamond on the Lexend firmware font)";
    }
  }

  EXPECT_NE(findTextOperation(canvas, "-"), nullptr) << "header date label uses an ASCII dash";
  EXPECT_NE(findTextOperation(canvas, "VERWARMING -"), nullptr) << "unknown heating uses an ASCII dash";
  // Quote ids are zero based, matching the phone's dayNumber % quotes.count, so
  // the default package already selects the first quote.
  EXPECT_NE(findTextOperation(canvas, "- Albert Einstein"), nullptr) << "footer author uses an ASCII dash";
}

TEST(DashboardV3Renderer, MaximumContentNeverEmitsTheEmDashGlyph) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Text) {
      continue;
    }
    EXPECT_EQ(operation.text.find("\xE2\x80\x94"), std::string::npos)
        << "U+2014 must not be emitted: \"" << operation.text << "\"";
  }
  EXPECT_NE(findTextOperation(canvas, "- Leonardo da Vinci"), nullptr);
}

TEST(DashboardV3Renderer, UnknownMarketChangeRendersAsciiHyphen) {
  auto package = maximumContentPackage();
  for (auto& market : package.markets) {
    market.changeBasisPoints = INT16_MIN;
  }
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  const Operation* change = findTextOperation(canvas, "-");
  ASSERT_NE(change, nullptr);
  EXPECT_GE(change->bounds.x, 270) << "the market change dash sits in the status column";
  EXPECT_GE(change->bounds.y, 465) << "the market change dash sits below the steps row";
}

// The mock-up marks direction with a solid triangle rather than a sign, so the
// percentage itself carries no "+" or "-".
TEST(DashboardV3Renderer, MarketChangeUsesATriangleInsteadOfASignedNumber) {
  auto package = maximumContentPackage();
  package.markets[0].changeBasisPoints = 80;    // rose 0,80%
  package.markets[1].changeBasisPoints = -120;  // fell 1,20%
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  const Operation* gain = findTextOperation(canvas, "0,80%");
  const Operation* loss = findTextOperation(canvas, "1,20%");
  ASSERT_NE(gain, nullptr) << "a rising market prints its change unsigned";
  ASSERT_NE(loss, nullptr) << "a falling market prints its change unsigned";
  EXPECT_EQ(findTextOperation(canvas, "+0,80%"), nullptr) << "the triangle already says it rose";
  EXPECT_EQ(findTextOperation(canvas, "-1,20%"), nullptr) << "the triangle already says it fell";

  // A triangle is drawn as one filled row per scanline, so its apex is simply
  // its narrowest row: at the top when rising, at the bottom when falling.
  const auto apexIsAtTop = [&canvas](const Operation& valueRow) {
    const Operation* narrowest = nullptr;
    const Operation* widest = nullptr;
    for (const auto& operation : canvas.operations) {
      if (operation.kind != Operation::Kind::Fill || operation.bounds.height != 1) continue;
      if (operation.bounds.x >= valueRow.bounds.x) continue;  // the mark sits left of the number
      if (operation.bounds.y < valueRow.bounds.y - 4) continue;
      if (operation.bounds.y > valueRow.bounds.y + valueRow.bounds.height + 4) continue;
      if (narrowest == nullptr || operation.bounds.width < narrowest->bounds.width) narrowest = &operation;
      if (widest == nullptr || operation.bounds.width > widest->bounds.width) widest = &operation;
    }
    EXPECT_NE(narrowest, nullptr) << "no triangle rows found beside the market value";
    return narrowest != nullptr && widest != nullptr && narrowest->bounds.y < widest->bounds.y;
  };

  EXPECT_TRUE(apexIsAtTop(*gain)) << "a rising market points up";
  EXPECT_FALSE(apexIsAtTop(*loss)) << "a falling market points down";
}

// Row 0 is the panel's leading financial figure. The renderer does not know
// what it means -- the phone decides which source fills the first slot -- it
// only knows that the first row is the one worth reading from across the room.
TEST(DashboardV3Renderer, LeadingMarketDrawsItsChangeAtTheHeadingRung) {
  auto package = maximumContentPackage();
  copyText(package.markets[0].label, package.markets[0].labelLength, "Portefeuille");
  package.markets[0].changeBasisPoints = 68;
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  const Operation* value = findTextOperation(canvas, "0,68%");
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(value->font, dashboard::v3::FontRole::Heading)
      << "the leading market outranks the rows below it, but the Value rung would "
         "cost two of the five WhatsApp rows that fill the rest of this column";
  EXPECT_GE(value->bounds.x, 270) << "it stays in the status column";
}

// The caption rung is ALL-CAPS everywhere else on the panel, and this label
// arrives as a Home Assistant friendly name.
TEST(DashboardV3Renderer, LeadingMarketLabelBecomesAnUpperCaseCaption) {
  auto package = maximumContentPackage();
  copyText(package.markets[0].label, package.markets[0].labelLength, "Portefeuille");
  package.markets[0].changeBasisPoints = 68;
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  const Operation* caption = findTextOperation(canvas, "PORTEFEUILLE");
  ASSERT_NE(caption, nullptr) << "the label itself is the caption of the block";
  EXPECT_EQ(caption->font, dashboard::v3::FontRole::Micro);
  EXPECT_EQ(findTextOperation(canvas, "Portefeuille"), nullptr)
      << "the mixed-case name does not also appear";
}

TEST(DashboardV3Renderer, RemainingMarketsKeepTheSmallRungUnderTheirOwnHeading) {
  auto package = maximumContentPackage();
  copyText(package.markets[0].label, package.markets[0].labelLength, "Portefeuille");
  package.markets[0].changeBasisPoints = 68;
  package.markets[1].changeBasisPoints = 42;
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  const Operation* heading = findTextOperation(canvas, "MARKTEN \xc2\xb7 DAG");
  const Operation* lead = findTextOperation(canvas, "0,68%");
  const Operation* row = findTextOperation(canvas, "0,42%");
  ASSERT_NE(heading, nullptr);
  ASSERT_NE(lead, nullptr);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->font, dashboard::v3::FontRole::Small) << "the index rows are unchanged";
  EXPECT_GT(heading->bounds.y, lead->bounds.y) << "the heading sits below the leading block";
  EXPECT_GT(row->bounds.y, heading->bounds.y);
}

TEST(DashboardV3Renderer, ASingleMarketLeavesOutTheMarketsHeading) {
  auto package = maximumContentPackage();
  copyText(package.markets[0].label, package.markets[0].labelLength, "Portefeuille");
  package.markets[0].changeBasisPoints = 68;
  package.marketCount = 1;
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  EXPECT_NE(findTextOperation(canvas, "0,68%"), nullptr);
  EXPECT_EQ(findTextOperation(canvas, "MARKTEN \xc2\xb7 DAG"), nullptr)
      << "a heading over nothing is noise";
}

TEST(DashboardV3Renderer, MissingLeadingMarketChangeDrawsADashAtTheHeadingRung) {
  auto package = maximumContentPackage();
  copyText(package.markets[0].label, package.markets[0].labelLength, "Portefeuille");
  package.markets[0].changeBasisPoints = INT16_MIN;
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  const Operation* dash = findTextOperation(canvas, "-");
  ASSERT_NE(dash, nullptr);
  EXPECT_EQ(dash->font, dashboard::v3::FontRole::Heading)
      << "the block keeps its size when the source is missing, so the layout does not jump";
}

// --- Agenda time geometry --------------------------------------------------
//
// The agenda time sits on the Micro rung, where the widest clock ("00:00")
// measures 48 px against the built-in Lexend 8 bold advance table. The box must
// stay wider than that, and short enough that the right-aligned text never
// reaches the first timeline dot.

TEST(DashboardV3Renderer, AgendaTimeBoundsHoldAFullHHMMInTheProductionFont) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  const Operation* time = findTextOperation(canvas, "09:00");
  ASSERT_NE(time, nullptr);
  EXPECT_GE(time->bounds.width, 48) << "agenda time box must hold a full HH:MM at the Micro rung";
  EXPECT_LE(time->bounds.x + time->bounds.width, 85)
      << "right-aligned time text must not reach the first timeline dot";

  const Operation* title = findTextOperation(canvas, "AFSPRAAK 1");
  ASSERT_NE(title, nullptr);
  EXPECT_GE(title->bounds.x, time->bounds.x + time->bounds.width)
      << "the agenda title must start after the widened time column";
  expectOperationsInsideCanvas(canvas);
}

// --- Steps -----------------------------------------------------------------

TEST(DashboardV3Renderer, StepsShowOnlyTheCurrentCountInTheStatusColumn) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  const Operation* stepsValue = findTextOperation(canvas, "7.850");
  ASSERT_NE(stepsValue, nullptr);
  EXPECT_GE(stepsValue->bounds.x, 270);
  EXPECT_EQ(findTextOperation(canvas, "7.850 / 10.000"), nullptr)
      << "the narrow status column shows only the current step count, not count / goal";
}

// --- Markets and chats -----------------------------------------------------

TEST(DashboardV3Renderer, ChatSectionMovesDirectlyBelowStepsWhenMarketsAreEmpty) {
  auto package = maximumContentPackage();
  package.marketCount = 0;
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);

  EXPECT_EQ(findTextOperation(canvas, "MARKTEN"), nullptr)
      << "an empty MARKTEN heading must not leave a large blank block";
  const Operation* whatsapp = findTextOperation(canvas, "WHATSAPP");
  const Operation* papa = findTextOperation(canvas, "PAPA");
  ASSERT_NE(whatsapp, nullptr);
  ASSERT_NE(papa, nullptr);
  const Operation* steps = findTextOperation(canvas, "STAPPEN");
  ASSERT_NE(steps, nullptr);
  EXPECT_GT(papa->bounds.y, steps->bounds.y) << "the first chat row sits below steps";
  const auto body = dashboard::v3::computeDashboardV3Layout(528, 792, {}).body;
  EXPECT_LT(papa->bounds.y, body.y + body.height) << "and stays inside the body band";
  EXPECT_LT(whatsapp->bounds.y, papa->bounds.y);
}

TEST(DashboardV3Renderer, ChatRowsAreCompactOneLineRowsLedByTheirTime) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  const Operation* papa = findTextOperation(canvas, "PAPA");
  const Operation* mama = findTextOperation(canvas, "MAMA");
  const Operation* werk = findTextOperation(canvas, "WERK");
  const Operation* time = findTextOperation(canvas, "10:00");
  ASSERT_NE(papa, nullptr);
  ASSERT_NE(mama, nullptr);
  ASSERT_NE(werk, nullptr);
  ASSERT_NE(time, nullptr);

  EXPECT_LE(std::abs(time->bounds.y - papa->bounds.y), 2)
      << "name and last-message time share one compact row";
  EXPECT_LE(time->bounds.x + time->bounds.width, papa->bounds.x)
      << "the time leads the row and must not overlap the chat name";
  EXPECT_GT(mama->bounds.y, papa->bounds.y);
  EXPECT_GT(werk->bounds.y, mama->bounds.y);
  EXPECT_LE(time->bounds.x + time->bounds.width, canvas.width());
}

TEST(DashboardV3Renderer, WithMarketsWhatsAppSitsBelowTheLastMarketRowWithoutOverlap) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  const Operation* dax = findTextOperation(canvas, "DAX");
  const Operation* whatsapp = findTextOperation(canvas, "WHATSAPP");
  const Operation* papa = findTextOperation(canvas, "PAPA");
  ASSERT_NE(dax, nullptr);
  ASSERT_NE(whatsapp, nullptr);
  ASSERT_NE(papa, nullptr);
  EXPECT_GT(whatsapp->bounds.y, dax->bounds.y + dax->bounds.height)
      << "WhatsApp heading starts below the last market row";
  EXPECT_GT(papa->bounds.y, whatsapp->bounds.y + whatsapp->bounds.height);
  expectOperationsInsideCanvas(canvas);
}

// --- Footer ----------------------------------------------------------------

TEST(DashboardV3Renderer, FooterQuoteIsLeftAlignedWithAsciiHyphenAuthor) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  // quoteId 1 in the shared fixture, which is the second entry of the table.
  const Operation* quote = findTextOperation(canvas, "Eenvoud is de ultieme verfijning.");
  const Operation* author = findTextOperation(canvas, "- Leonardo da Vinci");
  ASSERT_NE(quote, nullptr);
  ASSERT_NE(author, nullptr);
  EXPECT_EQ(quote->align, dashboard::v3::TextAlign::Left);
  EXPECT_EQ(author->align, dashboard::v3::TextAlign::Left);
  EXPECT_GE(quote->bounds.y, dashboard::v3::computeDashboardV3Layout(528, 792, {}).footer.y)
      << "the quote line lives inside the footer band";
  EXPECT_GE(author->bounds.y, quote->bounds.y + quote->bounds.height)
      << "the smaller author line sits below the quote line";
  EXPECT_LE(author->bounds.y + author->bounds.height, 792);
  EXPECT_EQ(quote->font, dashboard::v3::FontRole::Body)
      << "the quote must stay on the Body rung: the wider rungs truncate at 528 px";
}

TEST(DashboardV3Renderer, DeviceBatteryOverridesOnlyTheX3StatusValue) {
  auto package = maximumContentPackage();
  package.status.x3Battery = 12;
  package.status.vehicleBattery = 65;
  package.status.homeBattery = 42;

  dashboard::v3::applyDashboardV3DeviceBattery(package, 76);

  EXPECT_EQ(package.status.x3Battery, 76);
  EXPECT_EQ(package.status.vehicleBattery, 65);
  EXPECT_EQ(package.status.homeBattery, 42);
}

// --- Optional PBM artifact canvas ------------------------------------------
//
// PbmCanvas rasterizes the renderer's draw operations into a monochrome
// 528x792 pixel buffer and can write a binary PBM (P4). It exists only so a
// human can review the maximum-content layout geometry outside the firmware;
// normal test runs never write a file because the write is gated behind the
// DASHBOARD_V3_PBM_DIR environment variable in the test below.
//
// Text uses a compact built-in 7x9 test glyph table (kTestGlyphsAscii below)
// instead of a real font. These glyphs are NOT Lexend and only need to make
// labels distinguishable; production Lexend rasterization is separately
// target-compiled. Icons are bounded filled-disk placeholders for the same
// reason.

// --- 8A ---------------------------------------------------------------------
//
// Format-4 packages are drawn with the approved 8A design instead of the legacy
// bands: a black header, the rain line, the agenda, and the three lower bands
// that tile the rest of the canvas. There is no standalone hero band any more:
// the first day group heads on white, then draws its 07:00..23:00 ribbon, then
// the 58 px focus row for row 0 — the appointment in black on the same white as
// the rows below it — and then the day's remaining rows. Every later group is
// heading, ribbon and rows, with no focus row.
// These tests pin what was approved and what is easy to regress: nothing outside
// the canvas, no invented clock times, no silently dropped events, no row drawn
// twice, and values that survive where captions may be shortened.

void setAgenda8A(dashboard::v3::DashboardV3Package& package, const size_t index, const uint8_t dayOffset,
                 const uint16_t minute, const char* title, const uint16_t duration, const bool allDay = false,
                 const bool soft = false, const uint8_t dayTotal = 0, const uint8_t dayAllDay = 0) {
  dashboard::v3::AgendaRow& row = package.agenda[index];
  row.dayOffset = dayOffset;
  row.minuteOfDay = minute;
  row.durationMinutes = duration;
  row.isAllDay = allDay;
  row.isSoftBlock = soft;
  row.dayTotalCount = dayTotal;
  row.dayAllDayCount = dayAllDay;
  copyText(row.title, row.titleLength, title);
}

void setMarket8A(dashboard::v3::DashboardV3Package& package, const size_t index, const char* label,
                 const int16_t basisPoints) {
  dashboard::v3::MarketRow& row = package.markets[index];
  copyText(row.label, row.labelLength, label);
  row.changeBasisPoints = basisPoints;
}

/// The demonstration day: Sunday 2026-09-13 11:15, the first event at 19:00,
/// three day groups, four indices and a mover.
dashboard::v3::DashboardV3Package agenda8APackage() {
  dashboard::v3::DashboardV3Package package{};
  package.formatVersion = dashboard::v3::FORMAT_VERSION_8A;
  package.generatedAt = 1789298100ULL;  // 2026-09-13 11:15 UTC, a Sunday
  package.weather.currentCelsius = 13;
  package.weather.minimumCelsius = 9;
  package.weather.maximumCelsius = 17;
  package.weather.conditionIconId = 3;
  package.weather.windKilometersPerHour = 12;
  package.weather.windDirection = 4;
  package.weather.sunriseTodayMinute = 7 * 60 + 12;
  package.weather.sunsetTodayMinute = 19 * 60 + 50;
  package.weather.sunriseTomorrowMinute = 7 * 60 + 13;
  for (size_t index = 0; index < dashboard::v3::RAIN_BUCKET_COUNT; ++index) {
    package.rain[index] = index < 20 ? 0 : 1;
  }
  package.rainStartMinute = 11 * 60 + 15;
  package.heatingKnown = true;
  package.heatingAllowed = true;
  copyText(package.traffic.destination, package.traffic.destinationLength, "NAAR WERK");
  package.traffic.travelMinutes = 43;
  package.traffic.nationalCongestionKilometers = 128;
  package.traffic.classification = 2;
  package.status.x3Battery = 76;
  package.status.vehicleBattery = 49;
  package.status.homeBattery = 23;
  package.status.steps = 7850;
  package.status.stepGoal = 10000;
  package.portfolioChangeBasisPoints = 90;
  static const char* const labels[] = {"AEX", "S&P", "NDX", "BTC"};
  static const int16_t changes[] = {50, 30, 70, 120};
  static_assert(std::size(labels) == dashboard::v3::MAX_MARKETS, "one label per wire market row");
  for (size_t index = 0; index < std::size(labels); ++index) {
    setMarket8A(package, index, labels[index], changes[index]);
  }
  package.marketCount = static_cast<uint8_t>(std::size(labels));
  package.moverCount = 3;
  copyText(package.strongestMover.label, package.strongestMover.labelLength, "ASML");
  package.strongestMover.changeBasisPoints = 610;
  static const char* const chats[] = {"Chanel Kortekaas", "Richard Vaderman", "Luke De Brouwer"};
  for (size_t index = 0; index < std::size(chats); ++index) {
    copyText(package.chats[index].name, package.chats[index].nameLength, chats[index]);
    package.chats[index].unreadCount = static_cast<uint16_t>(index + 1);
    package.chats[index].lastMessageMinuteOfDay = static_cast<uint16_t>(11 * 60 + index);
  }
  package.chatCount = static_cast<uint8_t>(std::size(chats));
  package.unreadTotal = 24;
  package.quoteId = 1;
  setAgenda8A(package, 0, 0, 19 * 60, "Verhalenhuis 3", 120);
  setAgenda8A(package, 1, 1, 16 * 60, "Theaterles", 60);
  setAgenda8A(package, 2, 1, 18 * 60 + 30, "Padel", 90);
  setAgenda8A(package, 3, 2, 8 * 60, "Blok", 120, false, /*soft=*/true);
  setAgenda8A(package, 4, 2, 8 * 60, "Overleg", 45);
  setAgenda8A(package, 5, 2, 8 * 60, "Koffie", 30);
  setAgenda8A(package, 6, 2, 9 * 60 + 30, "AI doc", 60);
  package.agendaCount = 7;
  return package;
}

/// Nothing but a timestamp: the 8A bands must still hold their geometry and
/// show dashes rather than collapsing into each other.
dashboard::v3::DashboardV3Package empty8APackage() {
  dashboard::v3::DashboardV3Package package{};
  package.formatVersion = dashboard::v3::FORMAT_VERSION_8A;
  package.generatedAt = 1789298100ULL;
  package.rainKnown = false;
  package.rainStartMinute = UINT16_MAX;
  package.portfolioChangeBasisPoints = INT16_MIN;
  return package;
}

/// Every optional source at its wire maximum, including eight agenda rows over
/// four calendar days: the case where the agenda cannot draw every row and has
/// to account for the rest in words.
dashboard::v3::DashboardV3Package extreme8APackage() {
  dashboard::v3::DashboardV3Package package = agenda8APackage();
  copyText(package.traffic.destination, package.traffic.destinationLength, "NAAR AMSTERDAM Z");
  package.traffic.travelMinutes = 188;
  package.traffic.nationalCongestionKilometers = 1860;
  package.status.steps = 19999;
  package.unreadTotal = 999;
  package.portfolioChangeBasisPoints = -800;
  for (size_t index = 0; index < dashboard::v3::MAX_MARKETS; ++index) {
    setMarket8A(package, index, "All-World ET", -1234);
  }
  package.moverCount = 8;
  copyText(package.strongestMover.label, package.strongestMover.labelLength, "All-World ET");
  package.strongestMover.changeBasisPoints = -9999;
  for (size_t index = 0; index < dashboard::v3::MAX_CHATS; ++index) {
    copyText(package.chats[index].name, package.chats[index].nameLength, "Familie Kortekaas");
    package.chats[index].unreadCount = 128;
  }
  package.chatCount = static_cast<uint8_t>(dashboard::v3::MAX_CHATS);
  setAgenda8A(package, 0, 0, 8 * 60, "Eerste afspraak", UINT16_MAX);
  for (size_t index = 1; index < dashboard::v3::MAX_AGENDA_ROWS; ++index) {
    const uint8_t day = static_cast<uint8_t>(index / 2);
    const uint16_t duration = index % 3 == 0 ? UINT16_MAX : 1440;
    setAgenda8A(package, index, day, static_cast<uint16_t>(8 * 60 + index), "Kwartaalreview Fiberforce Nederl",
                duration);
  }
  package.agendaCount = static_cast<uint8_t>(dashboard::v3::MAX_AGENDA_ROWS);
  package.quoteId = 0;
  return package;
}

/// A day with an all-day event first and one event around midnight: the two
/// places a panel is tempted to claim a 00:00 that the wire never sent.
dashboard::v3::DashboardV3Package allDay8APackage() {
  dashboard::v3::DashboardV3Package package = agenda8APackage();
  setAgenda8A(package, 0, 0, 0, "Feestdag Koningsdag", UINT16_MAX, /*allDay=*/true);
  setAgenda8A(package, 1, 0, 5, "Nachtdienst", 90);
  setAgenda8A(package, 2, 0, 23 * 60 + 30, "Laat slapen", UINT16_MAX);
  package.agendaCount = 3;
  return package;
}

/// One day for the ribbon at known and unknown durations. The hero owns the
/// 08:00 event, so the list itself starts at 09:00 with "Kort", "Lang" and a
/// point that has no encoded duration.
dashboard::v3::DashboardV3Package ribbon8APackage() {
  dashboard::v3::DashboardV3Package package{};
  package.formatVersion = dashboard::v3::FORMAT_VERSION_8A;
  package.generatedAt = 1789298100ULL;
  setAgenda8A(package, 0, 0, 8 * 60, "Eerste", 30);
  setAgenda8A(package, 1, 0, 9 * 60, "Kort", 60);
  setAgenda8A(package, 2, 0, 9 * 60, "Lang", 120);
  setAgenda8A(package, 3, 0, 12 * 60, "Punt", UINT16_MAX);
  package.agendaCount = 4;
  return package;
}

/// One calendar day holding the hero and six more events. The hero owns the
/// first event, so the list below it is six rows deep: with the fixed
/// three-rows-per-day cap gone, the band must list every one of them.
dashboard::v3::DashboardV3Package singleDayAgenda8APackage() {
  dashboard::v3::DashboardV3Package package{};
  package.formatVersion = dashboard::v3::FORMAT_VERSION_8A;
  package.generatedAt = 1789298100ULL;
  setAgenda8A(package, 0, 0, 9 * 60, "Eerste", 60);
  static const char* const titles[] = {"Tweede", "Derde", "Vierde", "Vijfde", "Zesde", "Zevende"};
  for (size_t index = 0; index < std::size(titles); ++index) {
    setAgenda8A(package, index + 1, 0, static_cast<uint16_t>(9 * 60 + (index + 1) * 30), titles[index], 30);
  }
  package.agendaCount = static_cast<uint8_t>(std::size(titles) + 1);
  return package;
}

/// The hero's day plus three further days, each with two listed events. The
/// agenda band has room for the hero's group and two of the later ones, so the
/// last day rides in the standalone "+N meer" line.
dashboard::v3::DashboardV3Package threeDayAgenda8APackage() {
  dashboard::v3::DashboardV3Package package{};
  package.formatVersion = dashboard::v3::FORMAT_VERSION_8A;
  package.generatedAt = 1789298100ULL;
  setAgenda8A(package, 0, 0, 19 * 60, "Verhalenhuis 3", 120);
  static const char* const titles[] = {"Theaterles", "Padel", "Overleg", "Koffie", "Blok", "AI doc"};
  for (size_t index = 0; index < std::size(titles); ++index) {
    const uint8_t day = static_cast<uint8_t>(index / 2 + 1);
    const uint16_t minute = static_cast<uint16_t>(10 * 60 + (index % 2) * 60);
    setAgenda8A(package, index + 1, day, minute, titles[index], 30);
  }
  package.agendaCount = static_cast<uint8_t>(std::size(titles) + 1);
  return package;
}

std::vector<const Operation*> textOperationsInBand(const RecordingCanvas& canvas, const int y0, const int y1) {
  std::vector<const Operation*> found;
  for (const auto& operation : canvas.operations) {
    if (operation.kind == Operation::Kind::Text && operation.bounds.y >= y0 && operation.bounds.y < y1) {
      found.push_back(&operation);
    }
  }
  return found;
}

int countTextOperations(const RecordingCanvas& canvas, const std::string& value) {
  int count = 0;
  for (const auto& operation : canvas.operations) {
    if (operation.kind == Operation::Kind::Text && operation.text == value) ++count;
  }
  return count;
}

void expectTextFitsItsBox(const RecordingCanvas& canvas, const std::string& value, const char* what) {
  const Operation* operation = findTextOperation(canvas, value);
  ASSERT_NE(operation, nullptr) << what;
  EXPECT_GE(operation->bounds.width, nominalWidth(operation->font, value.c_str())) << what << " is clipped";
}

// The band the drawer of the day ribbon uses, found from the canvas rather than
// hard-coded, so a geometry change moves the test with it. It is the one
// full-content-width 10 px outline inside the agenda band; every day group draws
// one, and the first group's is the day the ribbon tests exercise.
const Operation* findDayRibbon(const RecordingCanvas& canvas) {
  const dashboard::v3::Agenda8ARects agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Rect) continue;
    if (operation.bounds.height != agenda.ribbonHeight) continue;
    if (operation.bounds.width != agenda.content.width) continue;
    if (operation.bounds.y < agenda.content.y) continue;
    if (operation.bounds.y + operation.bounds.height > agenda.content.y + agenda.content.height) continue;
    return &operation;
  }
  return nullptr;
}

// The focus row's reference height, pinned as a literal so these tests are not
// just re-deriving whatever the layout happens to say.
constexpr int FOCUS_ROW_8A_HEIGHT = 58;

/// The 58 px focus row the first day group draws for row 0. It is no longer a
/// drawn op — there is no black band to find any more — so it is the band the
/// day group's own metrics put between the ribbon's tick gap and the first
/// ordinary row.
dashboard::v3::Rect focusRowBounds(const dashboard::v3::Agenda8ARects& agenda) {
  return {agenda.band.x, agenda.content.y + agenda.headingHeight + agenda.ribbonHeight + agenda.ribbonGap,
          agenda.band.width, FOCUS_ROW_8A_HEIGHT};
}

TEST(DashboardV3Renderer8A, NormalPackageKeepsEveryOperationInsideTheCanvas) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);
  expectOperationsInsideCanvas(canvas);
}

TEST(DashboardV3Renderer8A, EmptyAndExtremePackagesKeepEveryOperationInsideTheCanvas) {
  RecordingCanvas emptyCanvas;
  dashboard::v3::renderDashboardV3(emptyCanvas, empty8APackage(), /*minuteOfDay=*/11 * 60 + 15);
  ASSERT_FALSE(emptyCanvas.operations.empty());
  expectOperationsInsideCanvas(emptyCanvas);

  RecordingCanvas extremeCanvas;
  dashboard::v3::renderDashboardV3(extremeCanvas, extreme8APackage(), /*minuteOfDay=*/11 * 60 + 15);
  ASSERT_FALSE(extremeCanvas.operations.empty());
  expectOperationsInsideCanvas(extremeCanvas);
}

TEST(DashboardV3Renderer8A, BandsTileTheCanvasWithTheHeroInsideTheAgenda) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  const dashboard::v3::Dashboard8ARects bands = dashboard::v3::computeDashboard8ALayout(528, 792, {});

  bool hasHeader = false;
  bool hasStandaloneHero = false;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Fill || !operation.black) continue;
    hasHeader |= operation.bounds == dashboard::v3::Rect{0, 0, 528, 62};
    hasStandaloneHero |= operation.bounds == dashboard::v3::Rect{0, 62, 528, 58};
  }
  EXPECT_TRUE(hasHeader) << "the 8A header is a black band 62 px tall";
  EXPECT_FALSE(hasStandaloneHero) << "the hero is drawn by the agenda's first group, not as a band of its own";
  // The focus row lives inside the agenda too. It draws no band of its own, so
  // it is found by its large time rather than by a black fill.
  const Operation* focusTime = findTextOperation(canvas, "19:00");
  ASSERT_NE(focusTime, nullptr) << "the first day group still draws the focus row for row 0";
  EXPECT_EQ(focusTime->font, dashboard::v3::FontRole::Hero);
  const dashboard::v3::Agenda8ARects agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
  const dashboard::v3::Rect focus = focusRowBounds(agenda);
  EXPECT_GE(focusTime->bounds.y, focus.y) << "the focus row's time sits inside the focus row";
  EXPECT_LT(focusTime->bounds.y, focus.y + focus.height);
  EXPECT_EQ(focus.height, FOCUS_ROW_8A_HEIGHT)
      << "the focus row still owns the 58 px the standalone hero band used to";
  EXPECT_LT(focus.y + focus.height, agenda.content.y + agenda.content.height)
      << "the focus row stays inside the agenda band";
  // The agenda now runs from directly under the header down to the moved rain
  // band, and the KPI band keeps its untouched 554 top.
  EXPECT_EQ(bands.hero.height, 0) << "the hero rect is a zero-height seam marker now";
  EXPECT_EQ(bands.agenda.y, 62);
  EXPECT_EQ(bands.agenda.y, bands.header.y + bands.header.height);
  EXPECT_EQ(bands.agenda.y + bands.agenda.height, 514);
  // The rain band moved directly under the agenda and still sits above the KPI
  // band, where it tiles the 40 px between them exactly.
  EXPECT_EQ(bands.rain.y, 514);
  EXPECT_EQ(bands.rain.y, bands.agenda.y + bands.agenda.height);
  EXPECT_EQ(bands.rain.y + bands.rain.height, bands.kpi.y);
  EXPECT_EQ(bands.hero.y, bands.agenda.y) << "the seam marker rides the agenda's top";
  // The bands below the agenda are separated by hairlines, so the four paper
  // bands do not read as one tall column.
  for (const int boundary : {554, 694, 728}) {
    bool rule = false;
    for (const auto& operation : canvas.operations) {
      rule |= operation.kind == Operation::Kind::Line && operation.bounds.y == boundary &&
              operation.bounds.width == 528;
    }
    EXPECT_TRUE(rule) << "no hairline at y=" << boundary;
  }
}

TEST(DashboardV3Renderer8A, HeaderCarriesDateRangeAndRefreshInOneBand) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  const Operation* date = findTextOperation(canvas, "ZO 13 SEP");
  ASSERT_NE(date, nullptr);
  EXPECT_LT(date->bounds.y + date->bounds.height, 62) << "the date sits inside the header band";
  EXPECT_FALSE(date->black) << "header text stays white on the black band";

  const Operation* caption = findTextOperation(canvas, "9\xc2\xb0 / 17\xc2\xb0 \xc2\xb7 ververst 11:15");
  ASSERT_NE(caption, nullptr) << "the range and the refresh time share the caption line";
  EXPECT_LT(caption->bounds.y + caption->bounds.height, 62);
  EXPECT_NE(findTextOperation(canvas, "13\xc2\xb0"), nullptr) << "the current reading is on the value line";
  const Operation* sun = findTextOperation(canvas, "19:50");
  ASSERT_NE(sun, nullptr) << "at 11:15 the next sun event is today's sunset";
  EXPECT_LT(sun->bounds.y + sun->bounds.height, 62) << "it shares the header's value line";
  EXPECT_NE(findTextOperation(canvas, "12"), nullptr) << "the wind speed is drawn beside its icon";

  // "ververst" moved out of the quote band into the header.
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Text) continue;
    if (operation.text.find("ververst") != std::string::npos) {
      EXPECT_LT(operation.bounds.y, 62) << "the refresh time belongs to the header now";
    }
  }
}

TEST(DashboardV3Renderer8A, RainBandShowsTheCompactOutlookAndAnAbsoluteWindow) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  const dashboard::v3::Rect band = dashboard::v3::computeDashboard8ALayout(528, 792, {}).rain;
  const auto insideBand = [&](const dashboard::v3::Rect bounds) {
    return bounds.y >= band.y && bounds.y + bounds.height <= band.y + band.height;
  };
  const auto iconInBand = [&](const uint8_t iconId) {
    for (const auto& operation : canvas.operations) {
      if (operation.kind == Operation::Kind::Icon && operation.iconId == iconId && insideBand(operation.bounds)) {
        return &operation;
      }
    }
    return static_cast<const Operation*>(nullptr);
  };

  // The moved band keeps its rain icon and its outlook, and names both ends of
  // its two-hour window explicitly: 11:15 (rainStartMinute) to 13:15.
  const Operation* icon = iconInBand(3);
  ASSERT_NE(icon, nullptr) << "the rain icon marks what the message is about";
  EXPECT_TRUE(insideBand(icon->bounds)) << "the rain icon moved with the band";
  EXPECT_NE(findTextOperation(canvas, "12:55 lichte regen"), nullptr)
      << "the outlook names the absolute clock the rain starts at";
  const Operation* start = findTextOperation(canvas, "11:15");
  const Operation* end = findTextOperation(canvas, "13:15");
  ASSERT_NE(start, nullptr) << "the window's start clock is explicit on the row";
  ASSERT_NE(end, nullptr) << "the strip's window ends at 11:15 + two hours";
  EXPECT_TRUE(insideBand(start->bounds));
  EXPECT_TRUE(insideBand(end->bounds));

  // The rain icon and the single heating badge are the only icons the row draws.
  int iconsInBand = 0;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Icon) continue;
    if (!insideBand(operation.bounds)) continue;
    ++iconsInBand;
  }
  EXPECT_EQ(iconsInBand, 2) << "the rain icon and one heating badge, nothing else";

  int shadesInRainBand = 0;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Shade) continue;
    if (!insideBand(operation.bounds)) continue;
    ++shadesInRainBand;
  }
  EXPECT_GT(shadesInRainBand, 0) << "the four wet buckets draw at least one intensity segment";
}

// The rain band's two rules. The band's own 40 px are white, the same paper the
// agenda above it is drawn on, so without a rule the two would read as one tall
// column; the divider marks where the rain sentence ends and the reserved stove
// slot begins. Both are band structure rather than content, so they are drawn
// for every 8A package and the band keeps the same shape package to package.
TEST(DashboardV3Renderer8A, RainBandIsSealedByARuleAndItsStoveSlotIsDividedOff) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  const dashboard::v3::Rect band = dashboard::v3::computeDashboard8ALayout(528, 792, {}).rain;

  // The rule sits exactly on the band's top edge and spans the whole canvas, so
  // it seals the agenda off from the band the way the lower hairlines seal the
  // KPI, markets and quote bands.
  const Operation* rule = nullptr;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Line || !operation.black) continue;
    if (operation.bounds.y != band.y || operation.bounds.height != 1) continue;
    if (operation.bounds.x != 0 || operation.bounds.width != canvas.width()) continue;
    rule = &operation;
  }
  EXPECT_NE(rule, nullptr) << "a 1 px rule spans the canvas at y=" << band.y << ", the top of the rain band";

  // The divider is the band's only vertical line. It is inset from the band's
  // top and bottom so it reads as a separator rather than as a band edge.
  const Operation* divider = nullptr;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Line || !operation.black) continue;
    if (operation.bounds.width != 1 || operation.bounds.height <= 1) continue;
    if (operation.bounds.y < band.y || operation.bounds.y + operation.bounds.height > band.y + band.height) continue;
    divider = &operation;
  }
  ASSERT_NE(divider, nullptr) << "a short 1 px vertical divider sits inside the rain band";
  EXPECT_GT(divider->bounds.y, band.y) << "the divider is inset from the band's top";
  EXPECT_LT(divider->bounds.y + divider->bounds.height, band.y + band.height)
      << "the divider is inset from the band's bottom";
  EXPECT_GT(divider->bounds.height, 8) << "the divider is a visible line, not a stray dot";
  EXPECT_LT(divider->bounds.height, band.height) << "the divider stays a short line inside the 40 px band";

  // It stands immediately before the reserved stove slot: the window's end clock
  // is entirely on its left and the flame entirely on its right, so the last
  // clock and the verdict cannot run together into one word.
  const Operation* endTime = findTextOperation(canvas, "13:15");
  const Operation* flame = findIconOperation(canvas, 14);
  ASSERT_NE(endTime, nullptr) << "the window's end clock is the value left of the divider";
  ASSERT_NE(flame, nullptr) << "the heating verdict is the reserved slot right of the divider";
  EXPECT_LE(endTime->bounds.x + endTime->bounds.width, divider->bounds.x)
      << "the end clock stays left of the divider";
  EXPECT_LE(divider->bounds.x + divider->bounds.width, flame->bounds.x) << "the flame stays right of the divider";
  EXPECT_LT(flame->bounds.x - divider->bounds.x, 8)
      << "the divider is immediately before the reserved stove slot, not somewhere up the row";

  expectOperationsInsideCanvas(canvas);
}

TEST(DashboardV3Renderer8A, UnknownRainDrawsNoStripAndNeverClaimsDry) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, empty8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  EXPECT_NE(findTextOperation(canvas, "regen -"), nullptr) << "an absent forecast says so with a dash";
  // An unknown forecast is not a heating verdict either: no flame, no strike.
  EXPECT_EQ(findIconOperation(canvas, 14), nullptr) << "unknown heating draws nothing at all";
  for (const auto& operation : canvas.operations) {
    if (operation.kind == Operation::Kind::Text) {
      EXPECT_EQ(operation.text.find("droog"), std::string::npos) << "no dry claim without data: " << operation.text;
    }
    EXPECT_NE(operation.kind, Operation::Kind::Shade) << "an absent forecast draws no strip";
  }
}

TEST(DashboardV3Renderer8A, APackageWithoutATimestampInventsNoDateAndNoCountdown) {
  auto package = agenda8APackage();
  package.generatedAt = 0;
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);

  EXPECT_NE(findTextOperation(canvas, "-"), nullptr) << "the header date is a dash";
  EXPECT_EQ(findTextOperation(canvas, "ZO 13 SEP"), nullptr);
  EXPECT_NE(findTextOperation(canvas, "hierna"), nullptr) << "the hero still says which event comes next";
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Text) continue;
    EXPECT_EQ(operation.text.find("over "), std::string::npos)
        << "a countdown needs a reference to count from: " << operation.text;
    EXPECT_EQ(operation.text.find("ververst"), std::string::npos) << operation.text;
  }
  // The agenda still says what is scheduled and how much of it; only the dates
  // it would have to derive from the timestamp become dashes.
  EXPECT_NE(findTextOperation(canvas, "- \xc2\xb7 2 AFSPRAKEN"), nullptr);
  EXPECT_NE(findTextOperation(canvas, "Theaterles"), nullptr);
}

TEST(DashboardV3Renderer8A, HeroShowsTheFirstEventTimeAndTheCountdown) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/8 * 60);

  const dashboard::v3::Agenda8ARects agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
  const dashboard::v3::Rect focus = focusRowBounds(agenda);

  const Operation* clock = findTextOperation(canvas, "19:00");
  ASSERT_NE(clock, nullptr);
  EXPECT_EQ(clock->font, dashboard::v3::FontRole::Hero) << "the next event's time is the loudest thing here";
  EXPECT_GE(clock->bounds.y, focus.y) << "the focus row's clock lives in the focus row";
  EXPECT_LT(clock->bounds.y + clock->bounds.height, focus.y + focus.height);

  const Operation* caption = findTextOperation(canvas, "hierna \xc2\xb7 over 7u45");
  ASSERT_NE(caption, nullptr) << "11:15 to 19:00 is seven hours and a quarter";
  EXPECT_LT(caption->bounds.y + caption->bounds.height, focus.y + focus.height);
  EXPECT_LT(focus.y + focus.height, agenda.content.y + agenda.content.height)
      << "the focus row stays inside the agenda";

  // The countdown is measured from the package's reference minute, so the
  // device's own clock must not move it.
  RecordingCanvas laterCanvas;
  dashboard::v3::renderDashboardV3(laterCanvas, agenda8APackage(), /*minuteOfDay=*/21 * 60);
  EXPECT_NE(findTextOperation(laterCanvas, "hierna \xc2\xb7 over 7u45"), nullptr);
}

TEST(DashboardV3Renderer8A, FocusRowDrawsItsLargeTextInBlackOnWhite) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  const dashboard::v3::Rect focus = focusRowBounds(dashboard::v3::computeAgenda8ALayout(528, 792, {}));

  // The focus row is no longer inverted: the same Hero time and Body title keep
  // their size and their places, but they are black ink on the row's white.
  const Operation* time = findTextOperation(canvas, "19:00");
  ASSERT_NE(time, nullptr);
  EXPECT_EQ(time->font, dashboard::v3::FontRole::Hero) << "the focus time keeps the Hero rung";
  EXPECT_TRUE(time->bold);
  EXPECT_TRUE(time->black) << "the focus time is black on white, not white on black";
  EXPECT_FALSE(time->dithered) << "the loudest thing on the row is solid";

  const Operation* title = findTextOperation(canvas, "Verhalenhuis 3");
  ASSERT_NE(title, nullptr);
  EXPECT_EQ(title->font, dashboard::v3::FontRole::Body) << "the title keeps the Body rung";
  EXPECT_FALSE(title->bold) << "the refinement keeps the title subordinate to the Hero time";
  EXPECT_TRUE(title->black);
  EXPECT_FALSE(title->dithered);
  EXPECT_GE(title->bounds.y, focus.y);
  EXPECT_LT(title->bounds.y, focus.y + focus.height);

  // The hairline the row splits its time and title with is black too.
  bool hairline = false;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Line || !operation.black) continue;
    if (operation.bounds.width != 1 || operation.bounds.height < 20) continue;
    if (operation.bounds.x <= focus.x || operation.bounds.x >= focus.x + focus.width) continue;
    if (operation.bounds.y != focus.y + 12) continue;
    hairline = true;
  }
  EXPECT_TRUE(hairline) << "a black hairline splits the focus time from its title";

  // The caption stays the secondary line: dithered, never solid black.
  const Operation* caption = findTextOperation(canvas, "hierna \xc2\xb7 over 7u45");
  ASSERT_NE(caption, nullptr);
  EXPECT_EQ(caption->font, dashboard::v3::FontRole::Micro);
  EXPECT_TRUE(caption->dithered) << "the caption is dithered, not a solid black line";
}

TEST(DashboardV3Renderer8A, FocusRowCaptionLeadsWithTheLocationWhenTheWireCarriesOne) {
  auto package = agenda8APackage();
  copyText(package.agenda[0].detail, package.agenda[0].detailLength, "Teams");
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);

  // 11:15 to 19:00 is seven hours and a quarter; the appointment's location
  // leads that countdown, separated by the same middle dot the day headings use.
  const Operation* caption = findTextOperation(canvas, "Teams \xc2\xb7 over 7u45");
  ASSERT_NE(caption, nullptr) << "the location is the caption prefix, ahead of the countdown";
  EXPECT_TRUE(caption->dithered) << "the location caption is still the secondary line";
  EXPECT_EQ(findTextOperation(canvas, "hierna \xc2\xb7 over 7u45"), nullptr)
      << "the location replaces the bare \"hierna\" prefix";

  // On a later day the location still leads, and the countdown it leads is the
  // same day-aware one the row would have printed on its own.
  auto tomorrow = agenda8APackage();
  setAgenda8A(tomorrow, 0, 1, 16 * 60, "Theaterles", 60);  // the first event is tomorrow at 16:00
  copyText(tomorrow.agenda[0].detail, tomorrow.agenda[0].detailLength, "Teams");
  RecordingCanvas tomorrowCanvas;
  dashboard::v3::renderDashboardV3(tomorrowCanvas, tomorrow, /*minuteOfDay=*/11 * 60 + 15);
  EXPECT_NE(findTextOperation(tomorrowCanvas, "Teams \xc2\xb7 over 1d 4u45"), nullptr)
      << "the location leads the day-aware countdown, not just the today case";
}

TEST(DashboardV3Renderer8A, FocusRowCaptionFallsBackToHiernaWhenTheEventHasNoLocation) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  // No location on the wire: the row keeps the honest "hierna · ..." it always
  // had, so an appointment without a room still says what it is counting to.
  const Operation* caption = findTextOperation(canvas, "hierna \xc2\xb7 over 7u45");
  ASSERT_NE(caption, nullptr);
  EXPECT_TRUE(caption->dithered) << "a caption with no location is still the secondary line";

  // A detail that is only padding is not a location either: the phone trims it,
  // before sending, but a stray space on the wire must not become a leading gap
  // or a bare middle dot.
  auto padded = agenda8APackage();
  copyText(padded.agenda[0].detail, padded.agenda[0].detailLength, "   ");
  RecordingCanvas paddedCanvas;
  dashboard::v3::renderDashboardV3(paddedCanvas, padded, /*minuteOfDay=*/11 * 60 + 15);
  EXPECT_NE(findTextOperation(paddedCanvas, "hierna \xc2\xb7 over 7u45"), nullptr)
      << "whitespace-only padding is not a location";
  for (const auto& operation : paddedCanvas.operations) {
    if (operation.kind != Operation::Kind::Text) continue;
    EXPECT_NE(operation.text.rfind("\xc2\xb7", 0), 0U) << "a caption never starts at the separator: " << operation.text;
  }
}

TEST(DashboardV3Renderer8A, HeroCountdownNeverRunsBackwardsOnAStalePackage) {
  auto package = agenda8APackage();
  package.generatedAt = 1789298100ULL + 10 * 3600;  // composed at 21:15, the 19:00 event has passed
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/21 * 60 + 15);

  EXPECT_NE(findTextOperation(canvas, "hierna \xc2\xb7 nu"), nullptr);
  for (const auto& operation : canvas.operations) {
    if (operation.kind == Operation::Kind::Text) {
      EXPECT_EQ(operation.text.find("over -"), std::string::npos) << operation.text;
    }
  }
}

TEST(DashboardV3Renderer8A, HeroAllDayEventSaysHeleDagWithoutAClockTime) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, allDay8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  const Operation* hero = findTextOperation(canvas, "hele dag");
  ASSERT_NE(hero, nullptr) << "an all-day event has no clock, so the hero says what it is";
  EXPECT_EQ(hero->font, dashboard::v3::FontRole::Hero);
  EXPECT_NE(findTextOperation(canvas, "hierna \xc2\xb7 hele dag"), nullptr);
  EXPECT_EQ(findTextOperation(canvas, "00:00"), nullptr) << "the wire never said 00:00";
  EXPECT_NE(findTextOperation(canvas, "00:05"), nullptr) << "a real five past midnight is still shown";
  EXPECT_NE(findTextOperation(canvas, "23:30"), nullptr);
}

TEST(DashboardV3Renderer8A, HeroNamesTheDayWhenTheFirstEventIsNotToday) {
  auto package = agenda8APackage();
  setAgenda8A(package, 0, 1, 16 * 60, "Theaterles", 60);  // first event is tomorrow at 16:00
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);

  EXPECT_NE(findTextOperation(canvas, "morgen \xc2\xb7 over 1d 4u45"), nullptr)
      << "11:15 today to 16:00 tomorrow, and the caption says which day that is";
  EXPECT_EQ(findTextOperation(canvas, "hierna \xc2\xb7 over 1d 4u45"), nullptr)
      << "\"hierna\" alone would hide that this is not today";

  // Beyond tomorrow the caption names the weekday instead of counting days.
  auto laterPackage = agenda8APackage();
  setAgenda8A(laterPackage, 0, 2, 8 * 60, "AI doc", 60);
  RecordingCanvas laterCanvas;
  dashboard::v3::renderDashboardV3(laterCanvas, laterPackage, /*minuteOfDay=*/11 * 60 + 15);
  EXPECT_NE(findTextOperation(laterCanvas, "di 15 \xc2\xb7 over 1d 20u45"), nullptr)
      << "11:15 Sunday to 08:00 Tuesday is one day and twenty hours";
}

TEST(DashboardV3Renderer8A, HeroOwnsTheFirstEventAndTheListDoesNotRepeatIt) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  // The focus row shows the first event; the day's list rows below it start at
  // the second one and must not draw the hero's event again.
  const dashboard::v3::Agenda8ARects agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
  const int listTop = focusRowBounds(agenda).y + FOCUS_ROW_8A_HEIGHT;
  EXPECT_EQ(countTextOperations(canvas, "Verhalenhuis 3"), 1) << "once in the hero, never again in the list";
  for (const Operation* operation : textOperationsInBand(canvas, listTop, 514)) {
    EXPECT_NE(operation->text, "Verhalenhuis 3") << "the hero's event is not repeated in the agenda list";
  }

  // The hero's own day keeps its later events in the list; only the first one
  // moved up into the hero.
  RecordingCanvas allDayCanvas;
  dashboard::v3::renderDashboardV3(allDayCanvas, allDay8APackage(), /*minuteOfDay=*/11 * 60 + 15);
  EXPECT_EQ(countTextOperations(allDayCanvas, "Feestdag Koningsdag"), 1);
  const std::vector<const Operation*> rows = textOperationsInBand(allDayCanvas, listTop, 514);
  int listed = 0;
  for (const Operation* operation : rows) {
    if (operation->text == "Nachtdienst" || operation->text == "Laat slapen") ++listed;
  }
  EXPECT_EQ(listed, 2) << "the hero's day still lists the events that follow it";
}

TEST(DashboardV3Renderer8A, AgendaRowsShowTheEncodedDurationAndNotTheLegacyDetail) {
  auto package = agenda8APackage();
  // The legacy free-text detail often holds a room or a platform. On the 8A row
  // it sits right where a duration would, so it is not drawn at all.
  copyText(package.agenda[2].detail, package.agenda[2].detailLength, "MAARSSEN");
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);

  EXPECT_NE(findTextOperation(canvas, "1u30"), nullptr) << "the encoded 90 minutes are drawn as 1u30";
  EXPECT_NE(findTextOperation(canvas, "45m"), nullptr);
  EXPECT_NE(findTextOperation(canvas, "30m"), nullptr);
  EXPECT_NE(findTextOperation(canvas, "2u"), nullptr);
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Text) continue;
    EXPECT_EQ(operation.text.find("MAARSSEN"), std::string::npos)
        << "a free-text detail must not be mistaken for a duration: " << operation.text;
  }
  const Operation* detail = findTextOperation(canvas, "1u30");
  ASSERT_NE(detail, nullptr);
  EXPECT_GT(detail->bounds.x, 440) << "the duration sits in its own right-hand column";
}

TEST(DashboardV3Renderer8A, RibbonDrawsRealDurationsAndAPointForAnUnknownOne) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, ribbon8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  const Operation* ribbon = findDayRibbon(canvas);
  ASSERT_NE(ribbon, nullptr);
  const dashboard::v3::Rect bounds = ribbon->bounds;
  const int innerLeft = bounds.x + 1;
  const int innerWidth = bounds.width - 2;
  const auto xForMinute = [&](const int minute) {
    return innerLeft + (minute - 7 * 60) * innerWidth / (23 * 60 - 7 * 60);
  };

  int sixty = 0;
  int oneTwenty = 0;
  int point = 0;
  int pointX = -1;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Fill || operation.bounds.height != bounds.height - 2) continue;
    if (operation.bounds.y != bounds.y + 1) continue;  // stays inside the outline
    if (operation.bounds.x < innerLeft || operation.bounds.x > bounds.x + bounds.width) continue;
    if (operation.bounds.width == 2) {
      ++point;
      pointX = operation.bounds.x;
    }
    if (operation.bounds.width >= 25 && operation.bounds.width <= 35) sixty = operation.bounds.width;
    if (operation.bounds.width >= 55 && operation.bounds.width <= 70) oneTwenty = operation.bounds.width;
  }

  // 07:00..23:00 is sixteen hours across 498 px: a quarter of an hour is about
  // 7.8 px, so 60 and 120 minutes must draw as intervals, not as marks.
  EXPECT_NEAR(sixty * 2, oneTwenty, 3) << "the ribbon is proportional to duration";
  EXPECT_EQ(sixty, xForMinute(10 * 60) - xForMinute(9 * 60));
  EXPECT_EQ(oneTwenty, xForMinute(11 * 60) - xForMinute(9 * 60));
  EXPECT_EQ(point, 1) << "an unknown duration is one mark";
  EXPECT_EQ(pointX, xForMinute(12 * 60)) << "the mark sits where the event starts";
}

TEST(DashboardV3Renderer8A, RibbonDithersSoftBlocksAndTicksTheHours) {
  auto package = ribbon8APackage();
  // One of the three listed rows is a soft block, so the day still draws every
  // row it has and the dithered interval is on the strip.
  package.agenda[2].isSoftBlock = true;  // "Lang"
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);

  const Operation* ribbon = findDayRibbon(canvas);
  ASSERT_NE(ribbon, nullptr);
  int shaded = 0;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Shade) continue;
    // The ordinary rows' own stripes are shaded too, so only the intervals that
    // sit inside the outlined strip count as the ribbon's own ink.
    if (operation.bounds.y < ribbon->bounds.y ||
        operation.bounds.y + operation.bounds.height > ribbon->bounds.y + ribbon->bounds.height) {
      continue;
    }
    ++shaded;
    EXPECT_GE(operation.bounds.x, ribbon->bounds.x);
    EXPECT_LE(operation.bounds.x + operation.bounds.width, ribbon->bounds.x + ribbon->bounds.width);
  }
  EXPECT_EQ(shaded, 1) << "the soft block is the only dithered interval";

  int ticks = 0;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Line) continue;
    if (operation.bounds.y != ribbon->bounds.y + ribbon->bounds.height || operation.bounds.height != 4) continue;
    ++ticks;
  }
  EXPECT_EQ(ticks, 3) << "08:00, 12:00 and 18:00 get a tick under the strip";

  // Each tick carries the hour it stands for, and the label sits below the tick
  // but above the first agenda row, so it never collides with either.
  const int stripBottom = ribbon->bounds.y + ribbon->bounds.height;
  for (const char* const hour : {"8", "12", "18"}) {
    const Operation* hourLabel = findTextOperation(canvas, hour);
    ASSERT_NE(hourLabel, nullptr) << "the tick labels its hour: " << hour;
    EXPECT_GT(hourLabel->bounds.y, stripBottom) << hour << " is below the strip";
    EXPECT_EQ(hourLabel->align, dashboard::v3::TextAlign::Center);
  }
  // The first listed row, so the hour labels can be checked against the rows
  // that follow them.
  const Operation* firstRow = findTextOperation(canvas, "Kort");
  ASSERT_NE(firstRow, nullptr);
  for (const char* const hour : {"8", "12", "18"}) {
    const Operation* hourLabel = findTextOperation(canvas, hour);
    ASSERT_NE(hourLabel, nullptr);
    EXPECT_LE(hourLabel->bounds.y + hourLabel->bounds.height, firstRow->bounds.y)
        << "the hour label stays clear of the day's rows";
  }
}

TEST(DashboardV3Renderer8A, RibbonClipsAnIntervalThatRunsPastTheWindow) {
  auto package = ribbon8APackage();
  setAgenda8A(package, 3, 0, 22 * 60 + 30, "Nacht", 240);  // 22:30 for four hours
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);

  const Operation* ribbon = findDayRibbon(canvas);
  ASSERT_NE(ribbon, nullptr);
  const dashboard::v3::Rect bounds = ribbon->bounds;
  const int innerLeft = bounds.x + 1;
  const int innerWidth = bounds.width - 2;
  const int windowMinutes = 23 * 60 - 7 * 60;
  const auto xForMinute = [&](const int minute) {
    return innerLeft + (minute - 7 * 60) * innerWidth / windowMinutes;
  };

  bool found = false;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Fill) continue;
    if (operation.bounds.y != bounds.y + 1 || operation.bounds.height != bounds.height - 2) continue;
    if (operation.bounds.x != xForMinute(22 * 60 + 30)) continue;
    found = true;
    // Half an hour is all of the four-hour block that still fits before 23:00,
    // and the mark stops at the strip's own edge.
    EXPECT_EQ(operation.bounds.width, xForMinute(23 * 60) - xForMinute(22 * 60 + 30));
    EXPECT_LE(operation.bounds.x + operation.bounds.width, innerLeft + innerWidth);
  }
  EXPECT_TRUE(found) << "the late event is clipped to the window, not dropped";
}

/// One calendar day holding `count` half-hourly appointments from 09:00, each
/// with its own title. `count == 1` is the single-appointment case, `count == 6`
/// the half dozen and `count == 11` a day wider than the old three-row cap.
dashboard::v3::DashboardV3Package appointmentCount8APackage(const size_t count) {
  dashboard::v3::DashboardV3Package package{};
  package.formatVersion = dashboard::v3::FORMAT_VERSION_DAY_TOTALS;
  package.generatedAt = 1789298100ULL;
  for (size_t index = 0; index < count; ++index) {
    char title[dashboard::v3::MAX_AGENDA_TITLE_BYTES + 1];
    std::snprintf(title, sizeof(title), "AFSPRAAK %u", static_cast<unsigned>(index + 1));
    setAgenda8A(package, index, 0, static_cast<uint16_t>(9 * 60 + index * 30), title, 30, false, false,
                static_cast<uint8_t>(count), 0);
  }
  package.agendaCount = static_cast<uint8_t>(count);
  return package;
}

/// Two appointments that start at the same minute: a double booking the wire
/// allows, and both must keep their own row.
dashboard::v3::DashboardV3Package sameStart8APackage() {
  dashboard::v3::DashboardV3Package package{};
  package.formatVersion = dashboard::v3::FORMAT_VERSION_DAY_TOTALS;
  package.generatedAt = 1789298100ULL;
  setAgenda8A(package, 0, 0, 9 * 60, "Eerste", 60, false, false, 2, 0);
  setAgenda8A(package, 1, 0, 9 * 60, "Tweede", 30, false, false, 2, 0);
  package.agendaCount = 2;
  return package;
}

/// A full-width wire title (31 of the 32 bytes the field carries), the longest
/// thing the ordinary rows and the focus row ever have to set.
constexpr const char* kLongAgendaTitle8A = "Kwartaalreview Fiberforce Nederl";

/// The ordinary rows' stripes: the first ordinary row of a day is paper and
/// every second one after it carries the quarter-tone. The focus row, the day
/// headings, the ribbon and the "+N meer" line never take a stripe.
TEST(DashboardV3Renderer8A, OrdinaryRowsAlternateAQuarterToneStripeAndResetEveryDay) {
  const auto stripesIn = [](const RecordingCanvas& canvas) {
    const dashboard::v3::Agenda8ARects agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
    std::vector<dashboard::v3::Rect> stripes;
    for (const auto& [bounds, level] : canvas.shadeOps) {
      // The rain segments are Quarter-tone too, so only a stripe as tall as a
      // row counts; the ribbon's soft blocks are Half.
      if (level != dashboard::v3::Shade::Quarter) continue;
      if (bounds.height != agenda.rowHeight) continue;
      stripes.push_back(bounds);
    }
    return stripes;
  };
  const auto stripeUnder = [&](const RecordingCanvas& canvas, const char* title) {
    const dashboard::v3::Agenda8ARects agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
    const Operation* text = findTextOperation(canvas, title);
    EXPECT_NE(text, nullptr) << title;
    if (text == nullptr) return false;
    for (const dashboard::v3::Rect& stripe : stripesIn(canvas)) {
      if (text->bounds.y < stripe.y || text->bounds.y >= stripe.y + stripe.height) continue;
      EXPECT_EQ(stripe.x, agenda.content.x) << "a stripe spans the row's full width";
      EXPECT_EQ(stripe.width, agenda.content.width);
      EXPECT_NE(stripe.y, focusRowBounds(agenda).y) << "the focus row is never striped";
      return true;
    }
    return false;
  };

  // Three calendar days: Sunday holds only the focus row, Monday two rows and
  // Tuesday four, so the pattern has to reset at every day heading.
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);
  EXPECT_EQ(stripesIn(canvas).size(), 3u) << "one stripe per second row of a day, nowhere else";
  EXPECT_FALSE(stripeUnder(canvas, "Verhalenhuis 3")) << "the focus row is paper";
  EXPECT_FALSE(stripeUnder(canvas, "Theaterles")) << "the first ordinary row of a day is paper";
  EXPECT_TRUE(stripeUnder(canvas, "Padel")) << "the second ordinary row carries the quarter-tone";
  // Tuesday resets: "Koffie" is paper even though "Overleg" two rows above it is
  // not, and the heading above them does not carry a stripe.
  EXPECT_FALSE(stripeUnder(canvas, "Blok"));
  EXPECT_TRUE(stripeUnder(canvas, "Overleg"));
  EXPECT_FALSE(stripeUnder(canvas, "Koffie"));
  EXPECT_TRUE(stripeUnder(canvas, "AI doc"));
  EXPECT_EQ(findTextOperation(canvas, "+4 meer"), nullptr) << "no summary line takes a stripe here";

  // On the hero's own day the row directly after the focus row is the first
  // ordinary row, so it is paper.
  RecordingCanvas singleDayCanvas;
  dashboard::v3::renderDashboardV3(singleDayCanvas, ribbon8APackage(), /*minuteOfDay=*/11 * 60 + 15);
  EXPECT_FALSE(stripeUnder(singleDayCanvas, "Kort")) << "the first row after the focus row is paper";
  EXPECT_TRUE(stripeUnder(singleDayCanvas, "Lang"));
  EXPECT_FALSE(stripeUnder(singleDayCanvas, "Punt"));
  EXPECT_EQ(singleDayCanvas.shadeOps.size(), 1u) << "with no soft block the only shade in the frame is the row stripe";
}

/// The ordinary rows' columns. The clock is right-aligned in its own column and
/// bolder than the title, the title starts on one fixed x, the duration is
/// right-aligned against the row's own right edge, and the vertical ruler that
/// used to split the clock from the title is gone.
TEST(DashboardV3Renderer8A, OrdinaryRowsAlignTheirColumnsAndDrawNoVerticalRuler) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, ribbon8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  const dashboard::v3::Agenda8ARects agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
  const int listTop = focusRowBounds(agenda).y + FOCUS_ROW_8A_HEIGHT;
  const int rowRight = agenda.content.x + agenda.content.width;

  const Operation* kort = findTextOperation(canvas, "Kort");
  const Operation* lang = findTextOperation(canvas, "Lang");
  const Operation* punt = findTextOperation(canvas, "Punt");
  ASSERT_NE(kort, nullptr);
  ASSERT_NE(lang, nullptr);
  ASSERT_NE(punt, nullptr);
  for (const Operation* row : {kort, lang, punt}) {
    EXPECT_EQ(row->bounds.x, agenda.titleX) << "the title column has one fixed left edge";
    EXPECT_FALSE(row->bold) << "the title is lighter than its own clock";
    EXPECT_LE(row->bounds.x + row->bounds.width, rowRight) << "a title box never runs past the row";
  }

  const Operation* clock = nullptr;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Text || operation.text != "09:00") continue;
    if (operation.bounds.y < listTop) continue;
    clock = &operation;
  }
  ASSERT_NE(clock, nullptr) << "the row's own clock is drawn";
  EXPECT_EQ(clock->align, dashboard::v3::TextAlign::Right) << "the clock column is right-aligned";
  EXPECT_TRUE(clock->bold) << "the clock carries more weight than the title";
  EXPECT_EQ(clock->bounds.x + clock->bounds.width, agenda.content.x + agenda.timeColumnWidth)
      << "the clock's right edge is the time column's edge";

  const Operation* duration = findTextOperation(canvas, "1u");
  ASSERT_NE(duration, nullptr) << "the encoded 60 minutes are drawn as 1u";
  EXPECT_EQ(duration->align, dashboard::v3::TextAlign::Right);
  EXPECT_EQ(duration->bounds.x + duration->bounds.width, rowRight)
      << "the duration column is right-aligned against the row's edge";
  EXPECT_LE(kort->bounds.x + kort->bounds.width, duration->bounds.x)
      << "the title is bounded before the duration column";

  // The only tall vertical line inside the band is the focus row's own hairline.
  int tallLines = 0;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Line || operation.bounds.width != 1) continue;
    if (operation.bounds.height <= 4) continue;  // the ribbon's hour ticks are short
    if (operation.bounds.y < agenda.band.y || operation.bounds.y >= agenda.band.y + agenda.band.height) continue;
    ++tallLines;
  }
  EXPECT_EQ(tallLines, 1) << "the ordinary rows draw no vertical ruler";
}

/// One, six and eleven appointments: every row is drawn exactly once, the stripe
/// pattern follows the count, and nothing leaves the canvas.
TEST(DashboardV3Renderer8A, OneSixAndElevenAppointmentDaysStayInsideTheCanvas) {
  const auto rowStripes = [](const RecordingCanvas& canvas) {
    const dashboard::v3::Agenda8ARects agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
    int stripes = 0;
    for (const auto& [bounds, level] : canvas.shadeOps) {
      if (level != dashboard::v3::Shade::Quarter || bounds.height != agenda.rowHeight) continue;
      if (bounds.y < agenda.content.y || bounds.y >= agenda.content.y + agenda.content.height) continue;
      ++stripes;
    }
    return stripes;
  };

  const struct {
    size_t count;
    int stripes;
  } cases[] = {{1, 0}, {6, 2}, {11, 5}};
  for (const auto& testCase : cases) {
    RecordingCanvas canvas;
    dashboard::v3::renderDashboardV3(canvas, appointmentCount8APackage(testCase.count), /*minuteOfDay=*/8 * 60);
    expectOperationsInsideCanvas(canvas);
    for (size_t index = 1; index <= testCase.count; ++index) {
      char title[dashboard::v3::MAX_AGENDA_TITLE_BYTES + 1];
      std::snprintf(title, sizeof(title), "AFSPRAAK %u", static_cast<unsigned>(index));
      EXPECT_EQ(countTextOperations(canvas, title), 1)
          << "a day of " << testCase.count << " appointments draws " << title << " exactly once";
    }
    EXPECT_EQ(rowStripes(canvas), testCase.stripes) << "a day of " << testCase.count << " appointments";
    for (const Operation* operation : textOperationsInBand(canvas, 179, 514)) {
      EXPECT_EQ(operation->text.find("meer"), std::string::npos)
          << "every row fits, so nothing is summarised: " << operation->text;
    }
  }
}

/// A full-width title is bounded before the duration column instead of running
/// under it, and the drawn string still fits the box it was given.
TEST(DashboardV3Renderer8A, LongTitlesAreBoundedBeforeTheDurationColumn) {
  dashboard::v3::DashboardV3Package package{};
  package.formatVersion = dashboard::v3::FORMAT_VERSION_DAY_TOTALS;
  package.generatedAt = 1789298100ULL;
  setAgenda8A(package, 0, 0, 9 * 60, "Eerste", 30, false, false, 2, 0);
  setAgenda8A(package, 1, 0, 10 * 60, kLongAgendaTitle8A, 60, false, false, 2, 0);
  package.agendaCount = 2;

  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);

  const dashboard::v3::Agenda8ARects agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
  const Operation* title = findTextOperation(canvas, kLongAgendaTitle8A);
  const Operation* duration = findTextOperation(canvas, "1u");
  ASSERT_NE(title, nullptr) << "the long title is drawn";
  ASSERT_NE(duration, nullptr) << "the duration keeps its own column beside it";
  EXPECT_EQ(title->bounds.x, agenda.titleX);
  EXPECT_LE(title->bounds.x + title->bounds.width, duration->bounds.x)
      << "the title box ends before the duration column starts";
  expectTextFitsItsBox(canvas, kLongAgendaTitle8A, "the long title is not clipped");
  expectOperationsInsideCanvas(canvas);
}

/// The focus row is unchanged in footprint and behaviour: the Hero clock stays
/// the loudest thing on the row, the title stays subordinate, a full-width title
/// is bounded rather than clipped, and the second line stays the quiet Micro
/// caption with the countdown.
TEST(DashboardV3Renderer8A, FocusRowKeepsItsHeroClockAndBoundsALongTitle) {
  dashboard::v3::DashboardV3Package package{};
  package.formatVersion = dashboard::v3::FORMAT_VERSION_DAY_TOTALS;
  package.generatedAt = 1789298100ULL;
  setAgenda8A(package, 0, 0, 19 * 60, kLongAgendaTitle8A, 60, false, false, 1, 0);
  package.agendaCount = 1;

  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);

  const dashboard::v3::Agenda8ARects agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
  const dashboard::v3::Rect focus = focusRowBounds(agenda);
  const Operation* clock = findTextOperation(canvas, "19:00");
  ASSERT_NE(clock, nullptr);
  EXPECT_EQ(clock->font, dashboard::v3::FontRole::Hero) << "the event time is the loudest thing here";
  EXPECT_TRUE(clock->bold);
  EXPECT_TRUE(clock->black);
  const Operation* title = findTextOperation(canvas, kLongAgendaTitle8A);
  ASSERT_NE(title, nullptr);
  EXPECT_EQ(title->font, dashboard::v3::FontRole::Body) << "the title keeps the Body rung";
  EXPECT_FALSE(title->bold) << "the title is less dominant than the time";
  EXPECT_GT(title->bounds.x, clock->bounds.x + clock->bounds.width) << "the title follows the time's hairline";
  EXPECT_LE(title->bounds.x + title->bounds.width, agenda.band.x + agenda.band.width)
      << "the title box is bounded by the band";
  EXPECT_GE(title->bounds.y, focus.y);
  EXPECT_LT(title->bounds.y, focus.y + focus.height);
  expectTextFitsItsBox(canvas, kLongAgendaTitle8A, "the focus title is not clipped");

  const Operation* caption = findTextOperation(canvas, "hierna \xc2\xb7 over 7u45");
  ASSERT_NE(caption, nullptr) << "the caption still leads with the day reference and the countdown";
  EXPECT_EQ(caption->font, dashboard::v3::FontRole::Micro);
  EXPECT_TRUE(caption->dithered) << "the caption is the secondary line, not a solid black one";
  EXPECT_LT(caption->bounds.y + caption->bounds.height, focus.y + focus.height);
  expectOperationsInsideCanvas(canvas);
}

/// Two appointments starting at the same minute are a double booking, not a
/// duplicate row: each keeps its own clock and its own title.
TEST(DashboardV3Renderer8A, TwoAppointmentsAtTheSameMinuteEachKeepTheirRow) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, sameStart8APackage(), /*minuteOfDay=*/8 * 60);

  EXPECT_EQ(countTextOperations(canvas, "Eerste"), 1) << "the first is the focus row";
  EXPECT_EQ(countTextOperations(canvas, "Tweede"), 1) << "the second keeps its own ordinary row";
  EXPECT_EQ(countTextOperations(canvas, "09:00"), 2) << "the Hero clock and the row's own clock";
  const dashboard::v3::Agenda8ARects agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
  const dashboard::v3::Rect focus = focusRowBounds(agenda);
  const Operation* rowTitle = findTextOperation(canvas, "Tweede");
  ASSERT_NE(rowTitle, nullptr);
  EXPECT_GE(rowTitle->bounds.y, focus.y + focus.height) << "the second booking is listed below the focus row";
  expectOperationsInsideCanvas(canvas);
}

/// The 08:00, 12:00 and 18:00 ticks land on the same x in every day group,
/// because the ribbon is a fixed window rather than a range derived from the
/// day's own events.
TEST(DashboardV3Renderer8A, RibbonTicksShareTheSameXOnEveryDay) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  const dashboard::v3::Agenda8ARects agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
  const dashboard::v3::Rect inner{agenda.content.x + 1, 0, agenda.content.width - 2, agenda.ribbonHeight};
  for (const char* const hour : {"8", "12", "18"}) {
    int count = 0;
    int firstX = -1;
    for (const auto& operation : canvas.operations) {
      if (operation.kind != Operation::Kind::Text || operation.text != hour) continue;
      // The values elsewhere on the panel can read "8" or "12" too; the tick is
      // the one inside the agenda band, under a day's heading.
      if (operation.bounds.y < agenda.content.y + agenda.headingHeight) continue;
      if (operation.bounds.y >= agenda.band.y + agenda.band.height) continue;
      ++count;
      if (firstX < 0) firstX = operation.bounds.x;
      EXPECT_EQ(operation.bounds.x, firstX) << "the " << hour << ":00 tick keeps one x on every day";
      EXPECT_EQ(operation.align, dashboard::v3::TextAlign::Center);
    }
    EXPECT_EQ(count, 3) << "one tick per day group for " << hour;
    const int tickX =
        dashboard::v3::agendaRibbonX(inner, std::atoi(hour) * 60, agenda.ribbonStartMinute, agenda.ribbonEndMinute);
    EXPECT_EQ(firstX + 12, tickX) << "the " << hour << ":00 label is centred on its tick";
  }
}

/// The heating badge is exactly one icon and no text: a flame when the stove may
/// go on, the same flame struck through with a strong diagonal when it may not,
/// and nothing at all when the package does not say.
TEST(DashboardV3Renderer8A, HeatingBadgeIsASingleIconWithNoText) {
  const dashboard::v3::Rect band = dashboard::v3::computeDashboard8ALayout(528, 792, {}).rain;
  const dashboard::v3::Rect slot{band.x + band.width - dashboard::v3::DASHBOARD8A_PAD - 24, band.y, 24, band.height};
  const auto strikeFillsIn = [&](const RecordingCanvas& canvas) {
    int count = 0;
    for (const auto& operation : canvas.operations) {
      if (operation.kind != Operation::Kind::Fill || !operation.black) continue;
      if (operation.bounds.x < slot.x || operation.bounds.x + operation.bounds.width > slot.x + slot.width) continue;
      if (operation.bounds.y < band.y || operation.bounds.y + operation.bounds.height > band.y + band.height) continue;
      ++count;
    }
    return count;
  };
  const auto heatingTextIn = [&](const RecordingCanvas& canvas) {
    for (const auto& operation : canvas.operations) {
      if (operation.kind != Operation::Kind::Text) continue;
      if (operation.text.find("STOKEN") != std::string::npos) return operation.text;
      if (operation.text.find("VERWARMING") != std::string::npos) return operation.text;
    }
    return std::string{};
  };

  RecordingCanvas allowed;
  dashboard::v3::renderDashboardV3(allowed, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);
  const Operation* flame = findIconOperation(allowed, 14);
  ASSERT_NE(flame, nullptr) << "the stove verdict is one icon";
  EXPECT_TRUE(flame->black);
  EXPECT_GE(flame->bounds.x, slot.x);
  EXPECT_LE(flame->bounds.x + flame->bounds.width, slot.x + slot.width);
  EXPECT_TRUE(heatingTextIn(allowed).empty()) << "the badge carries no text: " << heatingTextIn(allowed);
  EXPECT_EQ(strikeFillsIn(allowed), 0) << "an allowed stove is not struck through";

  auto disallowed = agenda8APackage();
  disallowed.heatingAllowed = false;
  RecordingCanvas struck;
  dashboard::v3::renderDashboardV3(struck, disallowed, /*minuteOfDay=*/11 * 60 + 15);
  ASSERT_NE(findIconOperation(struck, 14), nullptr) << "the struck badge is still the flame";
  EXPECT_TRUE(heatingTextIn(struck).empty());
  std::vector<dashboard::v3::Rect> strikeRows;
  for (const auto& operation : struck.operations) {
    if (operation.kind != Operation::Kind::Fill || !operation.black) continue;
    if (operation.bounds.x < slot.x || operation.bounds.x + operation.bounds.width > slot.x + slot.width) continue;
    if (operation.bounds.y < band.y || operation.bounds.y + operation.bounds.height > band.y + band.height) continue;
    strikeRows.push_back(operation.bounds);
  }
  EXPECT_GE(strikeRows.size(), 8u) << "the strike is a strong diagonal, not a hairline";
  EXPECT_LE(strikeRows.size(), 24u) << "and it stays inside the icon it marks";
  ASSERT_FALSE(strikeRows.empty());
  for (const dashboard::v3::Rect& row : strikeRows) {
    EXPECT_LE(row.width, 4) << "the strike is a band, not a filled block";
    EXPECT_EQ(row.height, 1);
  }
  EXPECT_LT(strikeRows.front().x, strikeRows.back().x) << "the mark runs down and across, not straight";

  auto unknown = agenda8APackage();
  unknown.heatingKnown = false;
  RecordingCanvas none;
  dashboard::v3::renderDashboardV3(none, unknown, /*minuteOfDay=*/11 * 60 + 15);
  EXPECT_EQ(findIconOperation(none, 14), nullptr) << "an unknown stove draws nothing";
  EXPECT_TRUE(heatingTextIn(none).empty());
  EXPECT_EQ(strikeFillsIn(none), 0);
  int iconsInSlot = 0;
  for (const auto& operation : none.operations) {
    if (operation.kind != Operation::Kind::Icon) continue;
    if (operation.bounds.x < slot.x) continue;
    if (operation.bounds.y < band.y || operation.bounds.y + operation.bounds.height > band.y + band.height) continue;
    ++iconsInSlot;
  }
  EXPECT_EQ(iconsInSlot, 0) << "no check, no exclamation mark, no second symbol";
}

TEST(DashboardV3Renderer8A, DayHeadingsCountTheHeroDayAndEveryEncodedEvent) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  // The hero's own day still heads the first group with its exact count, even
  // though the only event it has is the one the hero band carries.
  const Operation* sunday = findTextOperation(canvas, "ZO 13 SEP \xc2\xb7 1 AFSPRAAK");
  ASSERT_NE(sunday, nullptr) << "the hero's day gets a heading like any other";
  EXPECT_TRUE(sunday->black);
  EXPECT_EQ(sunday->bounds.y, dashboard::v3::computeAgenda8ALayout(528, 792, {}).content.y)
      << "and it heads the agenda band";
  EXPECT_NE(findTextOperation(canvas, "MA 14 SEP \xc2\xb7 2 AFSPRAKEN"), nullptr);
  EXPECT_NE(findTextOperation(canvas, "DI 15 SEP \xc2\xb7 4 AFSPRAKEN"), nullptr);
  // The band has room for all six events after the hero, so nothing is
  // summarised and Tuesday's fourth event is listed like the rest.
  EXPECT_NE(findTextOperation(canvas, "AI doc"), nullptr) << "a row the old three-row cap folded away is listed";
  for (const Operation* operation : textOperationsInBand(canvas, 62, 514)) {
    EXPECT_EQ(operation->text.find(" meer "), std::string::npos)
        << "the demonstration day fits, so nothing is summarised: " << operation->text;
  }
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Text) continue;
    EXPECT_EQ(operation.text.find("werkdag"), std::string::npos) << "the wire carries no work calendar";
    EXPECT_EQ(operation.text.find("ochtend"), std::string::npos);
  }
}

TEST(DashboardV3Renderer8A, OverflowAccountsForEveryEncodedEvent) {
  RecordingCanvas canvas;
  const dashboard::v3::DashboardV3Package package = extreme8APackage();
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);

  // The hero carries the package's first event; every other encoded event is
  // either a listed row or inside a "+N meer" line, and each line's N counts the
  // events it really hides.
  EXPECT_EQ(countTextOperations(canvas, "Eerste afspraak"), 1) << "the first event is the hero's";
  int rowEvents = 0;
  int summarisedEvents = 0;
  int summarisedLines = 0;
  for (const auto* operation : textOperationsInBand(canvas, 179, 514)) {
    if (operation->text.rfind("+", 0) == 0 && operation->text.find(" meer") != std::string::npos) {
      ++summarisedLines;
      summarisedEvents += std::atoi(operation->text.c_str() + 1);
    } else if (operation->text == "Kwartaalreview Fiberforce Nederl") {
      ++rowEvents;
    }
  }
  EXPECT_GT(summarisedLines, 0);
  EXPECT_EQ(1 + rowEvents + summarisedEvents, static_cast<int>(package.agendaCount))
      << "every encoded event is either a row or inside a summary";
}

TEST(DashboardV3Renderer8A, AgendaBandListsEveryRowOfASingleDayWithoutASummary) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, singleDayAgenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  // The hero owns the first event; the six that follow must all be listed. A
  // fixed three-rows-per-day cap would have folded the last three into a
  // "+N meer" line even though the band has room for them.
  EXPECT_EQ(countTextOperations(canvas, "Eerste"), 1) << "the hero still carries the first event exactly once";
  for (const char* const title : {"Tweede", "Derde", "Vierde", "Vijfde", "Zesde", "Zevende"}) {
    EXPECT_EQ(countTextOperations(canvas, title), 1) << title << " is listed once in the agenda";
  }
  for (const Operation* operation : textOperationsInBand(canvas, 179, 514)) {
    EXPECT_EQ(operation->text.find(" meer "), std::string::npos)
        << "nothing is summarised when every row fits: " << operation->text;
  }
}

TEST(DashboardV3Renderer8A, DayGroupsDrawNoBlackSeparatorBelowTheHeading) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  const dashboard::v3::Dashboard8ARects bands = dashboard::v3::computeDashboard8ALayout(528, 792, {});

  // No day group draws a full-width black band any more: the focus row sits on
  // the same white as the rows, and a heading separator would be its own band.
  int fullWidthBlackFills = 0;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Fill || !operation.black) continue;
    const dashboard::v3::Rect bounds = operation.bounds;
    if (bounds.y < bands.agenda.y || bounds.y + bounds.height > bands.agenda.y + bands.agenda.height) continue;
    if (bounds.x != bands.agenda.x || bounds.width != bands.agenda.width) continue;
    ++fullWidthBlackFills;
  }
  EXPECT_EQ(fullWidthBlackFills, 0) << "neither the focus row nor a heading separator is a black band";

  for (const char* const heading : {"ZO 13 SEP \xc2\xb7 1 AFSPRAAK", "MA 14 SEP \xc2\xb7 2 AFSPRAKEN",
                                    "DI 15 SEP \xc2\xb7 4 AFSPRAKEN"}) {
    const Operation* text = findTextOperation(canvas, heading);
    ASSERT_NE(text, nullptr);
    EXPECT_TRUE(text->black) << "every heading is black text on white";
  }
}

TEST(DashboardV3Renderer8A, FocusRowDrawsOnWhiteWithNoBlackFill) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  const dashboard::v3::Agenda8ARects agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
  const dashboard::v3::Rect focus = focusRowBounds(agenda);

  // The focus row is a row, not a band: nothing fills its rectangle with ink,
  // so its own time and title have to be the black on white.
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Fill || !operation.black) continue;
    const dashboard::v3::Rect bounds = operation.bounds;
    const bool overlapsFocus = bounds.x < focus.x + focus.width && bounds.x + bounds.width > focus.x &&
                               bounds.y < focus.y + focus.height && bounds.y + bounds.height > focus.y;
    EXPECT_FALSE(overlapsFocus) << "the focus row must not be filled black";
  }
}

TEST(DashboardV3Renderer8A, FirstDayGroupRunsHeadingRibbonFocusRowThenTheRestOfItsRows) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, singleDayAgenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  const dashboard::v3::Agenda8ARects agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});

  const Operation* heading = findTextOperation(canvas, "ZO 13 SEP \xc2\xb7 7 AFSPRAKEN");
  ASSERT_NE(heading, nullptr) << "the first group heads on white before its ribbon";
  EXPECT_TRUE(heading->black);
  EXPECT_EQ(heading->bounds.y, agenda.content.y);

  // The day's own ribbon comes first, so the day's shape reads before its list:
  // every day is heading, ribbon, then rows.
  const Operation* ribbon = nullptr;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Rect || operation.bounds.height != agenda.ribbonHeight) continue;
    if (operation.bounds.width != agenda.content.width) continue;
    if (ribbon == nullptr || operation.bounds.y < ribbon->bounds.y) ribbon = &operation;
  }
  ASSERT_NE(ribbon, nullptr);
  EXPECT_EQ(ribbon->bounds.y, agenda.content.y + agenda.headingHeight)
      << "the ribbon sits directly under the day heading, ahead of the focus row";

  // The 58 px focus row is the first entry of that day's list, not a band above
  // the ribbon.
  const dashboard::v3::Rect focus = focusRowBounds(agenda);
  EXPECT_EQ(focus.y, ribbon->bounds.y + agenda.ribbonHeight + agenda.ribbonGap);
  const Operation* focusTime = findTextOperation(canvas, "Eerste");
  ASSERT_NE(focusTime, nullptr) << "the focus row carries row 0's own title";
  EXPECT_EQ(focusTime->font, dashboard::v3::FontRole::Body);
  EXPECT_GE(focusTime->bounds.y, focus.y);
  EXPECT_LT(focusTime->bounds.y, focus.y + focus.height);

  const Operation* firstRow = findTextOperation(canvas, "Tweede");
  ASSERT_NE(firstRow, nullptr) << "the rows after the focus row are listed normally";
  EXPECT_GE(firstRow->bounds.y, focus.y + focus.height) << "those rows start below the focus row";

  // Row 0 is the focus row and only the focus row.
  EXPECT_EQ(countTextOperations(canvas, "Eerste"), 1);
}

TEST(DashboardV3Renderer8A, ASingleAppointmentStillDrawsItsHeadingHeroAndRibbon) {
  dashboard::v3::DashboardV3Package package{};
  package.formatVersion = dashboard::v3::FORMAT_VERSION_DAY_TOTALS;
  package.generatedAt = 1789298100ULL;
  setAgenda8A(package, 0, 0, 9 * 60, "Enige afspraak", 60, false, false, 1, 0);
  package.agendaCount = 1;

  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);

  // The one appointment is the hero's, and its day still heads the group with
  // the exact count the phone sent.
  EXPECT_NE(findTextOperation(canvas, "ZO 13 SEP \xc2\xb7 1 AFSPRAAK"), nullptr);
  EXPECT_EQ(countTextOperations(canvas, "Enige afspraak"), 1) << "drawn once, in the focus row";

  const dashboard::v3::Agenda8ARects agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
  const dashboard::v3::Rect focus = focusRowBounds(agenda);
  const Operation* focusTitle = findTextOperation(canvas, "Enige afspraak");
  ASSERT_NE(focusTitle, nullptr) << "one appointment still gets the 58 px focus row";
  EXPECT_GE(focusTitle->bounds.y, focus.y);
  EXPECT_LT(focusTitle->bounds.y, focus.y + focus.height);
  bool ribbon = false;
  for (const auto& operation : canvas.operations) {
    ribbon |= operation.kind == Operation::Kind::Rect && operation.bounds.height == agenda.ribbonHeight &&
              operation.bounds.width == agenda.content.width;
  }
  EXPECT_TRUE(ribbon) << "and the day's own outlined ribbon";
  for (const Operation* operation : textOperationsInBand(canvas, 62, 514)) {
    EXPECT_EQ(operation->text.find("meer"), std::string::npos) << "one appointment hides nothing";
  }
}

TEST(DashboardV3Renderer8A, OneWireRowWithATruncatedDayTotalHeadsHeroesAndSummarises) {
  dashboard::v3::DashboardV3Package package{};
  package.formatVersion = dashboard::v3::FORMAT_VERSION_DAY_TOTALS;
  package.generatedAt = 1789298100ULL;
  // The wire carried one row but the calendar holds five: the hero draws the one
  // row, the heading shows the phone's exact five, and the other four ride in
  // one standalone line. The hero's own row is counted in that total, not added
  // on top of it.
  setAgenda8A(package, 0, 0, 9 * 60, "Enige afspraak", 60, false, false, 5, 0);
  package.agendaCount = 1;

  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);

  EXPECT_NE(findTextOperation(canvas, "ZO 13 SEP \xc2\xb7 5 AFSPRAKEN"), nullptr);
  EXPECT_NE(findTextOperation(canvas, "+4 meer"), nullptr)
      << "five total minus the hero's row leaves the four the wire never sent";
  EXPECT_EQ(countTextOperations(canvas, "Enige afspraak"), 1);
  const dashboard::v3::Rect focus =
      focusRowBounds(dashboard::v3::computeAgenda8ALayout(528, 792, {}));
  const Operation* focusTitle = findTextOperation(canvas, "Enige afspraak");
  ASSERT_NE(focusTitle, nullptr) << "the focus row is still drawn for the one row that exists";
  EXPECT_GE(focusTitle->bounds.y, focus.y);
  EXPECT_LT(focusTitle->bounds.y, focus.y + focus.height);
}

TEST(DashboardV3Renderer8A, LaterDayGroupsDrawHeadingRibbonAndRowsWithoutAHero) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, threeDayAgenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  // The hero's group plus two later groups fit; the day after them is what the
  // standalone "+N meer" line stands in for.
  const dashboard::v3::Rect focus =
      focusRowBounds(dashboard::v3::computeAgenda8ALayout(528, 792, {}));
  const Operation* focusTitle = findTextOperation(canvas, "Verhalenhuis 3");
  ASSERT_NE(focusTitle, nullptr);
  EXPECT_GE(focusTitle->bounds.y, focus.y) << "only the first day group carries a focus row";
  EXPECT_LT(focusTitle->bounds.y, focus.y + focus.height);
  ASSERT_NE(findDayRibbon(canvas), nullptr);
  EXPECT_NE(findTextOperation(canvas, "MA 14 SEP \xc2\xb7 2 AFSPRAKEN"), nullptr);
  EXPECT_NE(findTextOperation(canvas, "DI 15 SEP \xc2\xb7 2 AFSPRAKEN"), nullptr);
  EXPECT_EQ(findTextOperation(canvas, "WO 16 SEP \xc2\xb7 2 AFSPRAKEN"), nullptr)
      << "the day that does not fit is named by the summary, not headed";

  const Operation* summary = nullptr;
  for (const Operation* operation : textOperationsInBand(canvas, 62, 514)) {
    if (operation->text.rfind("+", 0) == 0 && operation->text.find("meer") != std::string::npos) summary = operation;
  }
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->text, "+2 meer") << "the omitted day's two rows ride in one standalone line";
  EXPECT_EQ(summary->text.find("\xc2\xb7"), std::string::npos);

  // Only the first day group ever draws a focus row, so the later days' own
  // titles are ordinary rows and nothing else draws the Hero rung.
  int heroRungInk = 0;
  for (const auto& operation : canvas.operations) {
    if (operation.kind == Operation::Kind::Text && operation.font == dashboard::v3::FontRole::Hero) ++heroRungInk;
  }
  EXPECT_EQ(heroRungInk, 1) << "later groups are heading, ribbon and rows only";
  EXPECT_NE(findTextOperation(canvas, "Theaterles"), nullptr);
  EXPECT_NE(findTextOperation(canvas, "Padel"), nullptr);
}

TEST(DashboardV3Renderer8A, OverflowSummaryCoversEveryEventTheRowsCouldNotShow) {
  RecordingCanvas canvas;
  const dashboard::v3::DashboardV3Package package = extreme8APackage();
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);

  // When the list cannot hold everything it still ends in a single "+N meer"
  // line, and that final line counts every event the rows left out -- not just
  // the last day's. The hero holds the first event, so it is not in the list.
  int listedRows = 0;
  const Operation* finalSummary = nullptr;
  for (const Operation* operation : textOperationsInBand(canvas, 179, 514)) {
    if (operation->text.rfind("+", 0) == 0 && operation->text.find(" meer") != std::string::npos) {
      finalSummary = operation;
    } else if (operation->text == "Kwartaalreview Fiberforce Nederl") {
      ++listedRows;
    }
  }
  ASSERT_NE(finalSummary, nullptr) << "the events that do not fit are summarised";
  EXPECT_EQ(finalSummary->text.find("\xc2\xb7"), std::string::npos)
      << "the summary is standalone and never names an omitted appointment";
  const int listedEvents = 1 + listedRows;
  EXPECT_EQ(listedEvents + std::atoi(finalSummary->text.c_str() + 1), static_cast<int>(package.agendaCount))
      << "the final summary counts every event the rows could not show";
}

TEST(DashboardV3Renderer8A, ExactDayTotalDrivesHeadingAndStandaloneOverflowSummary) {
  dashboard::v3::DashboardV3Package package{};
  package.formatVersion = 5;
  package.generatedAt = 1789298100ULL;
  setAgenda8A(package, 0, 0, 9 * 60, "Hero", 30, false, false, 20, 0);
  for (size_t index = 1; index < 13; ++index) {
    setAgenda8A(package, index, 0, static_cast<uint16_t>(9 * 60 + index * 20), "Vandaag", 20,
                false, false, 20, 0);
  }
  package.agendaCount = 13;

  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);

  EXPECT_NE(findTextOperation(canvas, "ZO 13 SEP \xc2\xb7 20 AFSPRAKEN"), nullptr);
  EXPECT_NE(findTextOperation(canvas, "+8 meer"), nullptr);
}

TEST(DashboardV3Renderer8A, PortfolioBarGrowsFromTheCentreInBothDirections) {
  // The left KPI column's four bars share one track geometry; the portfolio's is
  // the last of them.
  const auto barFor = [](const RecordingCanvas& canvas) -> dashboard::v3::Rect {
    dashboard::v3::Rect last{};
    for (const auto& operation : canvas.operations) {
      if (operation.kind == Operation::Kind::Rect && operation.bounds.x < 264 && operation.bounds.height == 10 &&
          operation.bounds.width > 100) {
        last = operation.bounds;
      }
    }
    return last;
  };
  // The zero mark is drawn first and the fill may cover it, so the fill is the
  // widest thing drawn inside the track.
  const auto fillIn = [](const RecordingCanvas& canvas, const dashboard::v3::Rect bar) {
    dashboard::v3::Rect widest{};
    for (const auto& operation : canvas.operations) {
      if (operation.kind != Operation::Kind::Fill || operation.bounds.y != bar.y + 1) continue;
      if (operation.bounds.x < bar.x || operation.bounds.x >= bar.x + bar.width) continue;
      if (operation.bounds.height != bar.height - 2) continue;
      if (operation.bounds.width > widest.width) widest = operation.bounds;
    }
    return widest;
  };

  auto gainPackage = agenda8APackage();
  RecordingCanvas gainCanvas;
  dashboard::v3::renderDashboardV3(gainCanvas, gainPackage, /*minuteOfDay=*/11 * 60 + 15);
  const dashboard::v3::Rect gainBar = barFor(gainCanvas);
  ASSERT_GT(gainBar.width, 0);
  const int center = gainBar.x + 1 + (gainBar.width - 2) / 2;
  const dashboard::v3::Rect gainFill = fillIn(gainCanvas, gainBar);
  const int half = (gainBar.width - 2) / 2;
  EXPECT_EQ(gainFill.x, center) << "+0,9% grows to the right of zero";
  EXPECT_EQ(gainFill.width, half * 90 / 800);
  EXPECT_EQ(gainFill.height, gainBar.height - 2);

  auto lossPackage = agenda8APackage();
  lossPackage.portfolioChangeBasisPoints = -400;
  RecordingCanvas lossCanvas;
  dashboard::v3::renderDashboardV3(lossCanvas, lossPackage, /*minuteOfDay=*/11 * 60 + 15);
  const dashboard::v3::Rect lossBar = barFor(lossCanvas);
  const dashboard::v3::Rect lossFill = fillIn(lossCanvas, lossBar);
  const int lossCenter = lossBar.x + 1 + (lossBar.width - 2) / 2;
  EXPECT_EQ(lossFill.x + lossFill.width, lossCenter) << "-4,0% grows to the left of zero";
  EXPECT_EQ(lossFill.width, (lossBar.width - 2) / 2 * 400 / 800);
  EXPECT_LT(lossFill.x, lossCenter);

  auto unknownPackage = agenda8APackage();
  unknownPackage.portfolioChangeBasisPoints = INT16_MIN;
  RecordingCanvas unknownCanvas;
  dashboard::v3::renderDashboardV3(unknownCanvas, unknownPackage, /*minuteOfDay=*/11 * 60 + 15);
  const dashboard::v3::Rect unknownBar = barFor(unknownCanvas);
  const dashboard::v3::Rect unknownFill = fillIn(unknownCanvas, unknownBar);
  EXPECT_EQ(unknownFill.width, 1) << "an unknown change leaves the track empty but for the zero mark";
  EXPECT_EQ(unknownFill.x, unknownBar.x + 1 + (unknownBar.width - 2) / 2);
  EXPECT_NE(findTextOperation(unknownCanvas, "-"), nullptr) << "and its value is a dash";
}

TEST(DashboardV3Renderer8A, PortfolioBarClampsAtEightPercent) {
  const auto barFor = [](const RecordingCanvas& canvas) -> dashboard::v3::Rect {
    dashboard::v3::Rect last{};
    for (const auto& operation : canvas.operations) {
      if (operation.kind == Operation::Kind::Rect && operation.bounds.x < 264 && operation.bounds.height == 10 &&
          operation.bounds.width > 100) {
        last = operation.bounds;
      }
    }
    return last;
  };

  auto package = agenda8APackage();
  package.portfolioChangeBasisPoints = 5000;  // +50%, well past the bar's range
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);
  const dashboard::v3::Rect bar = barFor(canvas);
  ASSERT_GT(bar.width, 0);
  int widest = 0;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Fill || operation.bounds.y != bar.y + 1) continue;
    widest = std::max(widest, operation.bounds.width);
    EXPECT_LE(operation.bounds.x + operation.bounds.width, bar.x + bar.width - 1)
        << "the fill never leaves its track";
  }
  EXPECT_LE(widest, (bar.width - 2) / 2) << "the fill stops at half the track, the +/-8% end";
  EXPECT_GE(widest, (bar.width - 2) / 2 - 1);
}

TEST(DashboardV3Renderer8A, MarketLineDrawsAllFourIndicesAndTheMover) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  for (const char* const label : {"AEX", "S&P", "NDX", "BTC"}) {
    const Operation* operation = findTextOperation(canvas, label);
    ASSERT_NE(operation, nullptr) << "the primary indices are never dropped: " << label;
    EXPECT_GE(operation->bounds.y, 694) << label << " belongs on the single market line";
    EXPECT_LT(operation->bounds.y, 728);
  }
  for (const char* const change : {"+0,5%", "+0,3%", "+0,7%", "+1,2%"}) {
    expectTextFitsItsBox(canvas, change, "an index change must not be clipped");
  }
  EXPECT_NE(findTextOperation(canvas, "ASML"), nullptr);
  expectTextFitsItsBox(canvas, "+6,1%", "the strongest mover's own change");
  EXPECT_NE(findTextOperation(canvas, "+2"), nullptr) << "three movers means two besides the strongest";
  // One line: every market string shares the same row.
  const Operation* asml = findTextOperation(canvas, "ASML");
  const Operation* aex = findTextOperation(canvas, "AEX");
  ASSERT_NE(asml, nullptr);
  ASSERT_NE(aex, nullptr);
  EXPECT_EQ(asml->bounds.y, aex->bounds.y);
  // The design asks for daily percentage moves and nothing else: no euro
  // amounts, and no currency glyph at all on this line.
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Text) continue;
    EXPECT_EQ(operation.text.find("\xe2\x82\xac"), std::string::npos) << operation.text;
    EXPECT_EQ(operation.text.find("\x24"), std::string::npos) << operation.text;
  }
}

TEST(DashboardV3Renderer8A, MarketMoverNeedsMoreThanThreePercent) {
  auto package = agenda8APackage();
  package.moverCount = 2;
  package.strongestMover.changeBasisPoints = 300;  // exactly three percent
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);
  EXPECT_EQ(findTextOperation(canvas, "ASML"), nullptr) << "three percent is not a mover";
  EXPECT_EQ(findTextOperation(canvas, "+2"), nullptr);

  package.strongestMover.changeBasisPoints = 301;
  RecordingCanvas aboveCanvas;
  dashboard::v3::renderDashboardV3(aboveCanvas, package, /*minuteOfDay=*/11 * 60 + 15);
  EXPECT_NE(findTextOperation(aboveCanvas, "ASML"), nullptr);
}

TEST(DashboardV3Renderer8A, MarketMoverIsOptionalButIndicesAreNot) {
  auto package = agenda8APackage();
  package.moverCount = 0;
  package.strongestMover = {};
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);
  EXPECT_EQ(findTextOperation(canvas, "ASML"), nullptr);
  for (const char* const label : {"AEX", "S&P", "NDX", "BTC"}) {
    EXPECT_NE(findTextOperation(canvas, label), nullptr);
  }
}

TEST(DashboardV3Renderer8A, IndicesSurviveEvenWhenTheCanvasCannotMeasure) {
  auto package = agenda8APackage();
  RecordingCanvas canvas;
  canvas.canMeasure = false;  // the character-count fallback, with its margins
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);

  for (const char* const label : {"AEX", "S&P", "NDX", "BTC"}) {
    const Operation* operation = findTextOperation(canvas, label);
    ASSERT_NE(operation, nullptr) << "a wider guess drops the optional mover, never an index: " << label;
  }
  for (const char* const change : {"+0,5%", "+0,3%", "+0,7%", "+1,2%"}) {
    expectTextFitsItsBox(canvas, change, "no measurement must not clip an index value");
  }
  expectOperationsInsideCanvas(canvas);
}

TEST(DashboardV3Renderer8A, ExtremeLabelsShrinkTheCaptionNotTheNumber) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, extreme8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  int changes = 0;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Text || operation.text != "-12,3%") continue;
    ++changes;
    EXPECT_GE(operation.bounds.width, nominalWidth(operation.font, operation.text.c_str()))
        << "four twelve-byte labels still leave every number whole";
  }
  EXPECT_EQ(changes, 4) << "the four indices are all still on the line";
}

TEST(DashboardV3Renderer8A, KpiValuesStayIntactWhileCaptionsMayShorten) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  expectTextFitsItsBox(canvas, "76%", "the X3 battery reading");
  expectTextFitsItsBox(canvas, "49%", "the car battery reading");
  expectTextFitsItsBox(canvas, "23%", "the home battery reading");
  expectTextFitsItsBox(canvas, "+0,9%", "the portfolio change");
  expectTextFitsItsBox(canvas, "43 min", "the travel time");
  expectTextFitsItsBox(canvas, "128 km", "the national jam");
  expectTextFitsItsBox(canvas, "24", "the unread count");
  expectTextFitsItsBox(canvas, "7.850", "the step count");

  // Every KPI row sits in one of the two equal columns, and the four left-hand
  // bars share one column of gauges.
  std::vector<dashboard::v3::Rect> bars;
  for (const auto& operation : canvas.operations) {
    if (operation.kind == Operation::Kind::Rect && operation.bounds.height == 10 && operation.bounds.width > 100 &&
        operation.bounds.y >= 554 && operation.bounds.y < 694) {
      bars.push_back(operation.bounds);
    }
  }
  ASSERT_EQ(bars.size(), 4u);
  for (const dashboard::v3::Rect& bar : bars) {
    EXPECT_EQ(bar.x, bars.front().x) << "the four bars are aligned";
    EXPECT_EQ(bar.width, bars.front().width);
    EXPECT_LT(bar.x, 264) << "they belong to the left column";
  }
}

TEST(DashboardV3Renderer8A, KpiLeftRowsShareOneLineAndCarryNoCaptions) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  // The captions that used to strike a line through the bars are gone.
  for (const char* const caption : {"X3", "AUTO", "THUIS", "PORT."}) {
    EXPECT_EQ(findTextOperation(canvas, caption), nullptr) << caption << " is no longer drawn in the KPI band";
  }

  std::vector<dashboard::v3::Rect> bars;
  std::vector<dashboard::v3::Rect> icons;
  for (const auto& operation : canvas.operations) {
    if (operation.bounds.x >= 264 || operation.bounds.y < 554) continue;
    if (operation.kind == Operation::Kind::Rect && operation.bounds.height == 10 && operation.bounds.width > 100) {
      bars.push_back(operation.bounds);
    }
    if (operation.kind == Operation::Kind::Icon) icons.push_back(operation.bounds);
  }
  ASSERT_EQ(bars.size(), 4u);
  ASSERT_EQ(icons.size(), 4u);

  // Each bar shares its row with the icon and the value to its left.
  for (const dashboard::v3::Rect& bar : bars) {
    const int center = bar.y + bar.height / 2;
    bool iconOnRow = false;
    bool valueOnRow = false;
    for (const auto& operation : canvas.operations) {
      if (operation.bounds.y + operation.bounds.height < center - 8 || operation.bounds.y > center + 8) continue;
      if (operation.kind == Operation::Kind::Icon && operation.bounds.x >= 0 && operation.bounds.x < bar.x) {
        iconOnRow = true;
      }
      if (operation.kind == Operation::Kind::Text && operation.font == dashboard::v3::FontRole::Body &&
          operation.bounds.x < bar.x) {
        valueOnRow = true;
      }
    }
    EXPECT_TRUE(iconOnRow) << "the icon is on the bar's own row";
    EXPECT_TRUE(valueOnRow) << "the value is on the bar's own row, left of the bar";
  }
}

TEST(DashboardV3Renderer8A, KpiColumnsAreSplitByARuleOnTheHalfwayLine) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  bool rule = false;
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Line) continue;
    rule |= operation.bounds.x == 264 && operation.bounds.width == 1 && operation.bounds.y >= 554 &&
            operation.bounds.y + operation.bounds.height <= 694;
  }
  EXPECT_TRUE(rule) << "the two KPI columns are separated on the halfway line";
}

TEST(DashboardV3Renderer8A, MarketMoverExtraCountIsInkOnPaper) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  const Operation* extra = findTextOperation(canvas, "+2");
  ASSERT_NE(extra, nullptr) << "three movers means two besides the strongest";
  EXPECT_TRUE(extra->black) << "the \"+2\" sits on the white market band, so it must be drawn in black";
  EXPECT_GE(extra->bounds.width, nominalWidth(extra->font, extra->text.c_str())) << "and never clipped";
}

TEST(DashboardV3Renderer8A, MarketLineAlwaysDrawsFourFixedSlots) {
  auto package = agenda8APackage();
  package.marketCount = 0;
  package.moverCount = 0;
  package.strongestMover = {};
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/11 * 60 + 15);

  for (const char* const label : {"AEX", "S&P", "NDX", "BTC"}) {
    const Operation* operation = findTextOperation(canvas, label);
    ASSERT_NE(operation, nullptr) << "the fixed slot label is drawn even with no package rows: " << label;
    EXPECT_GE(operation->bounds.y, 694);
    EXPECT_LT(operation->bounds.y, 728);
  }

  int dashes = 0;
  for (const auto& operation : canvas.operations) {
    if (operation.kind == Operation::Kind::Text && operation.text == "-" && operation.bounds.y >= 694 &&
        operation.bounds.y < 728) {
      ++dashes;
    }
  }
  EXPECT_EQ(dashes, 4) << "with no package rows every change is a dash";

  // No mover means no divider and no extra count on the line.
  EXPECT_EQ(findTextOperation(canvas, "+2"), nullptr);
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Line || operation.bounds.width != 1) continue;
    EXPECT_FALSE(operation.bounds.y >= 694 && operation.bounds.y < 728) << "no mover means no divider";
  }
}

TEST(DashboardV3Renderer8A, RainBandFitsOutlookStripEndAndAdviceOnOneRow) {
  RecordingCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, agenda8APackage(), /*minuteOfDay=*/11 * 60 + 15);

  const dashboard::v3::Rect band = dashboard::v3::computeDashboard8ALayout(528, 792, {}).rain;
  const Operation* outlook = findTextOperation(canvas, "12:55 lichte regen");
  const Operation* startTime = findTextOperation(canvas, "11:15");
  const Operation* endTime = findTextOperation(canvas, "13:15");
  ASSERT_NE(outlook, nullptr) << "the compact outlook keeps its absolute start clock";
  ASSERT_NE(startTime, nullptr) << "the window's own start clock is on the row";
  ASSERT_NE(endTime, nullptr) << "the window's end clock is on the row";
  for (const Operation* operation : {outlook, startTime, endTime}) {
    EXPECT_GE(operation->bounds.y, band.y);
    EXPECT_LT(operation->bounds.y + operation->bounds.height, band.y + band.height)
        << "the moved rain band is still 40 px tall";
  }

  // The outlined 10 px strip is on the same row and bracketed by the two window
  // clocks; the heating badge is the only thing to the right of the end clock.
  const Operation* strip = nullptr;
  for (const auto& operation : canvas.operations) {
    if (operation.kind == Operation::Kind::Rect && operation.bounds.height == 10 && operation.bounds.y >= band.y &&
        operation.bounds.y < band.y + band.height) {
      strip = &operation;
    }
  }
  ASSERT_NE(strip, nullptr) << "the intensity strip shares the row";
  const int center = strip->bounds.y + strip->bounds.height / 2;
  for (const Operation* operation : {outlook, startTime, endTime}) {
    EXPECT_LE(operation->bounds.y, center);
    EXPECT_GE(operation->bounds.y + operation->bounds.height, center);
  }
  // The outlook's box is reserved wider than its text; what must clear the strip
  // is the drawn string, not the box.
  EXPECT_LT(outlook->bounds.x + nominalWidth(outlook->font, outlook->text.c_str()), strip->bounds.x + 1);
  // The start clock sits left of the strip and the end clock right of it, so the
  // window reads as one labelled axis.
  EXPECT_LE(startTime->bounds.x + startTime->bounds.width, strip->bounds.x + 1);
  EXPECT_LE(strip->bounds.x + strip->bounds.width, endTime->bounds.x + 1);
  // Nothing but the one heating icon follows the end clock, and no advice text
  // is drawn anywhere on the row.
  const Operation* flame = findIconOperation(canvas, 14);
  ASSERT_NE(flame, nullptr) << "the heating verdict is a single icon";
  EXPECT_GE(flame->bounds.x, endTime->bounds.x + endTime->bounds.width);
  for (const auto& operation : canvas.operations) {
    if (operation.kind != Operation::Kind::Text) continue;
    EXPECT_EQ(operation.text.find("STOKEN"), std::string::npos) << operation.text;
    EXPECT_EQ(operation.text.find("VERWARMING"), std::string::npos) << operation.text;
  }

  expectTextFitsItsBox(canvas, "12:55 lichte regen", "the outlook is never clipped");
  expectTextFitsItsBox(canvas, "11:15", "the window start clock is never clipped");
  expectTextFitsItsBox(canvas, "13:15", "the window end clock is never clipped");
  expectOperationsInsideCanvas(canvas);
}

TEST(DashboardV3Renderer8A, LegacyFormatsKeepTheLegacyRenderer) {
  for (const uint8_t version : {dashboard::v3::FORMAT_VERSION_V2, dashboard::v3::FORMAT_VERSION}) {
    auto package = maximumContentPackage();
    package.formatVersion = version;
    RecordingCanvas canvas;
    dashboard::v3::renderDashboardV3(canvas, package, /*minuteOfDay=*/12 * 60);
    EXPECT_NE(findTextOperation(canvas, "KOMENDE AFSPRAKEN"), nullptr) << "format " << int(version);
    EXPECT_NE(findTextOperation(canvas, "STATUS"), nullptr);
    EXPECT_EQ(findTextOperation(canvas, "PORT."), nullptr) << "the 8A bands must not leak into a legacy package";
  }
}

struct TestGlyph {
  uint16_t codePoint;
  std::array<uint16_t, 7> columns;  // column-major; bit row r = row r (9 rows used)
};

// Column-major bits, bit 0 = top row. Generated from a desktop monospace font
// (Menlo) and baked in so the host tests never depend on system fonts.
static constexpr TestGlyph kTestGlyphsAscii[95] = {
    {0x0020, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},  // ' '
    {0x0021, {0x00, 0x00, 0x1BF, 0x11F, 0x00, 0x00, 0x00}},  // '!'
    {0x0022, {0x7C, 0x7C, 0x00, 0x00, 0x00, 0x7C, 0x7C}},  // '"'
    {0x0023, {0x20, 0xE4, 0x3E, 0xA7, 0x3C, 0x27, 0x04}},  // '#'
    {0x0024, {0x00, 0x4C, 0x0A, 0x1FF, 0x12, 0x60, 0x00}},  // '$'
    {0x0025, {0x26, 0x29, 0x19, 0xD6, 0x130, 0x128, 0xC8}},  // '%'
    {0x0026, {0x60, 0x196, 0x119, 0x131, 0x1C1, 0x1C0, 0x20}},  // '&'
    {0x0027, {0x00, 0x1FF, 0x1FF, 0x1FF, 0x1FF, 0x00, 0x00}},  // '''
    {0x0028, {0x00, 0x00, 0x7C, 0x101, 0x00, 0x00, 0x00}},  // '('
    {0x0029, {0x00, 0x00, 0x101, 0xC6, 0x38, 0x00, 0x00}},  // ')'
    {0x002A, {0x00, 0x28, 0x38, 0xFE, 0x10, 0x28, 0x00}},  // '*'
    {0x002B, {0x10, 0x10, 0x10, 0xFE, 0x10, 0x10, 0x10}},  // '+'
    {0x002C, {0x100, 0x1F0, 0x1FF, 0xFF, 0x3F, 0x0F, 0x00}},  // ','
    {0x002D, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}},  // '-'
    {0x002E, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}},  // '.'
    {0x002F, {0x00, 0x100, 0xC0, 0x38, 0x06, 0x01, 0x00}},  // '/'
    {0x0030, {0x7C, 0x1E3, 0x131, 0x109, 0xCE, 0x7C, 0x00}},  // '0'
    {0x0031, {0x102, 0x103, 0x103, 0x1FF, 0x100, 0x100, 0x00}},  // '1'
    {0x0032, {0x100, 0x181, 0x141, 0x121, 0x11F, 0x10E, 0x00}},  // '2'
    {0x0033, {0x100, 0x101, 0x111, 0x111, 0x1BF, 0xE6, 0x00}},  // '3'
    {0x0034, {0x60, 0x58, 0x44, 0x43, 0x1FF, 0x40, 0x00}},  // '4'
    {0x0035, {0x10F, 0x10F, 0x109, 0x109, 0x99, 0xF0, 0x00}},  // '5'
    {0x0036, {0x7C, 0x19A, 0x109, 0x109, 0x199, 0xF0, 0x00}},  // '6'
    {0x0037, {0x01, 0x101, 0x1C1, 0x79, 0x0F, 0x03, 0x00}},  // '7'
    {0x0038, {0xE6, 0x1BF, 0x111, 0x111, 0x1BF, 0xE4, 0x00}},  // '8'
    {0x0039, {0x1C, 0x133, 0x121, 0x121, 0xB6, 0x7C, 0x00}},  // '9'
    {0x003A, {0x00, 0x00, 0x1C7, 0x1C7, 0x1C7, 0x00, 0x00}},  // ':'
    {0x003B, {0x00, 0x00, 0x100, 0x1E3, 0x63, 0x00, 0x00}},  // ';'
    {0x003C, {0x08, 0x18, 0x18, 0x24, 0x24, 0x24, 0x42}},  // '<'
    {0x003D, {0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24}},  // '='
    {0x003E, {0x42, 0x24, 0x24, 0x24, 0x18, 0x18, 0x18}},  // '>'
    {0x003F, {0x00, 0x01, 0x01, 0x131, 0x0F, 0x06, 0x00}},  // '?'
    {0x0040, {0x3C, 0x82, 0x139, 0x145, 0x145, 0x3E, 0x00}},  // '@'
    {0x0041, {0x180, 0xF0, 0x4E, 0x43, 0x5E, 0xF0, 0x100}},  // 'A'
    {0x0042, {0x1FF, 0x119, 0x109, 0x119, 0x1BF, 0xE4, 0x00}},  // 'B'
    {0x0043, {0x7C, 0xC6, 0x101, 0x101, 0x101, 0x101, 0x00}},  // 'C'
    {0x0044, {0x1FF, 0x101, 0x101, 0x101, 0xC6, 0x7C, 0x00}},  // 'D'
    {0x0045, {0x1FF, 0x119, 0x109, 0x109, 0x109, 0x101, 0x00}},  // 'E'
    {0x0046, {0x00, 0x1FF, 0x09, 0x09, 0x09, 0x01, 0x00}},  // 'F'
    {0x0047, {0x7C, 0xC6, 0x101, 0x101, 0x111, 0xF0, 0x00}},  // 'G'
    {0x0048, {0x1FF, 0x18, 0x08, 0x08, 0x18, 0x1FF, 0x00}},  // 'H'
    {0x0049, {0x00, 0x101, 0x101, 0x1FF, 0x101, 0x101, 0x00}},  // 'I'
    {0x004A, {0x00, 0x100, 0x100, 0x101, 0x181, 0xFF, 0x00}},  // 'J'
    {0x004B, {0x1FF, 0x18, 0x18, 0x34, 0xC2, 0x181, 0x100}},  // 'K'
    {0x004C, {0x1FF, 0x100, 0x100, 0x100, 0x100, 0x100, 0x00}},  // 'L'
    {0x004D, {0x1FF, 0x03, 0x1C, 0x30, 0x0C, 0x03, 0x1FF}},  // 'M'
    {0x004E, {0x1FF, 0x07, 0x0E, 0x38, 0xE0, 0x1FF, 0x1FF}},  // 'N'
    {0x004F, {0x7C, 0xC6, 0x101, 0x101, 0x183, 0x7C, 0x00}},  // 'O'
    {0x0050, {0x1FF, 0x31, 0x11, 0x11, 0x1B, 0x0E, 0x00}},  // 'P'
    {0x0051, {0x00, 0x3C, 0xC3, 0x81, 0x1C1, 0x7E, 0x00}},  // 'Q'
    {0x0052, {0x1FF, 0x11, 0x11, 0x31, 0x7B, 0x1CE, 0x100}},  // 'R'
    {0x0053, {0x0E, 0x11B, 0x111, 0x111, 0x1B1, 0xE0, 0x00}},  // 'S'
    {0x0054, {0x01, 0x01, 0x01, 0x1FF, 0x01, 0x01, 0x01}},  // 'T'
    {0x0055, {0xFF, 0x180, 0x100, 0x100, 0x180, 0xFF, 0x00}},  // 'U'
    {0x0056, {0x01, 0x0F, 0xF0, 0x180, 0xF0, 0x1E, 0x03}},  // 'V'
    {0x0057, {0x07, 0xF8, 0x70, 0x0C, 0xF0, 0xF8, 0x07}},  // 'W'
    {0x0058, {0x100, 0x183, 0x66, 0x38, 0x6C, 0x1C3, 0x100}},  // 'X'
    {0x0059, {0x01, 0x07, 0x0C, 0x1F8, 0x0C, 0x03, 0x01}},  // 'Y'
    {0x005A, {0x181, 0x1C1, 0x131, 0x11D, 0x107, 0x101, 0x00}},  // 'Z'
    {0x005B, {0x00, 0x00, 0x1FF, 0x101, 0x00, 0x00, 0x00}},  // '['
    {0x005C, {0x00, 0x01, 0x06, 0x38, 0xC0, 0x100, 0x00}},  // '\'
    {0x005D, {0x00, 0x00, 0x101, 0x1FF, 0x00, 0x00, 0x00}},  // ']'
    {0x005E, {0x20, 0x10, 0x0C, 0x0C, 0x08, 0x30, 0x00}},  // '^'
    {0x005F, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}},  // '_'
    {0x0060, {0x00, 0x04, 0x1C, 0x38, 0x30, 0x60, 0x00}},  // '`'
    {0x0061, {0xE0, 0x1B3, 0x111, 0x111, 0x191, 0xFE, 0x1FC}},  // 'a'
    {0x0062, {0x00, 0x1FF, 0x108, 0x104, 0x108, 0xF0, 0x00}},  // 'b'
    {0x0063, {0x7C, 0xFE, 0x183, 0x101, 0x101, 0x101, 0x82}},  // 'c'
    {0x0064, {0x00, 0xF0, 0x188, 0x104, 0x108, 0x1FF, 0x00}},  // 'd'
    {0x0065, {0x3C, 0x7E, 0xC9, 0x89, 0x89, 0x8B, 0x0E}},  // 'e'
    {0x0066, {0x00, 0x00, 0x0C, 0x1FF, 0x01, 0x01, 0x00}},  // 'f'
    {0x0067, {0x1C, 0x133, 0x141, 0x101, 0x1A2, 0xFF, 0x00}},  // 'g'
    {0x0068, {0x00, 0x1FF, 0x08, 0x04, 0x0C, 0x1F8, 0x00}},  // 'h'
    {0x0069, {0x00, 0x100, 0x104, 0x1FD, 0x100, 0x100, 0x00}},  // 'i'
    {0x006A, {0x00, 0x00, 0x100, 0x104, 0xFD, 0x00, 0x00}},  // 'j'
    {0x006B, {0x1FF, 0x20, 0x30, 0xC8, 0x180, 0x100, 0x00}},  // 'k'
    {0x006C, {0x00, 0x01, 0x01, 0x1FF, 0x100, 0x100, 0x00}},  // 'l'
    {0x006D, {0xFF, 0x01, 0x01, 0xFF, 0x01, 0x01, 0xFE}},  // 'm'
    {0x006E, {0x1FF, 0x06, 0x03, 0x01, 0x01, 0x07, 0x1FE}},  // 'n'
    {0x006F, {0x3C, 0x66, 0x81, 0x81, 0x81, 0x7E, 0x3C}},  // 'o'
    {0x0070, {0x1FF, 0x22, 0x41, 0x41, 0x23, 0x1E, 0x00}},  // 'p'
    {0x0071, {0x00, 0x3E, 0x63, 0x41, 0x21, 0x1FF, 0x00}},  // 'q'
    {0x0072, {0x1FF, 0x0E, 0x03, 0x01, 0x01, 0x03, 0x00}},  // 'r'
    {0x0073, {0x18E, 0x19F, 0x111, 0x111, 0x111, 0xF3, 0x60}},  // 's'
    {0x0074, {0x04, 0x04, 0xFF, 0x104, 0x104, 0x104, 0x00}},  // 't'
    {0x0075, {0xFF, 0x1C0, 0x100, 0x100, 0x180, 0xE0, 0x1FF}},  // 'u'
    {0x0076, {0x02, 0x1C, 0x70, 0xC0, 0x70, 0x1E, 0x02}},  // 'v'
    {0x0077, {0x06, 0x78, 0x60, 0x08, 0x60, 0x78, 0x06}},  // 'w'
    {0x0078, {0x80, 0xC6, 0x2C, 0x38, 0x6C, 0xC2, 0x00}},  // 'x'
    {0x0079, {0x101, 0x10E, 0xF0, 0x70, 0x0E, 0x01, 0x00}},  // 'y'
    {0x007A, {0x181, 0x1C1, 0x161, 0x139, 0x10D, 0x107, 0x103}},  // 'z'
    {0x007B, {0x00, 0x10, 0x30, 0x1CF, 0x101, 0x00, 0x00}},  // '{'
    {0x007C, {0x00, 0x00, 0x00, 0x1FF, 0x00, 0x00, 0x00}},  // '|'
    {0x007D, {0x00, 0x101, 0x1CF, 0x30, 0x10, 0x00, 0x00}},  // '}'
    {0x007E, {0x10, 0x08, 0x08, 0x10, 0x20, 0x20, 0x10}},  // '~'
};

// Extra non-ASCII code points used by the maximum-content fixture.
static constexpr TestGlyph kTestGlyphsExtras[] = {
    {0x00B0, {0x38, 0x6C, 0xC6, 0x82, 0xC6, 0x7C, 0x38}},  // '°'
    {0x2014, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}},  // '—'
};

const TestGlyph* findTestGlyph(const uint32_t codePoint) {
  if (codePoint >= 32 && codePoint <= 126) {
    return &kTestGlyphsAscii[codePoint - 32];
  }
  for (const TestGlyph& glyph : kTestGlyphsExtras) {
    if (glyph.codePoint == codePoint) {
      return &glyph;
    }
  }
  return nullptr;
}

class PbmCanvas final : public dashboard::v3::DashboardV3Canvas {
 public:
  static constexpr int kWidth = 528;
  static constexpr int kHeight = 792;

  PbmCanvas() : pixels_(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight), 0) {}

  int width() const override { return kWidth; }
  int height() const override { return kHeight; }

  void fill(const dashboard::v3::Rect rect, const bool black) override {
    const int x0 = std::max(0, rect.x);
    const int y0 = std::max(0, rect.y);
    const int x1 = std::min(kWidth, rect.x + rect.width);
    const int y1 = std::min(kHeight, rect.y + rect.height);
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        setPixel(x, y, black);
      }
    }
  }

  void line(const int x1, const int y1, const int x2, const int y2, const bool black) override {
    // Bresenham; out-of-canvas pixels are dropped by setPixel.
    int x = x1;
    int y = y1;
    const int dx = std::abs(x2 - x1);
    const int sx = x1 < x2 ? 1 : -1;
    const int dy = -std::abs(y2 - y1);
    const int sy = y1 < y2 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
      setPixel(x, y, black);
      if (x == x2 && y == y2) break;
      const int doubled = 2 * error;
      if (doubled >= dy) {
        error += dy;
        x += sx;
      }
      if (doubled <= dx) {
        error += dx;
        y += sy;
      }
    }
  }

  void rect(const dashboard::v3::Rect rect, const bool black) override {
    line(rect.x, rect.y, rect.x + rect.width - 1, rect.y, black);
    line(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, black);
    line(rect.x, rect.y, rect.x, rect.y + rect.height - 1, black);
    line(rect.x + rect.width - 1, rect.y, rect.x + rect.width - 1, rect.y + rect.height - 1, black);
  }

  void text(const dashboard::v3::TextSpec& spec, const char* value) override {
    // Renderer strings are UTF-8 (e.g. "17° / 24°"); decode into a bounded
    // code-point buffer.
    uint32_t codePoints[64];
    int count = 0;
    const uint8_t* source = reinterpret_cast<const uint8_t*>(value);
    while (*source != '\0' && count < 64) {
      uint32_t codePoint = 0;
      if ((*source & 0x80U) == 0) {
        codePoint = *source++;
      } else if ((*source & 0xE0U) == 0xC0U) {
        codePoint = static_cast<uint32_t>(*source++ & 0x1FU) << 6;
        codePoint |= static_cast<uint32_t>(*source++ & 0x3FU);
      } else if ((*source & 0xF0U) == 0xE0U) {
        codePoint = static_cast<uint32_t>(*source++ & 0x0FU) << 12;
        codePoint |= static_cast<uint32_t>(*source++ & 0x3FU) << 6;
        codePoint |= static_cast<uint32_t>(*source++ & 0x3FU);
      } else {
        ++source;  // skip a malformed byte rather than desynchronizing
        continue;
      }
      codePoints[count++] = codePoint;
    }
    if (count == 0) {
      return;
    }

    // Scale the 7x9 glyphs from the bounds height, then shrink further so the
    // whole label still fits its bounds. The production renderer truncates
    // overlength values; a complete label is easier to review geometrically.
    constexpr int kGlyphColumns = 7;
    constexpr int kGlyphRows = 9;
    constexpr int kAdvance = kGlyphColumns + 1;
    const int heightScale = std::max(1, spec.bounds.height / kGlyphRows);
    const int widthScale = std::max(1, spec.bounds.width / (kAdvance * count - 1));
    const int scale = std::min(5, std::min(heightScale, widthScale));
    const int textWidth = count * kAdvance * scale - scale;

    int x = spec.bounds.x;
    if (spec.align == dashboard::v3::TextAlign::Center) {
      x += (spec.bounds.width - textWidth) / 2;
    }
    if (spec.align == dashboard::v3::TextAlign::Right) {
      x += spec.bounds.width - textWidth;
    }
    for (int index = 0; index < count; ++index) {
      drawGlyph(codePoints[index], x + index * kAdvance * scale, spec.bounds.y, scale, spec.black);
    }
    // A dithered spec (the focus row's caption) is knocked back to a half-tone
    // so the artifact shows the secondary line the panel would draw.
    if (spec.dithered) {
      const int height = 9 * scale;
      for (int yy = spec.bounds.y; yy < spec.bounds.y + height; ++yy) {
        for (int xx = x; xx < x + textWidth; ++xx) {
          if (!dashboard::v3::shadeCoversPixel(dashboard::v3::Shade::Half, xx, yy)) setPixel(xx, yy, false);
        }
      }
    }
  }

  void icon(const uint8_t, const dashboard::v3::Rect bounds, const bool black) override {
    // Bounded placeholder: a filled disk centered in the icon bounds.
    // Production icon rasterization is separately target-compiled, so the exact
    // icon glyphs are out of scope for this host-side artifact.
    const int centerX = bounds.x + bounds.width / 2;
    const int centerY = bounds.y + bounds.height / 2;
    const int radiusX = bounds.width / 2;
    const int radiusY = bounds.height / 2;
    for (int y = bounds.y; y < bounds.y + bounds.height; ++y) {
      for (int x = bounds.x; x < bounds.x + bounds.width; ++x) {
        const int dx = x - centerX;
        const int dy = y - centerY;
        if (dx * dx * radiusY * radiusY + dy * dy * radiusX * radiusX <=
            radiusX * radiusX * radiusY * radiusY) {
          setPixel(x, y, black);
        }
      }
    }
  }

  bool writePbm(const std::string& path) const {
    static_assert(kWidth % 8 == 0, "PBM row packing assumes byte-aligned rows");
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
      return false;
    }
    out << "P4\n" << width() << " " << height() << "\n";
    constexpr int kRowBytes = kWidth / 8;
    std::array<uint8_t, kRowBytes> row{};
    for (int y = 0; y < kHeight; ++y) {
      row.fill(0);
      for (int x = 0; x < kWidth; ++x) {
        if (pixels_[static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)] != 0) {
          row[static_cast<size_t>(x) / 8] |= static_cast<uint8_t>(0x80U >> (x & 7));
        }
      }
      out.write(reinterpret_cast<const char*>(row.data()), kRowBytes);
    }
    return out.good();
  }

  void shade(const dashboard::v3::Rect bounds, const dashboard::v3::Shade level) override {
    if (level == dashboard::v3::Shade::None) return;
    for (int y = bounds.y; y < bounds.y + bounds.height; ++y) {
      for (int x = bounds.x; x < bounds.x + bounds.width; ++x) {
        if (dashboard::v3::shadeCoversPixel(level, x, y)) setPixel(x, y, true);
      }
    }
  }

  const std::vector<uint8_t>& pixels() const { return pixels_; }

 private:
  void setPixel(const int x, const int y, const bool black) {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) {
      return;
    }
    pixels_[static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)] = black ? 1 : 0;
  }

  void drawGlyph(const uint32_t codePoint, const int x, const int y, const int scale, const bool black) {
    const TestGlyph* glyph = findTestGlyph(codePoint);
    if (glyph == nullptr) {
      rect({x, y, 7 * scale, 9 * scale}, black);  // fallback: hollow box
      return;
    }
    for (int column = 0; column < 7; ++column) {
      for (int row = 0; row < 9; ++row) {
        if ((glyph->columns[column] & (1U << row)) == 0) {
          continue;
        }
        for (int yy = 0; yy < scale; ++yy) {
          for (int xx = 0; xx < scale; ++xx) {
            setPixel(x + column * scale + xx, y + row * scale + yy, black);
          }
        }
      }
    }
  }

  std::vector<uint8_t> pixels_;
};

TEST(DashboardV3Renderer, MaximumContentWritesPbmArtifactWhenEnvDirIsSet) {
  const char* artifactDir = std::getenv("DASHBOARD_V3_PBM_DIR");
  if (artifactDir == nullptr || artifactDir[0] == '\0') {
    GTEST_SKIP() << "DASHBOARD_V3_PBM_DIR is not set; skipping the PBM artifact write";
  }

  PbmCanvas canvas;
  dashboard::v3::renderDashboardV3(canvas, maximumContentPackage(), /*minuteOfDay=*/12 * 60);

  const std::string path = std::string(artifactDir) + "/dashboard-v3-maximum.pbm";
  ASSERT_TRUE(canvas.writePbm(path)) << "could not write " << path;

  // Read the artifact back: header must declare 528x792 and the payload must be
  // one bit per pixel, MSB-first, 66 bytes per row.
  std::ifstream in(path, std::ios::binary);
  ASSERT_TRUE(in.is_open()) << "could not reopen " << path;
  std::string magic;
  std::string dimensions;
  ASSERT_TRUE(std::getline(in, magic));
  ASSERT_TRUE(std::getline(in, dimensions));
  EXPECT_EQ(magic, "P4");
  EXPECT_EQ(dimensions, "528 792");
  in.seekg(0, std::ios::end);
  const std::streamoff fileSize = in.tellg();
  constexpr std::streamoff kHeaderBytes = 11;  // "P4\n528 792\n"
  EXPECT_EQ(fileSize, kHeaderBytes + static_cast<std::streamoff>(528 * 792 / 8));
  EXPECT_NE(std::find(canvas.pixels().begin(), canvas.pixels().end(), 1), canvas.pixels().end())
      << "the artifact must contain black pixels (the header is a black band)";
}

}  // namespace
