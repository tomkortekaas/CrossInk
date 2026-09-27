#pragma once

namespace dashboard::v3 {

struct Insets {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  constexpr bool operator==(const Rect& other) const {
    return x == other.x && y == other.y && width == other.width && height == other.height;
  }
};

struct DashboardV3Rects {
  Rect header{};
  Rect rain{};
  Rect traffic{};
  Rect body{};
  Rect footer{};
  Rect bodyLeft{};
  Rect bodyRight{};
};

DashboardV3Rects computeDashboardV3Layout(int width, int height, Insets safe);

// --- The 8A design -------------------------------------------------------
//
// Format-4 and newer packages use the approved 528x792 design instead of the
// legacy bands above. The two layouts are deliberately separate: the legacy
// bands are still verified rect-by-rect by the host tests and are what every
// format-2/3 package in a device cache keeps rendering with, so nothing here
// reuses or re-tunes them.
//
// The reference bands tile 0..792 exactly:
//   header  0..62   agenda  62..514  rain  514..554
//   kpi   554..694  markets 694..728 quote 728..792
// The focus row has no band of its own: the agenda's first day group draws the
// 58 px focus row for its first row, so the agenda starts right under the header
// and absorbs the 58 px the old 102..160 band held. The readability refinement
// moves the rain band (still 40 px) below the agenda, directly above the KPI
// band, where the agenda's rows cannot be crowded by it. Each band except the
// last is scaled by the usable height and the quote band absorbs the remainder,
// so an unusual canvas still tiles exactly.
constexpr int AGENDA_8A_REFERENCE_WIDTH = 528;
constexpr int AGENDA_8A_REFERENCE_HEIGHT = 792;
/// Text margin inside every 8A band. The renderer takes it from here so the
/// band boxes it draws into and the geometry the tests assert stay in step.
constexpr int DASHBOARD8A_PAD = 14;

/// Geometry of the agenda band: the padded content box the day groups stack in
/// plus the row metrics every group shares.
struct Agenda8ARects {
  /// The whole band, edge to edge. The first group's black hero band spans this
  /// width, exactly like the standalone hero it replaced.
  Rect band{};
  Rect content{};
  int headingHeight = 0;  ///< the day/date/count line and its gap
  /// The focus row the first day group draws for row 0 after its ribbon, in
  /// place of a list row. Later groups have no focus row, so it is added to the
  /// first group's height only.
  int heroHeight = 0;
  int ribbonHeight = 0;   ///< the outlined 07:00..23:00 day ribbon
  int ribbonGap = 0;      ///< the hour ticks and their numeric labels under the ribbon, plus the gap to the rows
  int rowHeight = 0;      ///< one time/title/duration row
  int summaryHeight = 0;  ///< one "+N meer" line
  int dayGap = 0;         ///< between two day groups
  /// The ribbon's window. It is a fixed day window, not a schedule range: an
  /// event outside it is clipped to the nearest edge rather than stretching the
  /// axis, so 08:00 sits in the same place on every day.
  int ribbonStartMinute = 7 * 60;
  int ribbonEndMinute = 23 * 60;
  int timeColumnWidth = 0;  ///< right-aligned clock column at the left of a row
  int titleX = 0;           ///< left edge of the title/duration column
  int durationWidth = 0;    ///< right-aligned duration column
};

struct Dashboard8ARects {
  Rect header{};
  Rect agenda{};
  /// The rain band, directly under the agenda and above the KPI band. Its 40 px
  /// no longer sits between the header and the agenda.
  Rect rain{};
  /// Always zero-height: the 8A focus row is drawn by the agenda's first day
  /// group, so this rect only marks the seam at the agenda's top for callers
  /// that still name it.
  Rect hero{};
  Rect kpi{};
  Rect kpiLeft{};
  Rect kpiRight{};
  Rect markets{};
  Rect quote{};
};

Dashboard8ARects computeDashboard8ALayout(int width, int height, Insets safe);

/// The agenda band's inner geometry. Kept as its own function because the
/// renderer and the tests both need to reason about it without the bands.
Agenda8ARects computeAgenda8ALayout(int width, int height, Insets safe);

/// Vertical size of one day group's list: its heading, ribbon and tick gap, plus
/// `rowCount` rows and `summaryCount` "+N meer" lines. The first day group draws
/// `heroHeight` as its focus row after its ribbon in place of row 0, so that one
/// group measures this plus `Agenda8ARects::heroHeight`. The renderer and the
/// layout test share this arithmetic so "does a whole day still fit?" has one
/// answer.
int agenda8ADayGroupHeight(const Agenda8ARects& agenda, int rowCount, int summaryCount = 0);

/// Maps a minute-of-day onto an x pixel inside the ribbon, clamped to the
/// ribbon's ends. Out-of-window minutes land on the nearest end rather than
/// off-canvas, which is what lets an early or late event still show as a mark.
int agendaRibbonX(Rect ribbon, int minute, int startMinute, int endMinute);

}  // namespace dashboard::v3
