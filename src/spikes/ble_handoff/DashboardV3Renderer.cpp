#include "DashboardV3Renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dashboard::v3 {
namespace {

constexpr int PAD = 16;

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

void renderHeader(DashboardV3Canvas& canvas, const Rect rect, const DashboardV3Package& package,
                  const uint16_t minuteOfDay) {
  canvas.fill(rect, true);
  const int columnWidth = rect.width / 4;
  for (int column = 1; column < 4; ++column) {
    const int x = rect.x + column * columnWidth;
    canvas.line(x, rect.y + 12, x, rect.y + rect.height - 12, false);
  }

  label(canvas, {rect.x + 14, rect.y + 14, columnWidth - 28, 28}, "DATUM", FontRole::Heading16, true, false);
  char range[20] = "— / —";
  if (package.weather.minimumCelsius != INT8_MIN && package.weather.maximumCelsius != INT8_MIN) {
    std::snprintf(range, sizeof(range), "%d° / %d°", package.weather.minimumCelsius, package.weather.maximumCelsius);
  }
  label(canvas, {rect.x + 14, rect.y + 48, columnWidth - 28, 18}, range, FontRole::Utility10, false, false);

  const int weatherX = rect.x + columnWidth;
  if (package.weather.conditionIconId != 0) {
    canvas.icon(package.weather.conditionIconId, {weatherX + 12, rect.y + 18, 32, 32}, false);
  }
  char temperature[12] = "—";
  if (package.weather.currentCelsius != INT8_MIN) {
    std::snprintf(temperature, sizeof(temperature), "%d°", package.weather.currentCelsius);
  }
  label(canvas, {weatherX + 48, rect.y + 17, columnWidth - 58, 28}, temperature, FontRole::Value18, true, false);
  label(canvas, {weatherX + 48, rect.y + 48, columnWidth - 58, 18}, "WEER", FontRole::Utility10, false, false);

  const int windX = rect.x + 2 * columnWidth;
  char wind[12] = "—";
  if (package.weather.windKilometersPerHour != UINT8_MAX) {
    std::snprintf(wind, sizeof(wind), "%u", static_cast<unsigned>(package.weather.windKilometersPerHour));
  }
  label(canvas, {windX + 14, rect.y + 17, columnWidth - 28, 28}, wind, FontRole::Value18, true, false);
  label(canvas, {windX + 14, rect.y + 48, columnWidth - 28, 18}, "KM/U", FontRole::Utility10, false, false);

  const int sunX = rect.x + 3 * columnWidth;
  uint16_t sunMinute = UINT16_MAX;
  const char* caption = "ZON";
  if (package.weather.sunriseTodayMinute != UINT16_MAX && minuteOfDay < package.weather.sunriseTodayMinute) {
    sunMinute = package.weather.sunriseTodayMinute;
    caption = "ZON OP";
  } else if (package.weather.sunsetTodayMinute != UINT16_MAX && minuteOfDay < package.weather.sunsetTodayMinute) {
    sunMinute = package.weather.sunsetTodayMinute;
    caption = "ZON ONDER";
  } else if (package.weather.sunriseTomorrowMinute != UINT16_MAX) {
    sunMinute = package.weather.sunriseTomorrowMinute;
    caption = "MORGEN OP";
  }
  char sunTime[8] = "—";
  if (sunMinute != UINT16_MAX) {
    std::snprintf(sunTime, sizeof(sunTime), "%02u:%02u", static_cast<unsigned>(sunMinute / 60),
                  static_cast<unsigned>(sunMinute % 60));
  }
  label(canvas, {sunX + 14, rect.y + 17, columnWidth - 28, 28}, sunTime, FontRole::Value18, true, false,
        TextAlign::Right);
  label(canvas, {sunX + 14, rect.y + 48, columnWidth - 28, 18}, caption, FontRole::Utility10, false, false,
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
  char destination[MAX_DESTINATION_BYTES + 1] = "—";
  if (package.traffic.destinationLength > 0) {
    std::memcpy(destination, package.traffic.destination.data(), package.traffic.destinationLength);
    destination[package.traffic.destinationLength] = '\0';
  }
  label(canvas, {rect.x + PAD, rect.y + 10, 180, 20}, destination, FontRole::Utility10, true);
  char travel[16] = "—";
  if (package.traffic.travelMinutes != UINT16_MAX) {
    std::snprintf(travel, sizeof(travel), "%u MIN", static_cast<unsigned>(package.traffic.travelMinutes));
  }
  label(canvas, {rect.x + PAD, rect.y + 28, 240, 40}, travel, FontRole::Value28, true);
  char congestion[20] = "—";
  if (package.traffic.nationalCongestionKilometers != UINT16_MAX) {
    std::snprintf(congestion, sizeof(congestion), "%u KM FILE",
                  static_cast<unsigned>(package.traffic.nationalCongestionKilometers));
  }
  label(canvas, {rect.x + rect.width - 220, rect.y + 10, 204, 20}, congestion, FontRole::Utility10, true, true,
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
  const uint8_t values[] = {package.status.x3Battery, package.status.vehicleBattery, package.status.homeBattery};
  for (size_t index = 0; index < 3; ++index) {
    char percent[8];
    formatPercent(values[index], percent);
    label(canvas, {layout.bodyRight.x + PAD, y, 100, 20}, names[index], FontRole::Utility10, true);
    label(canvas, {layout.bodyRight.x + 120, y, layout.bodyRight.width - 136, 20}, percent, FontRole::Utility10, true,
          true, TextAlign::Right);
    y += 28;
  }
  label(canvas, {layout.bodyRight.x + PAD, y + 12, 120, 24}, "STAPPEN", FontRole::Utility10, true);
  const int marketsHeadingY = y + 48;
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
