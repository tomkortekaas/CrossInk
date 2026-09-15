#include "DashboardV3Layout.h"

#include <algorithm>

namespace dashboard::v3 {
namespace {

constexpr int REFERENCE_WIDTH = 528;
constexpr int REFERENCE_HEIGHT = 792;
constexpr int HEADER_HEIGHT = 77;
// The rain band shrank from 117 when its chart became the mock-up's fixed
// 24 px intensity strip. The 21 px it gave up went to the body, which is the
// band that runs out of room first: it is where the agenda rows live.
constexpr int RAIN_HEIGHT = 96;
constexpr int TRAFFIC_HEIGHT = 77;
constexpr int BODY_HEIGHT = 480;
constexpr int LEFT_COLUMN_WIDTH = 270;

int scaled(const int available, const int reference) {
  return available * reference / REFERENCE_HEIGHT;
}

}  // namespace

DashboardV3Rects computeDashboardV3Layout(const int width, const int height, const Insets safe) {
  const int usableWidth = width - safe.left - safe.right;
  const int usableHeight = height - safe.top - safe.bottom;
  if (usableWidth <= 0 || usableHeight <= 0 || safe.left < 0 || safe.top < 0 || safe.right < 0 || safe.bottom < 0) {
    return {};
  }

  const int headerHeight = scaled(usableHeight, HEADER_HEIGHT);
  const int rainHeight = scaled(usableHeight, RAIN_HEIGHT);
  const int trafficHeight = scaled(usableHeight, TRAFFIC_HEIGHT);
  const int bodyHeight = scaled(usableHeight, BODY_HEIGHT);
  const int footerHeight = usableHeight - headerHeight - rainHeight - trafficHeight - bodyHeight;

  DashboardV3Rects result{};
  int y = safe.top;
  result.header = {safe.left, y, usableWidth, headerHeight};
  y += headerHeight;
  result.rain = {safe.left, y, usableWidth, rainHeight};
  y += rainHeight;
  result.traffic = {safe.left, y, usableWidth, trafficHeight};
  y += trafficHeight;
  result.body = {safe.left, y, usableWidth, bodyHeight};
  y += bodyHeight;
  result.footer = {safe.left, y, usableWidth, footerHeight};

  const int leftWidth = usableWidth * LEFT_COLUMN_WIDTH / REFERENCE_WIDTH;
  result.bodyLeft = {safe.left, result.body.y, leftWidth, bodyHeight};
  result.bodyRight = {safe.left + leftWidth, result.body.y, usableWidth - leftWidth, bodyHeight};
  return result;
}

namespace {

// The approved 8A design, measured at its reference size. Only the band heights
// scale with the canvas; the agenda's text columns stay in pixels because the
// faces themselves are fixed-size (a Micro clock measures 47 px on every
// canvas), so scaling them would just move the text out of its box.
constexpr int HEADER_8A_HEIGHT = 62;
constexpr int RAIN_8A_HEIGHT = 40;
// The focus row is no longer a band: the agenda's first day group draws this
// much for row 0, so the agenda absorbs the 58 px and grows from 394 to 452.
constexpr int AGENDA_8A_HERO_HEIGHT = 58;
constexpr int AGENDA_8A_HEIGHT = 452;
constexpr int KPI_8A_HEIGHT = 140;
constexpr int MARKETS_8A_HEIGHT = 34;

constexpr int AGENDA_8A_HEADING_HEIGHT = 19;  // Micro 17 + 2
constexpr int AGENDA_8A_RIBBON_HEIGHT = 10;
// Tick (4) + the numeric 8/12/18 label under it (Micro 17) + gap (2). The
// labels are what a reader lines an event up against, so they get a real row of
// their own rather than being crammed against the strip; the 17 px they cost
// per day is what pushes a third *full* day out of the band.
constexpr int AGENDA_8A_RIBBON_GAP = 22;
constexpr int AGENDA_8A_ROW_HEIGHT = 26;      // Body 21 on the face's own 26 px line
constexpr int AGENDA_8A_SUMMARY_HEIGHT = 21;  // Micro 17 + 4
constexpr int AGENDA_8A_DAY_GAP = 8;
constexpr int AGENDA_8A_DAY_PAD = 8;          // top and bottom of the content box

constexpr int AGENDA_8A_TIME_COLUMN = 52;  // "00:00" measures 47 px at Micro
constexpr int AGENDA_8A_RULE_GAP = 4;
constexpr int AGENDA_8A_TITLE_GAP = 10;
constexpr int AGENDA_8A_DURATION_COLUMN = 56;

/// Reference metrics scaled onto the real band, never below one pixel: a
/// canvas small enough to round a row height to zero must still produce a
/// layout the renderer can draw into without dividing by anything.
int scaledMetric(const int available, const int reference) {
  return std::max(1, scaled(available, reference));
}

}  // namespace

Dashboard8ARects computeDashboard8ALayout(const int width, const int height, const Insets safe) {
  const int usableWidth = width - safe.left - safe.right;
  const int usableHeight = height - safe.top - safe.bottom;
  if (usableWidth <= 0 || usableHeight <= 0 || safe.left < 0 || safe.top < 0 || safe.right < 0 ||
      safe.bottom < 0) {
    return {};
  }

  Dashboard8ARects result{};
  const int headerHeight = scaled(usableHeight, HEADER_8A_HEIGHT);
  const int rainHeight = scaled(usableHeight, RAIN_8A_HEIGHT);
  const int agendaHeight = scaled(usableHeight, AGENDA_8A_HEIGHT);
  const int kpiHeight = scaled(usableHeight, KPI_8A_HEIGHT);
  const int marketsHeight = scaled(usableHeight, MARKETS_8A_HEIGHT);
  // The quote band takes the remainder so the six bands tile the usable
  // height exactly instead of leaving a seam at the bottom edge. The hero band
  // is gone, so it contributes nothing here: the agenda already carries it.
  const int quoteHeight = usableHeight - headerHeight - rainHeight - agendaHeight - kpiHeight - marketsHeight;

  int y = safe.top;
  result.header = {safe.left, y, usableWidth, headerHeight};
  y += headerHeight;
  result.rain = {safe.left, y, usableWidth, rainHeight};
  y += rainHeight;
  // Zero-height marker where the old standalone hero band began, kept so callers
  // that still name `hero` keep working; nothing is drawn there any more.
  result.hero = {safe.left, y, usableWidth, 0};
  result.agenda = {safe.left, y, usableWidth, agendaHeight};
  y += agendaHeight;
  result.kpi = {safe.left, y, usableWidth, kpiHeight};
  y += kpiHeight;
  result.markets = {safe.left, y, usableWidth, marketsHeight};
  y += marketsHeight;
  result.quote = {safe.left, y, usableWidth, quoteHeight};

  const int kpiLeftWidth = usableWidth / 2;
  result.kpiLeft = {safe.left, result.kpi.y, kpiLeftWidth, kpiHeight};
  result.kpiRight = {safe.left + kpiLeftWidth, result.kpi.y, usableWidth - kpiLeftWidth, kpiHeight};
  return result;
}

Agenda8ARects computeAgenda8ALayout(const int width, const int height, const Insets safe) {
  const Dashboard8ARects bands = computeDashboard8ALayout(width, height, safe);
  if (bands.agenda.width <= 0 || bands.agenda.height <= 0) return {};

  const Rect band = bands.agenda;
  Agenda8ARects result{};
  result.band = band;
  result.content = {band.x + DASHBOARD8A_PAD, band.y + AGENDA_8A_DAY_PAD, band.width - 2 * DASHBOARD8A_PAD,
                    band.height - 2 * AGENDA_8A_DAY_PAD};
  const int scaleHeight = height - safe.top - safe.bottom;
  result.headingHeight = scaledMetric(scaleHeight, AGENDA_8A_HEADING_HEIGHT);
  result.heroHeight = scaledMetric(scaleHeight, AGENDA_8A_HERO_HEIGHT);
  result.ribbonHeight = scaledMetric(scaleHeight, AGENDA_8A_RIBBON_HEIGHT);
  result.ribbonGap = scaledMetric(scaleHeight, AGENDA_8A_RIBBON_GAP);
  result.rowHeight = scaledMetric(scaleHeight, AGENDA_8A_ROW_HEIGHT);
  result.summaryHeight = scaledMetric(scaleHeight, AGENDA_8A_SUMMARY_HEIGHT);
  result.dayGap = scaledMetric(scaleHeight, AGENDA_8A_DAY_GAP);
  result.timeColumnWidth = AGENDA_8A_TIME_COLUMN;
  result.ruleX = result.content.x + result.timeColumnWidth + AGENDA_8A_RULE_GAP;
  result.titleX = result.content.x + result.timeColumnWidth + AGENDA_8A_TITLE_GAP;
  result.durationWidth = AGENDA_8A_DURATION_COLUMN;
  if (result.content.width <= 0 || result.content.height <= 0) return {};
  return result;
}

int agenda8ADayGroupHeight(const Agenda8ARects& agenda, const int rowCount, const int summaryCount) {
  if (rowCount < 0 || summaryCount < 0) return 0;
  return agenda.headingHeight + agenda.ribbonHeight + agenda.ribbonGap + rowCount * agenda.rowHeight +
         summaryCount * agenda.summaryHeight;
}

int agendaRibbonX(const Rect ribbon, const int minute, const int startMinute, const int endMinute) {
  if (ribbon.width <= 0 || endMinute <= startMinute) return ribbon.x;
  const int clamped = std::clamp(minute, startMinute, endMinute);
  return ribbon.x + (clamped - startMinute) * ribbon.width / (endMinute - startMinute);
}

}  // namespace dashboard::v3
