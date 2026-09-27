#include <gtest/gtest.h>

#include "DashboardV3Layout.h"

namespace {

TEST(DashboardV3Layout, PortraitCanvasMatchesApprovedBandsAndColumns) {
  const auto layout = dashboard::v3::computeDashboardV3Layout(528, 792, {});

  EXPECT_EQ(layout.header, (dashboard::v3::Rect{0, 0, 528, 77}));
  // The rain band gave 21 px to the body when its chart became the mock-up's
  // fixed 24 px intensity strip. The five bands still tile the canvas exactly.
  EXPECT_EQ(layout.rain, (dashboard::v3::Rect{0, 77, 528, 96}));
  EXPECT_EQ(layout.traffic, (dashboard::v3::Rect{0, 173, 528, 77}));
  EXPECT_EQ(layout.body, (dashboard::v3::Rect{0, 250, 528, 480}));
  EXPECT_EQ(layout.footer, (dashboard::v3::Rect{0, 730, 528, 62}));
  EXPECT_EQ(layout.bodyLeft, (dashboard::v3::Rect{0, 250, 270, 480}));
  EXPECT_EQ(layout.bodyRight, (dashboard::v3::Rect{270, 250, 258, 480}));
}

TEST(DashboardV3Layout, SafeInsetsKeepEveryBandInsideTheUsableCanvas) {
  const dashboard::v3::Insets safe{9, 15, 9, 9};
  const auto layout = dashboard::v3::computeDashboardV3Layout(528, 792, safe);

  EXPECT_EQ(layout.header.x, 9);
  EXPECT_EQ(layout.header.y, 15);
  EXPECT_EQ(layout.header.width, 510);
  EXPECT_EQ(layout.footer.y + layout.footer.height, 783);
  EXPECT_EQ(layout.bodyLeft.width + layout.bodyRight.width, 510);
  EXPECT_EQ(layout.bodyRight.x, layout.bodyLeft.x + layout.bodyLeft.width);
}

TEST(DashboardV3Layout, InvalidInsetsProduceAnEmptyLayout) {
  const auto layout = dashboard::v3::computeDashboardV3Layout(20, 20, {11, 0, 10, 0});
  EXPECT_EQ(layout.header.width, 0);
  EXPECT_EQ(layout.footer.height, 0);
}

TEST(DashboardV3Layout8A, ReferenceCanvasMatchesApprovedBandsAndColumns) {
  const auto layout = dashboard::v3::computeDashboard8ALayout(528, 792, {});

  // The readability refinement moves the rain band under the agenda: the six
  // bands now run header, agenda, rain, KPI, markets, quote. The rain keeps its
  // 40 px and sits directly between the agenda and the KPI band, and quote still
  // takes the remainder so the bands always reach the bottom edge exactly.
  EXPECT_EQ(layout.header, (dashboard::v3::Rect{0, 0, 528, 62}));
  EXPECT_EQ(layout.agenda, (dashboard::v3::Rect{0, 62, 528, 452}));
  EXPECT_EQ(layout.rain, (dashboard::v3::Rect{0, 514, 528, 40}));
  // The standalone hero band is gone: the agenda starts right under the header
  // and absorbs the 58 px the hero used to occupy, so the old hero rect is only a
  // zero-height marker at the agenda's top and the KPI band keeps its 554 top.
  EXPECT_EQ(layout.hero, (dashboard::v3::Rect{0, 62, 528, 0}));
  EXPECT_EQ(layout.hero.height, 0) << "the hero consumes no band of its own";
  EXPECT_EQ(layout.kpi, (dashboard::v3::Rect{0, 554, 528, 140}));
  EXPECT_EQ(layout.markets, (dashboard::v3::Rect{0, 694, 528, 34}));
  EXPECT_EQ(layout.quote, (dashboard::v3::Rect{0, 728, 528, 64}));

  // The bands tile without a seam or an overlap, in that order.
  EXPECT_EQ(layout.agenda.y, layout.header.y + layout.header.height);
  EXPECT_EQ(layout.rain.y, layout.agenda.y + layout.agenda.height);
  EXPECT_EQ(layout.rain.y + layout.rain.height, layout.kpi.y);
  EXPECT_EQ(layout.kpi.y + layout.kpi.height, layout.markets.y);
  EXPECT_EQ(layout.markets.y + layout.markets.height, layout.quote.y);
  EXPECT_EQ(layout.quote.y + layout.quote.height, 792);

  EXPECT_EQ(layout.kpiLeft, (dashboard::v3::Rect{0, 554, 264, 140}));
  EXPECT_EQ(layout.kpiRight, (dashboard::v3::Rect{264, 554, 264, 140}));
  EXPECT_EQ(layout.kpiLeft.width, layout.kpiRight.width);
}

TEST(DashboardV3Layout8A, SafeInsetsKeepEveryBandInsideTheUsableCanvas) {
  const dashboard::v3::Insets safe{9, 15, 9, 9};
  const auto layout = dashboard::v3::computeDashboard8ALayout(528, 792, safe);

  EXPECT_EQ(layout.header.x, 9);
  EXPECT_EQ(layout.rain.x, 9);
  EXPECT_EQ(layout.hero.x, 9);
  EXPECT_EQ(layout.agenda.x, 9);
  EXPECT_EQ(layout.kpi.x, 9);
  EXPECT_EQ(layout.markets.x, 9);
  EXPECT_EQ(layout.quote.x, 9);

  // The usable height shrinks from 792 to 768, so each scaled band height
  // below is 768 * reference / 792 (integer division) and quote absorbs the rest.
  EXPECT_EQ(layout.agenda.y, layout.header.y + layout.header.height);
  EXPECT_EQ(layout.hero.y, layout.agenda.y);
  EXPECT_EQ(layout.hero.height, 0);
  EXPECT_EQ(layout.rain.y, layout.agenda.y + layout.agenda.height);
  EXPECT_EQ(layout.kpi.y, layout.rain.y + layout.rain.height);
  EXPECT_EQ(layout.markets.y, layout.kpi.y + layout.kpi.height);
  EXPECT_EQ(layout.quote.y, layout.markets.y + layout.markets.height);
  EXPECT_EQ(layout.quote.y + layout.quote.height, 783);

  // 768 * 452 / 792 = 438: the agenda still carries the 58 px hero row's share,
  // and the rain it moved past keeps its own scaled 38 px.
  EXPECT_EQ(layout.agenda.height, 438);
  EXPECT_EQ(layout.rain.height, 38);
  EXPECT_EQ(layout.kpiLeft.width + layout.kpiRight.width, 510);
  EXPECT_EQ(layout.kpiLeft.width, layout.kpiRight.width);
}

TEST(DashboardV3Layout8A, InvalidInsetsProduceAnEmptyLayout) {
  const auto negativeInset = dashboard::v3::computeDashboard8ALayout(528, 792, {0, -1, 0, 0});
  const auto tooWide = dashboard::v3::computeDashboard8ALayout(20, 20, {11, 0, 10, 0});

  const auto expectAllBandsEmpty = [](const dashboard::v3::Dashboard8ARects& layout) {
    EXPECT_EQ(layout.header, (dashboard::v3::Rect{}));
    EXPECT_EQ(layout.rain, (dashboard::v3::Rect{}));
    EXPECT_EQ(layout.hero, (dashboard::v3::Rect{}));
    EXPECT_EQ(layout.agenda, (dashboard::v3::Rect{}));
    EXPECT_EQ(layout.kpi, (dashboard::v3::Rect{}));
    EXPECT_EQ(layout.kpiLeft, (dashboard::v3::Rect{}));
    EXPECT_EQ(layout.kpiRight, (dashboard::v3::Rect{}));
    EXPECT_EQ(layout.markets, (dashboard::v3::Rect{}));
    EXPECT_EQ(layout.quote, (dashboard::v3::Rect{}));
  };
  expectAllBandsEmpty(negativeInset);
  expectAllBandsEmpty(tooWide);

  const auto negativeAgenda = dashboard::v3::computeAgenda8ALayout(528, 792, {0, -1, 0, 0});
  EXPECT_EQ(negativeAgenda.content.width, 0);
  EXPECT_EQ(negativeAgenda.content.height, 0);
  const auto tooWideAgenda = dashboard::v3::computeAgenda8ALayout(20, 20, {11, 0, 10, 0});
  EXPECT_EQ(tooWideAgenda.content.width, 0);
  EXPECT_EQ(tooWideAgenda.content.height, 0);
}

TEST(DashboardV3Layout8A, AgendaGeometryMatchesReference) {
  const auto agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
  const auto bands = dashboard::v3::computeDashboard8ALayout(528, 792, {});

  // agenda is {0,62,528,452}, so content is inset 14 horizontally and 8 vertically.
  EXPECT_EQ(agenda.content, (dashboard::v3::Rect{14, 70, 500, 436}));
  // The band itself is carried too, so the day transition bar can span it.
  EXPECT_EQ(agenda.band, bands.agenda);
  EXPECT_EQ(agenda.band.y, 62);
  EXPECT_EQ(agenda.band.y + agenda.band.height, 514) << "the agenda ends where the rain band starts";
  // The content box sits 8 px above the agenda band's bottom edge.
  EXPECT_EQ(agenda.content.y + agenda.content.height + 8, bands.agenda.y + bands.agenda.height);

  // The time column is right-aligned and the title starts on one fixed x for
  // every row; the gap between them is wider than the ruler it replaced, and
  // there is no ruler anchor left in the layout at all.
  EXPECT_EQ(agenda.timeColumnWidth, 52);
  EXPECT_GT(agenda.titleX - (agenda.content.x + agenda.timeColumnWidth), 10)
      << "the refinement opens the time/title gap rather than shrinking it";
  EXPECT_GT(agenda.durationWidth, 0);
  EXPECT_EQ(agenda.durationWidth, 56);
  EXPECT_EQ(agenda.ribbonStartMinute, 420);
  EXPECT_EQ(agenda.ribbonEndMinute, 1380);
  // The first day group draws row 0 as the 58 px focus row, so the layout has to
  // hand the renderer that row's height separately from any band rect.
  EXPECT_EQ(agenda.heroHeight, 58);
  // No day heading carries a blank separator any more: a group is its heading
  // line, its ribbon and the tick gap under it.
  EXPECT_EQ(dashboard::v3::agenda8ADayGroupHeight(agenda, 0),
            agenda.headingHeight + agenda.ribbonHeight + agenda.ribbonGap);
}

TEST(DashboardV3Layout8A, ReferenceAgendaFitsTheHeroDayAndTwoFullLaterDays) {
  const auto agenda = dashboard::v3::computeAgenda8ALayout(528, 792, {});
  // A three-row day, kept as a literal so this geometry stays pinned: the row
  // count is no longer a cap the renderer searches against.
  constexpr int threeRows = 3;

  // A later day of three rows and its "+N meer" line still fits inside the band.
  const int laterDayWithSummary = dashboard::v3::agenda8ADayGroupHeight(agenda, threeRows, 1);
  EXPECT_LE(laterDayWithSummary, agenda.content.height);

  // The first group's focus row stands in for row 0, so the hero day costs the
  // same 58 px whether it lists no rows or one.
  const int heroDay = dashboard::v3::agenda8ADayGroupHeight(agenda, 0) + 58;
  EXPECT_EQ(heroDay, agenda.headingHeight + 58 + agenda.ribbonHeight + agenda.ribbonGap);
  const int laterFullDay = dashboard::v3::agenda8ADayGroupHeight(agenda, threeRows);
  EXPECT_LE(heroDay + 2 * laterFullDay + 2 * agenda.dayGap, agenda.content.height);

  // The band holds the hero day plus a full three-row day plus a five-row day;
  // a six-row third day spills into the summary line.
  const int laterFiveRows = dashboard::v3::agenda8ADayGroupHeight(agenda, 5);
  EXPECT_LE(heroDay + laterFullDay + laterFiveRows + 2 * agenda.dayGap, agenda.content.height);
  const int laterSixRows = dashboard::v3::agenda8ADayGroupHeight(agenda, 6);
  EXPECT_GT(heroDay + laterFullDay + laterSixRows + 2 * agenda.dayGap, agenda.content.height);
}

TEST(DashboardV3Layout8A, RibbonXClampsAndScalesWithoutDividingByZero) {
  const dashboard::v3::Rect ribbon{0, 0, 528, 10};
  const int start = 420;
  const int end = 1380;

  EXPECT_EQ(dashboard::v3::agendaRibbonX(ribbon, start, start, end), ribbon.x);
  EXPECT_EQ(dashboard::v3::agendaRibbonX(ribbon, end, start, end), ribbon.x + ribbon.width);
  EXPECT_EQ(dashboard::v3::agendaRibbonX(ribbon, 0, start, end), ribbon.x);
  EXPECT_EQ(dashboard::v3::agendaRibbonX(ribbon, 1439, start, end), ribbon.x + ribbon.width);

  EXPECT_LE(dashboard::v3::agendaRibbonX(ribbon, start, start, end),
            dashboard::v3::agendaRibbonX(ribbon, 600, start, end));
  EXPECT_LE(dashboard::v3::agendaRibbonX(ribbon, 600, start, end),
            dashboard::v3::agendaRibbonX(ribbon, 900, start, end));
  EXPECT_LE(dashboard::v3::agendaRibbonX(ribbon, 900, start, end),
            dashboard::v3::agendaRibbonX(ribbon, end, start, end));

  EXPECT_EQ(dashboard::v3::agendaRibbonX({10, 0, 0, 10}, 600, start, end), 10);
  EXPECT_EQ(dashboard::v3::agendaRibbonX({10, 0, -5, 10}, 600, start, end), 10);
  EXPECT_EQ(dashboard::v3::agendaRibbonX(ribbon, 600, end, start), ribbon.x);
}

TEST(DashboardV3Layout8A, HalfScaleCanvasStillTilesAndKeepsMetricsDrawable) {
  const auto layout = dashboard::v3::computeDashboard8ALayout(264, 396, {});

  // 264x396 is exactly half of the reference canvas, so the bands scale in half
  // and the quote band still absorbs the remainder to reach the bottom edge.
  EXPECT_EQ(layout.agenda.y, layout.header.y + layout.header.height);
  EXPECT_EQ(layout.hero.y, layout.agenda.y);
  EXPECT_EQ(layout.rain.y, layout.agenda.y + layout.agenda.height);
  EXPECT_EQ(layout.kpi.y, layout.rain.y + layout.rain.height);
  EXPECT_EQ(layout.markets.y, layout.kpi.y + layout.kpi.height);
  EXPECT_EQ(layout.quote.y, layout.markets.y + layout.markets.height);
  EXPECT_EQ(layout.quote.y + layout.quote.height, 396);

  const auto agenda = dashboard::v3::computeAgenda8ALayout(264, 396, {});
  EXPECT_GE(agenda.headingHeight, 1);
  EXPECT_GE(agenda.heroHeight, 1);
  EXPECT_GE(agenda.ribbonHeight, 1);
  EXPECT_GE(agenda.ribbonGap, 1);
  EXPECT_GE(agenda.rowHeight, 1);
  EXPECT_GE(agenda.summaryHeight, 1);
  EXPECT_GE(agenda.dayGap, 1);
  EXPECT_GE(agenda.timeColumnWidth, 1);
  EXPECT_GE(agenda.durationWidth, 1);
}

}  // namespace
