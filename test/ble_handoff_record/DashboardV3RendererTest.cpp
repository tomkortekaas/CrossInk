#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
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
};

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
    operations.push_back({Operation::Kind::Text, spec.bounds, value, spec.black, 0, spec.align, spec.font});
  }

  void icon(const uint8_t iconId, const dashboard::v3::Rect bounds, const bool black) override {
    operations.push_back({Operation::Kind::Icon, bounds, {}, black, iconId});
  }

  void shade(const dashboard::v3::Rect bounds, const dashboard::v3::Shade level) override {
    if (level == dashboard::v3::Shade::None) return;
    operations.push_back({Operation::Kind::Shade, bounds, {}, true});
    shades.push_back(level);
  }

  std::vector<dashboard::v3::Shade> shades;

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
      "AFSPRAAK 5", "AFSPRAAK 6", "AFSPRAAK 7", "AFSPRAAK 8"};
  static const char* const agendaDetails[dashboard::v3::MAX_AGENDA_ROWS] = {
      "DETAIL 1", "DETAIL 2", "DETAIL 3", "DETAIL 4",
      "DETAIL 5", "DETAIL 6", "DETAIL 7", "DETAIL 8"};
  static_assert(std::size(agendaTitles) == dashboard::v3::MAX_AGENDA_ROWS, "one title per declared row");
  static_assert(std::size(agendaDetails) == dashboard::v3::MAX_AGENDA_ROWS, "one detail per declared row");
  for (size_t index = 0; index < dashboard::v3::MAX_AGENDA_ROWS; ++index) {
    package.agenda[index].dayOffset = static_cast<uint8_t>(index);
    package.agenda[index].minuteOfDay = static_cast<uint16_t>(9 * 60 + index);
    copyText(package.agenda[index].title, package.agenda[index].titleLength, agendaTitles[index]);
    copyText(package.agenda[index].detail, package.agenda[index].detailLength, agendaDetails[index]);
  }
  package.agendaCount = static_cast<uint8_t>(dashboard::v3::MAX_AGENDA_ROWS);

  static const char* const marketLabels[dashboard::v3::MAX_MARKETS] = {"AEX", "DOW", "DAX"};
  for (size_t index = 0; index < dashboard::v3::MAX_MARKETS; ++index) {
    copyText(package.markets[index].label, package.markets[index].labelLength, marketLabels[index]);
    package.markets[index].changeBasisPoints = static_cast<int16_t>(100 + static_cast<int16_t>(index) * 25);
  }
  package.marketCount = static_cast<uint8_t>(dashboard::v3::MAX_MARKETS);

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
// quote, which nothing else in the system would notice. These expectations are
// transcribed from Sources/DashboardCore/DashboardV3Quotes.swift.

TEST(DashboardV3Quotes, TableMatchesThePhoneTableEntryForEntry) {
  ASSERT_EQ(dashboard::v3::QUOTE_COUNT, 8u);

  const char* const expectedText[] = {
      "Verbeelding is belangrijker dan kennis.",
      "Eenvoud is de ultieme verfijning.",
      "Minder is meer.",
      "Pluk de dag.",
      "Geluk is waar voorbereiding en kans elkaar ontmoeten.",
      "Een reis van duizend mijl begint met \xc3\xa9\xc3\xa9n stap.",
      "Wie niet waagt, die niet wint.",
      "Kennis spreekt, maar wijsheid luistert.",
  };
  const char* const expectedAuthor[] = {
      "Albert Einstein", "Leonardo da Vinci",      "Ludwig Mies van der Rohe", "Horatius",
      "Seneca",          "Laozi",                  "Nederlands spreekwoord",   "Jimi Hendrix",
  };

  for (size_t index = 0; index < dashboard::v3::QUOTE_COUNT; ++index) {
    const dashboard::v3::Quote* quote = dashboard::v3::quoteForId(static_cast<uint8_t>(index));
    ASSERT_NE(quote, nullptr) << "id " << index << " must resolve";
    EXPECT_STREQ(quote->text, expectedText[index]) << "at id " << index;
    EXPECT_STREQ(quote->author, expectedAuthor[index]) << "at id " << index;
  }
}

TEST(DashboardV3Quotes, IdsAreZeroBasedAndOutOfRangeDrawsNothing) {
  const dashboard::v3::Quote* first = dashboard::v3::quoteForId(0);
  ASSERT_NE(first, nullptr) << "id 0 is a real quote, not an absent sentinel";
  EXPECT_STREQ(first->author, "Albert Einstein");
  // A newer phone may carry a longer table; an unknown id must draw nothing
  // rather than wrap around onto the wrong quote.
  EXPECT_EQ(dashboard::v3::quoteForId(static_cast<uint8_t>(dashboard::v3::QUOTE_COUNT)), nullptr);
  EXPECT_EQ(dashboard::v3::quoteForId(255), nullptr);
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
