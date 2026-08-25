#include "DashboardV3Renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "DashboardDateFields.h"

namespace dashboard::v3 {
namespace {

constexpr int PAD = 16;

// Icon ids from the dashboard icon catalog (dashboardIconTable.h). The
// catalog is generated from dashboard-icons.txt; these constants are the
// stable ids the wire format and the icon table agree on.
constexpr uint8_t ICON_SUN = 1;
constexpr uint8_t ICON_WIND = 6;
constexpr uint8_t ICON_SUNRISE = 11;
constexpr uint8_t ICON_SUNSET = 12;
constexpr uint8_t ICON_BATTERY = 17;
constexpr uint8_t ICON_HOUSE = 20;
constexpr uint8_t ICON_CAR = 25;
constexpr uint8_t ICON_TRAFFIC_CONE = 27;
constexpr uint8_t ICON_MAP_PIN = 28;
constexpr uint8_t ICON_CALENDAR = 42;
constexpr uint8_t ICON_FOOTPRINTS = 47;

void label(DashboardV3Canvas& canvas, const Rect bounds, const char* value, const FontRole font = FontRole::Utility10,
           const bool bold = false, const bool black = true, const TextAlign align = TextAlign::Left) {
  canvas.text({bounds, font, align, bold, black}, value);
}

void formatPercent(const uint8_t value, char (&out)[8]) {
  if (value == UINT8_MAX) {
    std::snprintf(out, sizeof(out), "—");
  } else {
    std::snprintf(out, sizeof(out), "%u%%", static_cast<unsigned>(value));
  }
}

template <size_t Size>
void copyField(const std::array<uint8_t, Size>& source, const uint8_t length, char (&out)[Size + 1]) {
  const size_t boundedLength = std::min(static_cast<size_t>(length), Size);
  std::memcpy(out, source.data(), boundedLength);
  out[boundedLength] = '\0';
}

void formatMinute(const uint16_t minute, char (&out)[8]) {
  std::snprintf(out, sizeof(out), "%02u:%02u", static_cast<unsigned>(minute / 60),
                static_cast<unsigned>(minute % 60));
}

struct CivilDate {
  int year;
  int month;
  int day;
};

// Howard Hinnant's civil_from_days, expressed against the Unix epoch
// (1970-01-01 is day 0). Pure integer arithmetic: no <ctime>, no timezone
// database, so the header can format the package's generatedAt on the C3
// without pulling in the SDK. generatedAt is treated as UTC epoch seconds;
// the package carries no timezone field, which is the only way to interpret
// it on the receiver.
CivilDate civilFromDays(int64_t days) {
  int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return {static_cast<int>(y + (m <= 2)), static_cast<int>(m), static_cast<int>(d)};
}

void uppercaseAscii(const char* input, char (&output)[8]) {
  size_t index = 0;
  for (; input[index] != '\0' && index + 1 < sizeof(output); ++index) {
    const char c = input[index];
    output[index] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  output[index] = '\0';
}

// The header's date column: the package timestamp as a compact Dutch date.
// A zero timestamp means the phone never sent a usable time, so a dash is
// shown rather than an invented date (same rule the date widget uses).
void formatDateColumn(const uint64_t generatedAt, char (&day)[8], char (&dateLabel)[16]) {
  if (generatedAt == 0) {
    std::snprintf(day, sizeof(day), "—");
    std::snprintf(dateLabel, sizeof(dateLabel), "— —");
    return;
  }
  const int64_t days = static_cast<int64_t>(generatedAt / 86400ULL);
  const CivilDate date = civilFromDays(days);
  std::snprintf(day, sizeof(day), "%d", date.day);
  // Epoch day 0 (1970-01-01) was a Thursday; Weekday::Thursday == 3.
  const auto weekday = static_cast<dashboard::Weekday>((days + 3) % 7);
  char weekdayUpper[8];
  char monthUpper[8];
  uppercaseAscii(dashboard::weekdayAbbreviation(weekday), weekdayUpper);
  uppercaseAscii(dashboard::monthAbbreviation(static_cast<uint8_t>(date.month)), monthUpper);
  std::snprintf(dateLabel, sizeof(dateLabel), "%s %s", weekdayUpper, monthUpper);
}

void formatSteps(const uint16_t steps, const uint16_t stepGoal, char (&out)[24]) {
  if (steps == UINT16_MAX) {
    std::snprintf(out, sizeof(out), "—");
    return;
  }
  if (stepGoal == UINT16_MAX) {
    std::snprintf(out, sizeof(out), "%u", static_cast<unsigned>(steps));
    return;
  }
  std::snprintf(out, sizeof(out), "%u.%03u / %u.%03u", static_cast<unsigned>(steps / 1000),
                static_cast<unsigned>(steps % 1000), static_cast<unsigned>(stepGoal / 1000),
                static_cast<unsigned>(stepGoal % 1000));
}

void renderHeader(DashboardV3Canvas& canvas, const Rect rect, const DashboardV3Package& package,
                  const uint16_t minuteOfDay) {
  canvas.fill(rect, true);
  const int columnWidth = rect.width / 4;
  for (int column = 1; column < 4; ++column) {
    const int x = rect.x + column * columnWidth;
    canvas.line(x, rect.y + 12, x, rect.y + rect.height - 12, false);
  }

  // Column 1 - date: the package timestamp as a compact Dutch date, with the
  // day number as the visual anchor of the whole header.
  const int dateX = rect.x;
  char day[8];
  char dateLabel[16];
  formatDateColumn(package.generatedAt, day, dateLabel);
  canvas.icon(ICON_CALENDAR, {dateX + 14, rect.y + 15, 32, 32}, false);
  label(canvas, {dateX + 50, rect.y + 13, columnWidth - 64, 34}, day, FontRole::Value28, true, false);
  label(canvas, {dateX + 50, rect.y + 50, columnWidth - 64, 16}, dateLabel, FontRole::Utility10, false, false);

  // Column 2 - weather: condition icon, current temperature, min/max range.
  // The package carries no free-text weather description, so none is drawn.
  const int weatherX = rect.x + columnWidth;
  if (package.weather.conditionIconId != 0) {
    canvas.icon(package.weather.conditionIconId, {weatherX + 12, rect.y + 18, 32, 32}, false);
  }
  char temperature[12] = "—";
  if (package.weather.currentCelsius != INT8_MIN) {
    std::snprintf(temperature, sizeof(temperature), "%d°", package.weather.currentCelsius);
  }
  label(canvas, {weatherX + 48, rect.y + 17, columnWidth - 58, 28}, temperature, FontRole::Value18, true, false);
  char weatherCaption[20] = "WEER";
  if (package.weather.minimumCelsius != INT8_MIN && package.weather.maximumCelsius != INT8_MIN) {
    std::snprintf(weatherCaption, sizeof(weatherCaption), "%d° / %d°", package.weather.minimumCelsius,
                  package.weather.maximumCelsius);
  }
  label(canvas, {weatherX + 48, rect.y + 48, columnWidth - 58, 18}, weatherCaption, FontRole::Utility10, false, false);

  // Column 3 - wind.
  const int windX = rect.x + 2 * columnWidth;
  canvas.icon(ICON_WIND, {windX + 14, rect.y + 18, 32, 32}, false);
  char wind[12] = "—";
  if (package.weather.windKilometersPerHour != UINT8_MAX) {
    std::snprintf(wind, sizeof(wind), "%u", static_cast<unsigned>(package.weather.windKilometersPerHour));
  }
  label(canvas, {windX + 50, rect.y + 17, columnWidth - 64, 28}, wind, FontRole::Value18, true, false);
  label(canvas, {windX + 50, rect.y + 48, columnWidth - 64, 18}, "KM/U", FontRole::Utility10, false, false);

  // Column 4 - sun: the next sunrise or sunset, with the matching icon.
  const int sunX = rect.x + 3 * columnWidth;
  uint16_t sunMinute = UINT16_MAX;
  uint8_t sunIconId = ICON_SUN;
  const char* caption = "ZON";
  if (package.weather.sunriseTodayMinute != UINT16_MAX && minuteOfDay < package.weather.sunriseTodayMinute) {
    sunMinute = package.weather.sunriseTodayMinute;
    caption = "ZON OP";
    sunIconId = ICON_SUNRISE;
  } else if (package.weather.sunsetTodayMinute != UINT16_MAX && minuteOfDay < package.weather.sunsetTodayMinute) {
    sunMinute = package.weather.sunsetTodayMinute;
    caption = "ZON ONDER";
    sunIconId = ICON_SUNSET;
  } else if (package.weather.sunriseTomorrowMinute != UINT16_MAX) {
    sunMinute = package.weather.sunriseTomorrowMinute;
    caption = "MORGEN OP";
    sunIconId = ICON_SUNRISE;
  }
  char sunTime[8] = "—";
  if (sunMinute != UINT16_MAX) {
    std::snprintf(sunTime, sizeof(sunTime), "%02u:%02u", static_cast<unsigned>(sunMinute / 60),
                  static_cast<unsigned>(sunMinute % 60));
  }
  canvas.icon(sunIconId, {sunX + 14, rect.y + 18, 32, 32}, false);
  label(canvas, {sunX + 50, rect.y + 17, columnWidth - 64, 28}, sunTime, FontRole::Value18, true, false,
        TextAlign::Right);
  label(canvas, {sunX + 50, rect.y + 48, columnWidth - 64, 18}, caption, FontRole::Utility10, false, false,
        TextAlign::Right);
}

void renderRain(DashboardV3Canvas& canvas, const Rect rect, const DashboardV3Package& package) {
  label(canvas, {rect.x + PAD, rect.y + 12, 150, 20}, "REGEN", FontRole::Utility10, true);
  const char* heating = !package.heatingKnown ? "VERWARMING —" : package.heatingAllowed ? "VERWARMING AAN" : "VERWARMING UIT";
  label(canvas, {rect.x + rect.width - 190, rect.y + 12, 174, 20}, heating, FontRole::Utility10, true, true,
        TextAlign::Right);
  const int chartLeft = rect.x + PAD;
  const int chartBottom = rect.y + rect.height - 10;
  const int chartHeight = rect.height - 44;
  const int chartWidth = rect.width - 2 * PAD;
  for (size_t index = 0; index < RAIN_BUCKET_COUNT; ++index) {
    const int height = static_cast<int>(package.rain[index]) * chartHeight / 15;
    const int x1 = chartLeft + static_cast<int>(index) * chartWidth / RAIN_BUCKET_COUNT;
    const int x2 = chartLeft + static_cast<int>(index + 1) * chartWidth / RAIN_BUCKET_COUNT;
    if (height > 0) canvas.fill({x1 + 1, chartBottom - height, std::max(1, x2 - x1 - 1), height}, true);
  }
}

void renderTraffic(DashboardV3Canvas& canvas, const Rect rect, const DashboardV3Package& package) {
  // Left subject - the commute: the destination as caption, travel minutes as
  // the dominant value.
  char destination[MAX_DESTINATION_BYTES + 1] = "—";
  if (package.traffic.destinationLength > 0) {
    std::memcpy(destination, package.traffic.destination.data(), package.traffic.destinationLength);
    destination[package.traffic.destinationLength] = '\0';
  }
  const int leftX = rect.x + PAD;
  canvas.icon(ICON_MAP_PIN, {leftX, rect.y + 12, 32, 32}, true);
  label(canvas, {leftX + 40, rect.y + 10, 190, 20}, destination, FontRole::Utility10, true);
  char travel[16] = "—";
  if (package.traffic.travelMinutes != UINT16_MAX) {
    std::snprintf(travel, sizeof(travel), "%u MIN", static_cast<unsigned>(package.traffic.travelMinutes));
  }
  label(canvas, {leftX + 40, rect.y + 30, 190, 40}, travel, FontRole::Value28, true);

  // Right subject - the national jam: congestion distance as the dominant
  // value, "KM FILE" as its unit caption. The classification byte is
  // validated by the decoder but has no documented meaning in this repo, so
  // it is deliberately not turned into a label here.
  const int rightX = rect.x + rect.width - PAD;
  canvas.icon(ICON_TRAFFIC_CONE, {rightX - 48, rect.y + 12, 32, 32}, true);
  label(canvas, {rightX - 246, rect.y + 10, 190, 20}, "KM FILE", FontRole::Utility10, true, true,
        TextAlign::Right);
  char congestion[20] = "—";
  if (package.traffic.nationalCongestionKilometers != UINT16_MAX) {
    std::snprintf(congestion, sizeof(congestion), "%u",
                  static_cast<unsigned>(package.traffic.nationalCongestionKilometers));
  }
  label(canvas, {rightX - 246, rect.y + 30, 190, 40}, congestion, FontRole::Value28, true, true,
        TextAlign::Right);
}

void renderBody(DashboardV3Canvas& canvas, const DashboardV3Rects& layout, const DashboardV3Package& package) {
  label(canvas, {layout.bodyLeft.x + PAD, layout.bodyLeft.y + 12, layout.bodyLeft.width - 2 * PAD, 24}, "AGENDA",
        FontRole::Heading14, true);
  label(canvas, {layout.bodyRight.x + PAD, layout.bodyRight.y + 12, layout.bodyRight.width - 2 * PAD, 24}, "STATUS",
        FontRole::Heading14, true);
  canvas.line(layout.bodyLeft.x + PAD, layout.bodyLeft.y + 36, layout.bodyLeft.x + layout.bodyLeft.width - PAD,
              layout.bodyLeft.y + 36, true);

  constexpr int timelineX = 72;
  constexpr int agendaStartY = 52;
  constexpr int agendaRowHeight = 76;
  if (package.agendaCount > 0) {
    const int firstY = layout.bodyLeft.y + agendaStartY;
    const int lastY = firstY + (package.agendaCount - 1) * agendaRowHeight;
    canvas.line(layout.bodyLeft.x + timelineX, firstY, layout.bodyLeft.x + timelineX, lastY + 8, true);
  }
  for (size_t index = 0; index < package.agendaCount; ++index) {
    const AgendaRow& row = package.agenda[index];
    const int rowY = layout.bodyLeft.y + agendaStartY + static_cast<int>(index) * agendaRowHeight;
    char time[8];
    formatMinute(row.minuteOfDay, time);
    char title[MAX_AGENDA_TITLE_BYTES + 1];
    copyField(row.title, row.titleLength, title);
    char detail[MAX_AGENDA_DETAIL_BYTES + 1];
    copyField(row.detail, row.detailLength, detail);
    label(canvas, {layout.bodyLeft.x + PAD, rowY, 48, 18}, time, FontRole::Utility10, true, true,
          TextAlign::Right);
    canvas.fill({layout.bodyLeft.x + timelineX - 3, rowY + 5, 7, 7}, true);
    if (index == 0) canvas.rect({layout.bodyLeft.x + 78, rowY - 6, layout.bodyLeft.width - 94, 54}, true);
    label(canvas, {layout.bodyLeft.x + 84, rowY, layout.bodyLeft.width - 100, 24}, title, FontRole::Heading14,
          index == 0);
    if (detail[0] != '\0') {
      label(canvas, {layout.bodyLeft.x + 84, rowY + 24, layout.bodyLeft.width - 100, 18}, detail,
            FontRole::Utility10);
    }
  }

  int y = layout.bodyRight.y + 48;
  const char* names[] = {"X3", "AUTO", "THUIS"};
  const uint8_t iconIds[] = {ICON_BATTERY, ICON_CAR, ICON_HOUSE};
  const uint8_t values[] = {package.status.x3Battery, package.status.vehicleBattery, package.status.homeBattery};
  constexpr int statusRowHeight = 32;
  for (size_t index = 0; index < 3; ++index) {
    const int iconX = layout.bodyRight.x + PAD;
    const int labelX = iconX + 40;
    const int percentX = layout.bodyRight.x + layout.bodyRight.width - PAD - 36;
    canvas.icon(iconIds[index], {iconX, y, 32, 32}, true);
    label(canvas, {labelX, y + 6, 60, 18}, names[index], FontRole::Utility10, true);
    char percent[8];
    formatPercent(values[index], percent);
    label(canvas, {percentX, y + 6, 36, 18}, percent, FontRole::Utility10, true, true, TextAlign::Right);
    const int barX = labelX;
    const int barWidth = percentX - 8 - barX;
    canvas.rect({barX, y + 24, barWidth, 6}, true);
    if (values[index] != UINT8_MAX && values[index] > 0) {
      canvas.fill({barX + 1, y + 25, (barWidth - 2) * values[index] / 100, 4}, true);
    }
    y += statusRowHeight;
  }

  // Steps stay a distinct row below the three battery bars: icon, label and
  // value, deliberately without another progress bar.
  const int stepsY = y + 6;
  canvas.icon(ICON_FOOTPRINTS, {layout.bodyRight.x + PAD, stepsY + 3, 24, 24}, true);
  label(canvas, {layout.bodyRight.x + PAD + 40, stepsY + 4, 90, 18}, "STAPPEN", FontRole::Utility10, true);
  char stepsValue[24];
  formatSteps(package.status.steps, package.status.stepGoal, stepsValue);
  label(canvas, {layout.bodyRight.x + layout.bodyRight.width - PAD - 130, stepsY + 4, 130, 18}, stepsValue,
        FontRole::Utility10, true, true, TextAlign::Right);

  const int marketsHeadingY = stepsY + 44;
  label(canvas, {layout.bodyRight.x + PAD, marketsHeadingY, 160, 24}, "MARKTEN", FontRole::Heading14, true);
  int marketY = marketsHeadingY + 28;
  for (size_t index = 0; index < package.marketCount; ++index) {
    const MarketRow& row = package.markets[index];
    char market[MAX_MARKET_LABEL_BYTES + 1];
    copyField(row.label, row.labelLength, market);
    char change[16] = "—";
    if (row.changeBasisPoints != INT16_MIN) {
      const int value = row.changeBasisPoints;
      std::snprintf(change, sizeof(change), "%c%d,%02d%%", value >= 0 ? '+' : '-', std::abs(value) / 100,
                    std::abs(value) % 100);
    }
    label(canvas, {layout.bodyRight.x + PAD, marketY, 100, 20}, market, FontRole::Utility10, true);
    label(canvas, {layout.bodyRight.x + 118, marketY, layout.bodyRight.width - 134, 20}, change,
          FontRole::Utility10, true, true, TextAlign::Right);
    marketY += 26;
  }

  if (package.chatCount == 0) return;
  const int chatsHeadingY = marketsHeadingY + 108;
  label(canvas, {layout.bodyRight.x + PAD, chatsHeadingY, 170, 24}, "WHATSAPP", FontRole::Heading14, true);
  canvas.icon(65, {layout.bodyRight.x + layout.bodyRight.width - 48, chatsHeadingY, 24, 24}, true);
  char unread[12];
  std::snprintf(unread, sizeof(unread), "%u", static_cast<unsigned>(package.unreadTotal));
  label(canvas, {layout.bodyRight.x + layout.bodyRight.width - 26, chatsHeadingY + 2, 18, 20}, unread,
        FontRole::Utility10, true, true, TextAlign::Right);
  int chatY = chatsHeadingY + 28;
  for (size_t index = 0; index < package.chatCount; ++index) {
    const ChatRow& row = package.chats[index];
    char name[MAX_CHAT_NAME_BYTES + 1];
    copyField(row.name, row.nameLength, name);
    char count[10];
    std::snprintf(count, sizeof(count), "%u", static_cast<unsigned>(row.unreadCount));
    char time[8];
    formatMinute(row.lastMessageMinuteOfDay, time);
    label(canvas, {layout.bodyRight.x + PAD, chatY, 130, 20}, name, FontRole::Utility10, true);
    label(canvas, {layout.bodyRight.x + 150, chatY, layout.bodyRight.width - 166, 20}, count, FontRole::Utility10,
          true, true, TextAlign::Right);
    label(canvas, {layout.bodyRight.x + 150, chatY + 14, layout.bodyRight.width - 166, 18}, time,
          FontRole::Utility10, false, true, TextAlign::Right);
    chatY += 34;
  }
}

void renderFooter(DashboardV3Canvas& canvas, const Rect rect) {
  label(canvas, {rect.x + PAD, rect.y + 14, rect.width - 2 * PAD, 24}, "Verbeelding is belangrijker dan kennis.",
        FontRole::Heading14, true, true, TextAlign::Center);
  label(canvas, {rect.x + PAD, rect.y + 38, rect.width - 2 * PAD, 16}, "— Albert Einstein", FontRole::Utility10,
        false, true, TextAlign::Center);
}

}  // namespace

void renderDashboardV3(DashboardV3Canvas& canvas, const DashboardV3Package& package, const uint16_t minuteOfDay) {
  const DashboardV3Rects layout = computeDashboardV3Layout(canvas.width(), canvas.height(), {});
  canvas.fill({0, 0, canvas.width(), canvas.height()}, false);
  renderHeader(canvas, layout.header, package, minuteOfDay);
  renderRain(canvas, layout.rain, package);
  renderTraffic(canvas, layout.traffic, package);
  canvas.line(0, layout.header.y + layout.header.height, canvas.width() - 1, layout.header.y + layout.header.height, true);
  canvas.line(0, layout.rain.y + layout.rain.height, canvas.width() - 1, layout.rain.y + layout.rain.height, true);
  canvas.line(0, layout.traffic.y + layout.traffic.height, canvas.width() - 1, layout.traffic.y + layout.traffic.height, true);
  canvas.line(layout.bodyRight.x, layout.body.y, layout.bodyRight.x, layout.body.y + layout.body.height - 1, true);
  renderBody(canvas, layout, package);
  canvas.line(0, layout.body.y + layout.body.height, canvas.width() - 1, layout.body.y + layout.body.height, true);
  renderFooter(canvas, layout.footer);
}

}  // namespace dashboard::v3

#ifdef CROSSINK_BLE_HANDOFF_READER

#include <GfxRenderer.h>

#include "components/icons/dashboardIconTable.h"
#include "fontIds.h"

namespace dashboard::v3 {
namespace {

int fontId(const FontRole role) {
  switch (role) {
    case FontRole::Utility10: return LEXENDDECA_10_FONT_ID;
    case FontRole::Utility12: return LEXENDDECA_12_FONT_ID;
    case FontRole::Heading14: return LEXENDDECA_14_FONT_ID;
    case FontRole::Heading16: return LEXENDDECA_16_FONT_ID;
    case FontRole::Value18: return LEXENDDECA_18_BOLD_DASH_FONT_ID;
    case FontRole::Value22: return LEXENDDECA_22_BOLD_DASH_FONT_ID;
    case FontRole::Value28: return LEXENDDECA_28_BOLD_DASH_FONT_ID;
    case FontRole::Value34: return LEXENDDECA_34_BOLD_DASH_FONT_ID;
  }
  return LEXENDDECA_10_FONT_ID;
}

void drawNaturalIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const Rect bounds, const bool black) {
  const int stride = (icon.w + 7) / 8;
  const int x = bounds.x + (bounds.width - icon.w) / 2;
  const int y = bounds.y + (bounds.height - icon.h) / 2;
  for (int row = 0; row < icon.h; ++row) {
    const uint8_t* source = icon.bits + row * stride;
    for (int column = 0; column < icon.w; ++column) {
      if ((source[column >> 3] & static_cast<uint8_t>(0x80U >> (column & 7))) == 0) {
        renderer.drawPixel(x + column, y + row, black);
      }
    }
  }
}

class GfxDashboardV3Canvas final : public DashboardV3Canvas {
 public:
  explicit GfxDashboardV3Canvas(GfxRenderer& renderer) : renderer_(renderer) {}

  int width() const override { return renderer_.getScreenWidth(); }
  int height() const override { return renderer_.getScreenHeight(); }
  void fill(const Rect rect, const bool black) override {
    renderer_.fillRect(rect.x, rect.y, rect.width, rect.height, black);
  }
  void line(const int x1, const int y1, const int x2, const int y2, const bool black) override {
    renderer_.drawLine(x1, y1, x2, y2, black);
  }
  void rect(const Rect rect, const bool black) override {
    renderer_.drawRect(rect.x, rect.y, rect.width, rect.height, black);
  }
  void text(const TextSpec& spec, const char* value) override {
    const int id = fontId(spec.font);
    const EpdFontFamily::Style style = spec.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const std::string bounded = renderer_.truncatedText(id, value, spec.bounds.width, style);
    const int textWidth = renderer_.getTextWidth(id, bounded.c_str(), style);
    int x = spec.bounds.x;
    if (spec.align == TextAlign::Center) x += (spec.bounds.width - textWidth) / 2;
    if (spec.align == TextAlign::Right) x += spec.bounds.width - textWidth;
    renderer_.drawText(id, x, spec.bounds.y, bounded.c_str(), spec.black, style);
  }
  void icon(const uint8_t iconId, const Rect bounds, const bool black) override {
    if (iconId == 0 || iconId > DASHBOARD_ICON_COUNT) return;
    const freeink::Icon* selected = bounds.width >= 40 && bounds.height >= 40 ? DASHBOARD_ICONS_48[iconId]
                                                                            : DASHBOARD_ICONS_32[iconId];
    if (selected != nullptr) drawNaturalIcon(renderer_, *selected, bounds, black);
  }

 private:
  GfxRenderer& renderer_;
};

}  // namespace

void renderDashboardV3(GfxRenderer& renderer, const DashboardV3Package& package, const uint16_t minuteOfDay) {
  GfxDashboardV3Canvas canvas(renderer);
  renderDashboardV3(canvas, package, minuteOfDay);
}

}  // namespace dashboard::v3

#endif
