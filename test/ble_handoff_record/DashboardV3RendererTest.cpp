#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "DashboardV3Renderer.h"

namespace {

struct Operation {
  enum class Kind { Fill, Line, Rect, Text, Icon } kind;
  dashboard::v3::Rect bounds;
  std::string text;
  bool black = true;
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
    operations.push_back({Operation::Kind::Text, spec.bounds, value, spec.black});
  }

  void icon(const uint8_t, const dashboard::v3::Rect bounds, const bool black) override {
    operations.push_back({Operation::Kind::Icon, bounds, {}, black});
  }

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
    hasAgenda |= operation.kind == Operation::Kind::Text && operation.text == "AGENDA";
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
  package.quoteId = 1;

  static const char* const agendaTitles[dashboard::v3::MAX_AGENDA_ROWS] = {
      "AFSPRAAK 1", "AFSPRAAK 2", "AFSPRAAK 3", "AFSPRAAK 4", "AFSPRAAK 5"};
  static const char* const agendaDetails[dashboard::v3::MAX_AGENDA_ROWS] = {
      "DETAIL 1", "DETAIL 2", "DETAIL 3", "DETAIL 4", "DETAIL 5"};
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

  static const char* const chatNames[dashboard::v3::MAX_CHATS] = {"PAPA", "MAMA", "WERK"};
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

}  // namespace
