// Host tests for NavScreenRenderer: a general, direct-to-framebuffer 1-bpp
// renderer that draws any supplied NavState in logical portrait coordinates
// (logical 528x792 on the physical 792x528 X3 panel).
//
// NavScreenRenderer shares NavSplash's layout contract. For the default state
// (Left maneuver, 180 m, "DUINWEG", 4,2 KM / 52 MIN, navigating) the output
// must stay byte-identical to the legacy NavSplash so the already-verified
// portrait transform and back-button behavior are preserved. Dynamic content
// tests then exercise maneuver icons, numeric/text changes, street
// truncation, and the unmistakable status screens.
//
// Layout formulas below mirror NavScreenRenderer.cpp exactly; see the
// "shared layout contract" comment blocks in both files. The tests probe
// logical coordinates through navtest's portrait mapping helpers so the
// rotation itself is exercised without threading it through every probe.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "NavScreenRenderer.h"
#include "NavSplash.h"
#include "NavState.h"
#include "NavTestFrame.h"
#include "map/RouteMapRenderer.h"
#include "map/RouteViewport.h"
#include "route/RoutePackageV1.h"

namespace {

using navigator::CurrentPosition;
using navigator::GeoPoint;
using navigator::Maneuver;
using navigator::NavFooterMetrics;
using navigator::NavMetricMode;
using navigator::NavScreenRenderer;
using navigator::NavState;
using navigator::NavStatus;
using navigator::RouteIndex;
using navigator::RouteProximity;
using navtest::countBlack;
using navtest::countLogicalBlack;
using navtest::expectGuardsUntouched;
using navtest::expectLogicalBlack;
using navtest::expectLogicalWhite;
using navtest::Frame;
using navtest::kSentinel;
using navtest::logicalPixelIsBlack;
using navtest::logicalSpan;
using navtest::rowBytes;

int clampInt(int value, int low, int high) { return value < low ? low : (value > high ? high : value); }

// Shared layout contract (mirrors NavScreenRenderer.cpp).
struct Geom {
  int LW;  // logical width  = physical height
  int LH;  // logical height = physical width

  int border;
  int maneuverH;
  int sepY;
  int sepThick;
  int mapTop;
  int largeScale;
  int midScale;
  int statusScale;
  int blockGap;
  int blockH;
  int blockY;
  int padR;
  int xRight;
  int statusY;
  int mapH;

  int iconH;
  int arrY0;
  int arrY1;
  int arrowThick;
  int boxW;
  int boxX0;
  int boxX1;
  int cx;
  int cornerY;
  int headH;
  int headHalf;

  int routeThick;
  int bendX;
  int bendY;
  int markerRadius;
  int ringThick;
  int dotY;
  int dotRadius;
  int routeBottom;
  int xLeft;

  explicit Geom(int physW, int physH)
      : LW(physH),
        LH(physW),
        border(clampInt(LW / 300 + 1, 1, 4)),
        maneuverH(LH * 38 / 100),
        sepY(border + maneuverH),
        sepThick(std::max(1, LH / 200)),
        mapTop(sepY + sepThick),
        largeScale(clampInt((LH + 99) / 100, 2, 8)),
        midScale(clampInt((LH + 149) / 150, 2, 6)),
        statusScale(clampInt((LH + 199) / 200, 1, 3)),
        blockGap(std::max(2, 7 * largeScale / 3)),
        blockH(7 * largeScale + blockGap + 7 * midScale),
        blockY(border + (maneuverH - blockH) / 2),
        padR(std::max(4, LW / 60)),
        xRight(LW - border - padR),
        statusY(LH - border - 7 * statusScale),
        mapH(statusY - mapTop),
        iconH(maneuverH * 72 / 100),
        arrY0(border + (maneuverH - iconH) / 2),
        arrY1(arrY0 + iconH),
        arrowThick(std::max(2, maneuverH / 12)),
        boxW(maneuverH * 70 / 100),
        boxX0(border + maneuverH / 10),
        boxX1(boxX0 + boxW),
        cx(boxX0 + boxW / 2),
        cornerY(arrY0 + iconH * 30 / 100),
        headH(std::max(6, arrowThick * 2)),
        headHalf(headH / 2),
        routeThick(std::max(3, LW / 66)),
        bendX(LW * 45 / 100),
        bendY(mapTop + mapH * 38 / 100),
        markerRadius(std::max(4, LH / 30)),
        ringThick(std::max(2, routeThick / 3)),
        dotY(mapTop + mapH * 62 / 100),
        dotRadius(std::max(3, LH / 48)),
        routeBottom(mapTop + mapH * 88 / 100),
        xLeft(border + std::max(4, LW / 40)) {}

  // Per-maneuver logical right edge of painted strokes (mirrors
  // NavScreenRenderer.cpp's maneuverIconRightEdge): the text-fit contract
  // keeps street names clear of painted icon strokes.
  int iconRightEdge(Maneuver maneuver) const {
    const int stemX = boxX0 + boxW * 60 / 100;
    const int poleHalf = std::max(2, arrowThick / 5);
    switch (maneuver) {
      case Maneuver::Straight:
      case Maneuver::SlightLeft:
        return cx + arrowThick / 2;
      case Maneuver::Left:
        return stemX + arrowThick / 2;
      case Maneuver::Right:
        return boxX1;
      case Maneuver::SlightRight:
        return cx + boxW * 25 / 100;
      case Maneuver::UTurn:
        return cx + boxW * 20 / 100 + headHalf;
      case Maneuver::Arrive:
        return cx + poleHalf + 1 + boxW * 30 / 100;
      case Maneuver::kCount:
        break;
    }
    return boxX1;
  }
};

NavState exampleState() {
  NavState state;  // Defaults are the legacy splash example.
  return state;
}

// ---------------------------------------------------------------------------
// Default-state equivalence with the legacy NavSplash (portrait transform and
// layout must be preserved byte-for-byte).
// ---------------------------------------------------------------------------

TEST(NavScreenRendererTest, DefaultStateMatchesLegacySplash792x528) {
  Frame splash(792, 528, kSentinel);
  NavSplash::draw(splash.pixels(), splash.width, splash.height);

  Frame dynamic(792, 528, kSentinel);
  NavScreenRenderer::draw(dynamic.pixels(), dynamic.width, dynamic.height, exampleState());

  EXPECT_EQ(dynamic.bytes.size(), splash.bytes.size());
  EXPECT_EQ(std::memcmp(dynamic.pixels(), splash.pixels(), rowBytes(792) * 528), 0) << "default state diverged";
  expectGuardsUntouched(dynamic);
  expectGuardsUntouched(splash);
}

TEST(NavScreenRendererTest, DefaultStateMatchesLegacySplash416x240) {
  Frame splash(416, 240, kSentinel);
  NavSplash::draw(splash.pixels(), splash.width, splash.height);

  Frame dynamic(416, 240, kSentinel);
  NavScreenRenderer::draw(dynamic.pixels(), dynamic.width, dynamic.height, exampleState());

  EXPECT_EQ(std::memcmp(dynamic.pixels(), splash.pixels(), rowBytes(416) * 240), 0) << "default state diverged";
  expectGuardsUntouched(dynamic);
}

// ---------------------------------------------------------------------------
// Safety: guard bytes, padding, tiny/null inputs for dynamic states.
// ---------------------------------------------------------------------------

TEST(NavScreenRendererTest, DynamicScreensKeepGuardBytesAndRowPadding) {
  // Dynamic, text-heavy screens on the real panel plus a width not divisible
  // by 8 must never touch the guard bytes or paint unused row bits.
  const int widths[] = {101, 417};
  for (const int w : widths) {
    Frame frame(w, 37, kSentinel);
    std::fill(frame.pixels(), frame.pixels() + rowBytes(w) * 37, 0x00);

    NavState state;
    state.maneuver = Maneuver::Right;
    state.nextDistanceMeters = 950;
    state.setStreet("ZUIDELIJKE RINGWEG 12345");
    state.status = NavStatus::OffRoute;
    NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);

    const size_t wb = rowBytes(w);
    const int padBits = w % 8 == 0 ? 0 : 8 - (w % 8);
    if (padBits != 0) {
      const uint8_t padMask = static_cast<uint8_t>(0xFFU >> (w % 8));
      for (int y = 0; y < frame.height; ++y) {
        const uint8_t lastByte = frame.pixels()[static_cast<size_t>(y) * wb + wb - 1U];
        EXPECT_EQ(lastByte & padMask, padMask) << "padding bits of width " << w << " row " << y << " not white";
      }
    }
    expectGuardsUntouched(frame);
  }
}

TEST(NavScreenRendererTest, DynamicScreensStaySafeOn792x528) {
  NavState state;
  state.maneuver = Maneuver::UTurn;
  state.setStreet("LANGESTRAATNAAM DIE ER ZOU KUNNEN BESTAAN OP EEN KRUISING");
  state.remainingDistanceMeters = 1234567;
  state.remainingMinutes = 1234;
  state.status = NavStatus::Recalculating;

  Frame frame(792, 528, kSentinel);
  NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
  expectGuardsUntouched(frame);
}

TEST(NavScreenRendererTest, NullZeroAndTinyInputsAreSafeForDynamicStates) {
  NavState state;
  state.status = NavStatus::Arrived;
  state.setStreet("WEESHUISSTRAAT");
  NavScreenRenderer::draw(nullptr, 792, 528, state);

  std::vector<uint8_t> tiny(64, kSentinel);
  NavScreenRenderer::draw(tiny.data(), 0, 0, state);
  NavScreenRenderer::draw(tiny.data(), 792, 0, state);
  NavScreenRenderer::draw(tiny.data(), 0, 528, state);

  struct Size {
    int w;
    int h;
  };
  const Size sizes[] = {{1, 1}, {2, 2}, {3, 5}, {5, 3}, {7, 9}, {9, 7}, {17, 9}, {15, 1}, {1, 15}};
  for (const Size s : sizes) {
    Frame frame(s.w, s.h, kSentinel);
    NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
    expectGuardsUntouched(frame);
  }
}

TEST(NavScreenRendererTest, ExtremeValuesRemainBounded) {
  // Largest fields must render clipped inside the panel with no overrun; the
  // text may crowd but must never write outside the framebuffer.
  NavState state;
  state.nextDistanceMeters = UINT16_MAX;
  state.remainingDistanceMeters = UINT32_MAX;
  state.remainingMinutes = UINT16_MAX;
  state.maneuver = Maneuver::Right;
  state.setStreet("EXTREEM LANGE STRAATNAAM VOOR TRUNCATION BOUNDS TEST 0123456789");

  Frame frame(792, 528, kSentinel);
  NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
  expectGuardsUntouched(frame);
}

// ---------------------------------------------------------------------------
// Maneuver icons.
// ---------------------------------------------------------------------------

// A rendered icon must leave a clear, non-solid stroke signature inside the
// icon box and place its distinctive features at the documented logical
// points, proving both icon variation and the portrait mapping.
TEST(NavScreenRendererTest, ManeuverIconsPaintInsideIconBox) {
  const Geom g(792, 528);
  const Maneuver all[] = {Maneuver::Straight,    Maneuver::Left,  Maneuver::Right, Maneuver::SlightLeft,
                          Maneuver::SlightRight, Maneuver::UTurn, Maneuver::Arrive};
  for (const Maneuver m : all) {
    NavState state;
    state.maneuver = m;
    Frame frame(792, 528, kSentinel);
    NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
    expectGuardsUntouched(frame);

    const int black = countLogicalBlack(frame, g.boxX0, g.arrY0, g.boxX1 + 1, g.arrY1 + 1);
    EXPECT_GE(black, 1500) << "icon for maneuver " << static_cast<int>(m) << " too faint";
    EXPECT_LE(black, 20000) << "icon for maneuver " << static_cast<int>(m) << " too heavy";
  }
}

TEST(NavScreenRendererTest, LeftIconKeepsLegacySilhouette) {
  NavState state;  // Default maneuver is Left.
  Frame frame(792, 528, kSentinel);
  NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
  const Geom g(792, 528);

  // The legacy left-turn head points at the box's left edge at cornerY.
  const int stemX = g.boxX0 + g.boxW * 60 / 100;
  expectLogicalBlack(frame, g.boxX0, g.cornerY, "left head apex");
  expectLogicalBlack(frame, stemX, g.cornerY + 40, "left shaft");
  expectLogicalWhite(frame, g.boxX1, g.cornerY, "right side stays empty");
}

TEST(NavScreenRendererTest, RightIconMirrorsLeft) {
  NavState state;
  state.maneuver = Maneuver::Right;
  Frame frame(792, 528, kSentinel);
  NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
  const Geom g(792, 528);

  expectLogicalBlack(frame, g.boxX1, g.cornerY, "right head apex");
  const int stemXr = g.boxX0 + g.boxX1 - (g.boxX0 + g.boxW * 60 / 100);
  expectLogicalBlack(frame, stemXr, g.cornerY + 40, "right shaft");
  expectLogicalWhite(frame, g.boxX0, g.cornerY, "left side stays empty");
}

TEST(NavScreenRendererTest, StraightIconHeadsUpAtCenter) {
  NavState state;
  state.maneuver = Maneuver::Straight;
  Frame frame(792, 528, kSentinel);
  NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
  const Geom g(792, 528);

  const int apexY = g.arrY0 + g.iconH * 12 / 100;
  expectLogicalBlack(frame, g.cx, apexY, "straight head apex");
  expectLogicalBlack(frame, g.cx, g.arrY0 + g.iconH * 70 / 100, "straight shaft");
  expectLogicalWhite(frame, g.boxX0, g.cornerY, "no left head");
  expectLogicalWhite(frame, g.boxX1, g.cornerY, "no right head");
}

TEST(NavScreenRendererTest, SlightTurnsLeanTowardTheirSide) {
  NavState slightLeftState;
  slightLeftState.maneuver = Maneuver::SlightLeft;
  Frame slightLeft(792, 528, kSentinel);
  NavScreenRenderer::draw(slightLeft.pixels(), slightLeft.width, slightLeft.height, slightLeftState);
  const Geom g(792, 528);

  const int leftApexX = g.cx - g.boxW * 25 / 100;
  expectLogicalBlack(slightLeft, leftApexX, g.cornerY, "slight-left head apex is mid-left");
  expectLogicalWhite(slightLeft, g.boxX0, g.cornerY, "slight-left must not reach the box edge");
  expectLogicalBlack(slightLeft, g.cx, g.cornerY + 40, "slight-left center shaft");

  NavState slightRightState;
  slightRightState.maneuver = Maneuver::SlightRight;
  Frame slightRight(792, 528, kSentinel);
  NavScreenRenderer::draw(slightRight.pixels(), slightRight.width, slightRight.height, slightRightState);

  const int rightApexX = g.cx + g.boxW * 25 / 100;
  expectLogicalBlack(slightRight, rightApexX, g.cornerY, "slight-right head apex is mid-right");
  expectLogicalWhite(slightRight, g.boxX1, g.cornerY, "slight-right must not reach the box edge");
}

TEST(NavScreenRendererTest, UTurnIconHasCenterEntryAndDownExit) {
  NavState state;
  state.maneuver = Maneuver::UTurn;
  Frame frame(792, 528, kSentinel);
  NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
  const Geom g(792, 528);

  const int exitX = g.cx + g.boxW * 20 / 100;
  const int downApexY = g.arrY0 + g.iconH * 78 / 100;
  expectLogicalBlack(frame, g.cx, g.arrY1 - 2, "u-turn entry stem");
  expectLogicalBlack(frame, g.cx + g.boxW * 10 / 100, g.arrY0 + g.iconH * 20 / 100 + g.arrowThick / 2,
                     "u-turn top bar");
  expectLogicalBlack(frame, exitX, downApexY, "u-turn downward exit head");
  expectLogicalWhite(frame, g.boxX0, g.cornerY, "no left head");
  expectLogicalWhite(frame, g.boxX1, g.cornerY, "no right head");
}

TEST(NavScreenRendererTest, ArriveIconDrawsFlagPole) {
  NavState state;
  state.maneuver = Maneuver::Arrive;
  Frame frame(792, 528, kSentinel);
  NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
  const Geom g(792, 528);

  const int flagTopY = g.arrY0 + g.iconH * 18 / 100;
  const int flagBottomY = flagTopY + g.arrowThick;
  expectLogicalBlack(frame, g.cx, g.arrY0 + g.iconH * 60 / 100, "arrive pole");
  expectLogicalBlack(frame, g.cx + g.boxW * 20 / 100, (flagTopY + flagBottomY) / 2, "arrive flag");
  expectLogicalWhite(frame, g.boxX0, g.cornerY, "no left head");
  expectLogicalWhite(frame, g.boxX1, g.cornerY, "no right head");
}

// ---------------------------------------------------------------------------
// Dynamic text output.
// ---------------------------------------------------------------------------

TEST(NavScreenRendererTest, NextDistanceTextRespondsToValue) {
  const Geom g(792, 528);
  auto drawWithNextDistance = [&](uint16_t meters) {
    NavState state;
    state.nextDistanceMeters = meters;
    Frame frame(792, 528, kSentinel);
    NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
    return frame;
  };

  // Distance line occupies logical rows [blockY, blockY + 7*largeScale); the
  // icon occupies the same rows on the left, so differences between frames
  // that share the default Left icon come from the distance text itself.
  const int ly0 = g.blockY;
  const int ly1 = ly0 + 7 * g.largeScale;
  const int meters180 = countLogicalBlack(drawWithNextDistance(180), 0, ly0, g.LW, ly1);
  const int meters35 = countLogicalBlack(drawWithNextDistance(35), 0, ly0, g.LW, ly1);
  const int meters1200 = countLogicalBlack(drawWithNextDistance(1200), 0, ly0, g.LW, ly1);

  EXPECT_GT(meters180, 0);
  EXPECT_NE(meters35, meters180) << "'35 M' must change the distance line";
  EXPECT_NE(meters1200, meters180) << "'1,2 KM' must change the distance line";
}

TEST(NavScreenRendererTest, RemainingDistanceAndMinutesTextRespondToValue) {
  const Geom g(792, 528);
  auto draw = [&](uint32_t meters, uint16_t minutes) {
    NavState state;
    state.remainingDistanceMeters = meters;
    state.remainingMinutes = minutes;
    Frame frame(792, 528, kSentinel);
    NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
    return frame;
  };

  // Bottom status line occupies logical rows [statusY, statusY + 7*statusScale].
  const int ly0 = g.statusY;
  const int ly1 = ly0 + 7 * g.statusScale;
  const int a = countLogicalBlack(draw(4200, 52), 0, ly0, g.LW, ly1);
  const int b = countLogicalBlack(draw(1500, 52), 0, ly0, g.LW, ly1);
  const int c = countLogicalBlack(draw(4200, 120), 0, ly0, g.LW, ly1);

  EXPECT_GT(a, 0);
  EXPECT_NE(b, a) << "changed remaining distance must change the status line";
  EXPECT_NE(c, a) << "changed remaining minutes must change the status line";
}

TEST(NavScreenRendererTest, LongStreetStaysBoundedAndNeverEntersIconBox) {
  const Geom g(792, 528);
  auto drawWithStreet = [&](const char* street) {
    NavState state;
    state.setStreet(street);
    Frame frame(792, 528, kSentinel);
    NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
    return frame;
  };

  const Frame shortStreet = drawWithStreet("DUINWEG");
  const Frame longStreet = drawWithStreet("ZUIDELIJKE RINGWEG DWARS DOOR DE POLDER 12");

  // The street line's logical row band (below the big distance line).
  const int ly0 = g.blockY + 7 * g.largeScale + g.blockGap;
  const int ly1 = ly0 + 7 * g.midScale;

  // The painted-icon region must be identical for both: a too-long street may
  // not bleed leftwards over the icon's strokes. (The icon box has an empty
  // right margin by design, so the contract is about painted strokes.)
  const int iconRight = g.iconRightEdge(Maneuver::Left);
  const int shortIcon = countLogicalBlack(shortStreet, g.boxX0, g.arrY0, iconRight + 1, g.arrY1 + 1);
  const int longIcon = countLogicalBlack(longStreet, g.boxX0, g.arrY0, iconRight + 1, g.arrY1 + 1);
  EXPECT_EQ(longIcon, shortIcon) << "long street leaked into the painted icon";

  // Text remains visible in the street band and stays inside the right border.
  const int textLx0 = iconRight + 2;
  EXPECT_GT(countLogicalBlack(longStreet, textLx0, ly0, g.LW - g.border, ly1), 0) << "street text missing";
  int minLx = 0;
  int maxLx = 0;
  EXPECT_TRUE(logicalSpan(longStreet, ly0, ly1, textLx0, g.LW - g.border, &minLx, &maxLx)) << "no street strokes found";
  EXPECT_GT(maxLx, g.boxX1) << "street text has nowhere to go";
  EXPECT_LT(maxLx, g.LW - g.border) << "street text ran past the right border";
  EXPECT_GE(minLx, textLx0) << "long street was not truncated clear of the icon";
}

// ---------------------------------------------------------------------------
// Status screens.
// ---------------------------------------------------------------------------

// All three non-navigating statuses render a message band and no maneuver
// icon. The message band (between the top border and the separator) lies in
// logical rows [winTop, winBot) for every message scale.
void assertStatusScreenBasics(NavStatus status, const char* label) {
  SCOPED_TRACE(label);
  NavState state;
  state.status = status;
  Frame frame(792, 528, kSentinel);
  NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
  expectGuardsUntouched(frame);

  const Geom g(792, 528);
  const int winTop = g.border + (g.maneuverH - 7 * g.largeScale) / 2;
  const int winBot = winTop + 7 * g.largeScale;

  // No maneuver icon: both icon-head apex points stay white.
  expectLogicalWhite(frame, g.boxX0, g.cornerY, "no left icon head");
  expectLogicalWhite(frame, g.boxX1, g.cornerY, "no right icon head");

  // A message is present in the band (strokes with white gaps, not a block).
  const int black = countLogicalBlack(frame, 0, winTop, g.LW, winBot);
  EXPECT_GT(black, 1200) << "status message missing in band for " << label;
  const int total = g.LW * (winBot - winTop);
  EXPECT_LT(black, total) << "message band must not be a solid block for " << label;

  // The status line at the bottom still shows remaining distance/time.
  const int statusBlack = countLogicalBlack(frame, 0, g.statusY, g.LW, g.statusY + 7 * g.statusScale);
  EXPECT_GT(statusBlack, 0) << "bottom status line missing for " << label;
}

TEST(NavScreenRendererTest, OffRouteScreenIsUnmistakable) {
  NavState state;
  state.status = NavStatus::OffRoute;
  Frame frame(792, 528, kSentinel);
  NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
  const Geom g(792, 528);

  assertStatusScreenBasics(NavStatus::OffRoute, "off-route");

  // Off route: the old route is dropped, only the current-position dot stays.
  expectLogicalBlack(frame, g.bendX, g.dotY, "off-route keeps position dot");
  expectLogicalWhite(frame, g.bendX, g.bendY - g.markerRadius, "no next-maneuver ring when off route");
  expectLogicalWhite(frame, g.bendX, g.bendY + g.markerRadius + 20, "no route bar when off route");
}

TEST(NavScreenRendererTest, RecalculatingScreenIsUnmistakable) {
  NavState state;
  state.status = NavStatus::Recalculating;
  Frame frame(792, 528, kSentinel);
  NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
  const Geom g(792, 528);

  assertStatusScreenBasics(NavStatus::Recalculating, "recalculating");

  // Recalculating: the old route remains but no next-maneuver marker is shown.
  expectLogicalBlack(frame, g.bendX, g.bendY + g.markerRadius + 20, "recalculating keeps the route");
  expectLogicalBlack(frame, g.bendX, g.dotY, "recalculating keeps position dot");
  expectLogicalWhite(frame, g.bendX, g.bendY - g.markerRadius, "no next-maneuver ring while recalculating");
}

TEST(NavScreenRendererTest, ArrivedScreenIsUnmistakable) {
  NavState state;
  state.status = NavStatus::Arrived;
  state.remainingDistanceMeters = 0;
  state.remainingMinutes = 0;
  Frame frame(792, 528, kSentinel);
  NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
  const Geom g(792, 528);

  assertStatusScreenBasics(NavStatus::Arrived, "arrived");

  // Arrived: a hollow destination ring marks the position dot; no route.
  expectLogicalBlack(frame, g.bendX, g.dotY - g.markerRadius, "destination ring top");
  expectLogicalBlack(frame, g.bendX, g.dotY + g.markerRadius, "destination ring bottom");
  expectLogicalWhite(frame, g.bendX, g.dotY, "destination ring interior stays white");
  expectLogicalWhite(frame, g.bendX, g.bendY - g.markerRadius, "no next-maneuver ring when arrived");
  expectLogicalWhite(frame, g.bendX, g.bendY + g.markerRadius + 20, "no route bar when arrived");
}

TEST(NavScreenRendererTest, StatusScreensHaveDistinctMessages) {
  NavState offRouteState;
  offRouteState.status = NavStatus::OffRoute;
  NavState recalcState;
  recalcState.status = NavStatus::Recalculating;
  NavState arrivedState;
  arrivedState.status = NavStatus::Arrived;

  const Geom g(792, 528);
  const int winTop = g.border + (g.maneuverH - 7 * g.largeScale) / 2;
  const int winBot = winTop + 7 * g.largeScale;

  auto messageSpan = [&](const NavState& state, int* lo, int* hi) {
    Frame frame(792, 528, kSentinel);
    NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
    EXPECT_TRUE(logicalSpan(frame, winTop, winBot, g.border, g.LW - g.border, lo, hi)) << "no message strokes";
  };

  int offLo = 0, offHi = 0;
  int recLo = 0, recHi = 0;
  int arrLo = 0, arrHi = 0;
  messageSpan(offRouteState, &offLo, &offHi);
  messageSpan(recalcState, &recLo, &recHi);
  messageSpan(arrivedState, &arrLo, &arrHi);

  // "ROUTE VERLATEN" is the widest message, then "AANGEKOMEN", then
  // "BEREKENEN"; each screen must read differently.
  EXPECT_GT(offHi - offLo, arrHi - arrLo + 6) << "off-route must be the widest status message";
  EXPECT_GT(arrHi - arrLo, recHi - recLo + 6) << "arrived must be wider than recalculating";
}

TEST(NavScreenRendererTest, StatusScreensRenderOnSmallPanel) {
  const Geom g(416, 240);
  const int winTop = g.border + (g.maneuverH - 7 * g.largeScale) / 2;
  const int winBot = winTop + 7 * g.largeScale;

  NavState states[3];
  states[0].status = NavStatus::OffRoute;
  states[1].status = NavStatus::Recalculating;
  states[2].status = NavStatus::Arrived;
  for (const NavState& state : states) {
    Frame frame(416, 240, kSentinel);
    NavScreenRenderer::draw(frame.pixels(), frame.width, frame.height, state);
    expectGuardsUntouched(frame);
    EXPECT_GT(countLogicalBlack(frame, 0, winTop, g.LW, winBot), 300) << "status message missing on small panel";
  }
}

// ---------------------------------------------------------------------------
// Gray footer metric policy (whole-route totals vs. distance/time still to go)
// ---------------------------------------------------------------------------

TEST(NavScreenRendererTest, FooterKeepsTotalsWithoutFixOrDeclaredTotal) {
  RouteIndex route;
  route.totalDistanceMeters = 4000;
  route.estimatedMinutes = 100;
  const CurrentPosition position{GeoPoint{520'000'000, 40'000'000}, 5, 0};
  RouteProximity near;
  near.valid = true;
  near.distanceMeters = 10;  // within max(40 m, 2 * 5 m accuracy)
  near.remainingDistanceMeters = 2000;

  // No fix at all: only the whole-route totals are meaningful.
  const NavFooterMetrics noFix = NavScreenRenderer::chooseFooterMetrics(nullptr, near, route);
  EXPECT_EQ(noFix.mode, NavMetricMode::Total);
  EXPECT_EQ(noFix.distanceMeters, 4000U);
  EXPECT_EQ(noFix.minutes, 100U);

  // A route that declares no total cannot scale a remaining estimate.
  RouteIndex undeclared;
  undeclared.totalDistanceMeters = 0;
  undeclared.estimatedMinutes = 45;
  const NavFooterMetrics zeroTotal = NavScreenRenderer::chooseFooterMetrics(&position, near, undeclared);
  EXPECT_EQ(zeroTotal.mode, NavMetricMode::Total);
  EXPECT_EQ(zeroTotal.distanceMeters, 0U);
  EXPECT_EQ(zeroTotal.minutes, 45U);
}

TEST(NavScreenRendererTest, FooterSwapsToRemainingForCloseValidFix) {
  RouteIndex route;
  route.totalDistanceMeters = 4000;
  route.estimatedMinutes = 100;
  const CurrentPosition position{GeoPoint{520'000'000, 40'000'000}, 5, 0};

  RouteProximity near;
  near.valid = true;
  near.distanceMeters = 10;  // within max(40 m, 2 * 5 m accuracy)
  near.remainingDistanceMeters = 1500;
  const NavFooterMetrics metrics = NavScreenRenderer::chooseFooterMetrics(&position, near, route);
  EXPECT_EQ(metrics.mode, NavMetricMode::Remaining);
  EXPECT_EQ(metrics.distanceMeters, 1500U);
  // 1500 / 4000 * 100 minutes, rounded half-up.
  EXPECT_EQ(metrics.minutes, 38U);

  // A fix exactly on the off-route guard boundary is still close enough.
  RouteProximity boundary;
  boundary.valid = true;
  boundary.distanceMeters = 40;  // == max(40, 2 * 5)
  boundary.remainingDistanceMeters = 500;
  const NavFooterMetrics atBoundary = NavScreenRenderer::chooseFooterMetrics(&position, boundary, route);
  EXPECT_EQ(atBoundary.mode, NavMetricMode::Remaining);
  EXPECT_EQ(atBoundary.minutes, 13U);  // 500 / 4000 * 100, half-up

  // The remaining meters are clamped to the declared total before scaling.
  RouteProximity oversized;
  oversized.valid = true;
  oversized.distanceMeters = 10;
  oversized.remainingDistanceMeters = 999'999;
  const NavFooterMetrics clamped = NavScreenRenderer::chooseFooterMetrics(&position, oversized, route);
  EXPECT_EQ(clamped.mode, NavMetricMode::Remaining);
  EXPECT_EQ(clamped.distanceMeters, 4000U);
  EXPECT_EQ(clamped.minutes, 100U);

  // Walked to the end: nothing left, zero minutes.
  RouteProximity arrived;
  arrived.valid = true;
  arrived.distanceMeters = 10;
  arrived.remainingDistanceMeters = 0;
  const NavFooterMetrics done = NavScreenRenderer::chooseFooterMetrics(&position, arrived, route);
  EXPECT_EQ(done.mode, NavMetricMode::Remaining);
  EXPECT_EQ(done.distanceMeters, 0U);
  EXPECT_EQ(done.minutes, 0U);
}

TEST(NavScreenRendererTest, FooterFallsBackToTotalsWhenFixIsOffRoute) {
  RouteIndex route;
  route.totalDistanceMeters = 4000;
  route.estimatedMinutes = 100;
  const CurrentPosition position{GeoPoint{520'000'000, 40'000'000}, 5, 0};

  // Straight-line distance beyond max(40 m, 2x accuracy): the projection
  // along the route no longer describes where the walk is, so the footer
  // keeps the established whole-route totals.
  RouteProximity off;
  off.valid = true;
  off.distanceMeters = 500;
  off.remainingDistanceMeters = 100;  // would be shown if the guard failed
  const NavFooterMetrics metrics = NavScreenRenderer::chooseFooterMetrics(&position, off, route);
  EXPECT_EQ(metrics.mode, NavMetricMode::Total);
  EXPECT_EQ(metrics.distanceMeters, 4000U);
  EXPECT_EQ(metrics.minutes, 100U);

  // An invalid proximity (e.g. no closest route edge was found) keeps totals.
  RouteProximity invalid;
  invalid.valid = false;
  invalid.distanceMeters = 10;
  invalid.remainingDistanceMeters = 1500;
  const NavFooterMetrics noProximity = NavScreenRenderer::chooseFooterMetrics(&position, invalid, route);
  EXPECT_EQ(noProximity.mode, NavMetricMode::Total);
  EXPECT_EQ(noProximity.distanceMeters, 4000U);
  EXPECT_EQ(noProximity.minutes, 100U);
}

// ---------------------------------------------------------------------------
// Gray footer arrival policy (hasReachedRouteEnd)
// ---------------------------------------------------------------------------

TEST(NavScreenRendererTest, ArrivalDeclaredOnlyForOnRouteFixAtTheEnd) {
  RouteIndex route;
  route.totalDistanceMeters = 4200;
  route.estimatedMinutes = 60;

  // On-route fix with good accuracy, remaining exactly on the arrival band
  // edge (max(10 m, 1x accuracy) = 10 m for a 10 m fix): arrived.
  const CurrentPosition position{GeoPoint{520'000'000, 40'000'000}, 10, 0};
  RouteProximity atEnd;
  atEnd.valid = true;
  atEnd.distanceMeters = 8;  // within max(40 m, 2x accuracy)
  atEnd.remainingDistanceMeters = 10;
  EXPECT_TRUE(NavScreenRenderer::hasReachedRouteEnd(&position, atEnd, route));

  // Standing exactly on the end with a tight 5 m fix also arrives (floor
  // band = 10 m).
  const CurrentPosition tight{GeoPoint{520'000'000, 40'000'000}, 5, 0};
  RouteProximity exact;
  exact.valid = true;
  exact.distanceMeters = 0;
  exact.remainingDistanceMeters = 0;
  EXPECT_TRUE(NavScreenRenderer::hasReachedRouteEnd(&tight, exact, route));
}

TEST(NavScreenRendererTest, ArrivalRejectsFixJustOutsideTheBand) {
  RouteIndex route;
  route.totalDistanceMeters = 4200;
  route.estimatedMinutes = 60;
  // Arrival band is max(10 m, 1x accuracy) = 10 m for a 10 m fix; one meter
  // past it the walk is still approaching and must not read as arrived.
  const CurrentPosition position{GeoPoint{520'000'000, 40'000'000}, 10, 0};
  RouteProximity shortOfEnd;
  shortOfEnd.valid = true;
  shortOfEnd.distanceMeters = 5;
  shortOfEnd.remainingDistanceMeters = 11;
  EXPECT_FALSE(NavScreenRenderer::hasReachedRouteEnd(&position, shortOfEnd, route));

  // A fix at the on-route guard boundary but still tens of meters short of
  // the end stays an approach as well.
  RouteProximity approaching;
  approaching.valid = true;
  approaching.distanceMeters = 40;  // == max(40 m, 2x 10 m accuracy)
  approaching.remainingDistanceMeters = 35;
  EXPECT_FALSE(NavScreenRenderer::hasReachedRouteEnd(&position, approaching, route));
}

TEST(NavScreenRendererTest, ArrivalRejectsPoorAccuracyOffRouteAndNoFix) {
  RouteIndex route;
  route.totalDistanceMeters = 4200;
  route.estimatedMinutes = 60;
  RouteProximity nearEnd;
  nearEnd.valid = true;
  nearEnd.distanceMeters = 5;
  nearEnd.remainingDistanceMeters = 4;

  // Poor accuracy: a 60 m fix cannot place the walk at the end even when the
  // reported remaining is tiny, because the true position may still be one
  // coarse step before it.
  const CurrentPosition coarse{GeoPoint{520'000'000, 40'000'000}, 60, 0};
  EXPECT_FALSE(NavScreenRenderer::hasReachedRouteEnd(&coarse, nearEnd, route));

  // Off-route: the fix is far enough from the route that its along-route
  // projection is untrusted, so a zero remaining must not trigger arrival.
  const CurrentPosition position{GeoPoint{520'000'000, 40'000'000}, 5, 0};
  RouteProximity offRoute;
  offRoute.valid = true;
  offRoute.distanceMeters = 500;
  offRoute.remainingDistanceMeters = 0;
  EXPECT_FALSE(NavScreenRenderer::hasReachedRouteEnd(&position, offRoute, route));

  // An invalid proximity (e.g. no closest route edge was found) keeps the
  // non-arrived status.
  RouteProximity invalid;
  invalid.valid = false;
  invalid.distanceMeters = 5;
  invalid.remainingDistanceMeters = 0;
  EXPECT_FALSE(NavScreenRenderer::hasReachedRouteEnd(&position, invalid, route));

  // No live fix at all.
  EXPECT_FALSE(NavScreenRenderer::hasReachedRouteEnd(nullptr, nearEnd, route));

  // A route that declares no total cannot support the along-route remaining.
  RouteIndex undeclared;
  undeclared.totalDistanceMeters = 0;
  undeclared.estimatedMinutes = 45;
  EXPECT_FALSE(NavScreenRenderer::hasReachedRouteEnd(&position, nearEnd, undeclared));
}

TEST(NavScreenRendererTest, ArrivalIgnoresLoopPassingNearItsOwnEndpoint) {
  RouteIndex route;
  route.totalDistanceMeters = 4200;
  route.estimatedMinutes = 60;
  // The route loops back next to its own endpoint early on: the fix sits
  // geographically on the line a few meters from that endpoint, but the
  // along-route distance still to walk is the whole closing loop.
  const CurrentPosition position{GeoPoint{520'000'000, 40'000'000}, 5, 0};
  RouteProximity loop;
  loop.valid = true;
  loop.distanceMeters = 3;
  loop.remainingDistanceMeters = 3900;
  EXPECT_FALSE(NavScreenRenderer::hasReachedRouteEnd(&position, loop, route));
}

TEST(NavScreenRendererTest, RemainingMinutesAreProportionalWithZeroAndOverflowBounds) {
  EXPECT_EQ(NavScreenRenderer::remainingMinutes(60, 0, 1000), 0U);
  EXPECT_EQ(NavScreenRenderer::remainingMinutes(60, 500, 1000), 30U);
  EXPECT_EQ(NavScreenRenderer::remainingMinutes(60, 999, 1000), 60U);  // half-up rounding
  EXPECT_EQ(NavScreenRenderer::remainingMinutes(60, 1000, 1000), 60U);
  EXPECT_EQ(NavScreenRenderer::remainingMinutes(60, 1, 1000), 0U);  // sub-minute stays 0
  EXPECT_EQ(NavScreenRenderer::remainingMinutes(0, 500, 1000), 0U);  // no declared estimate
  EXPECT_EQ(NavScreenRenderer::remainingMinutes(60, 1000, 0), 0U);  // degenerate total
  EXPECT_EQ(NavScreenRenderer::remainingMinutes(60, 2000, 1000), 60U);  // clamped to the estimate

  // 32-bit extremes: the product remainingMeters * estimatedMinutes must be
  // computed in 64 bits and the result clamped to the uint16 estimate.
  EXPECT_EQ(NavScreenRenderer::remainingMinutes(65535, 0xFFFFFFFFu, 0xFFFFFFFFu), 65535U);
  EXPECT_EQ(NavScreenRenderer::remainingMinutes(65535, 0, 0xFFFFFFFFu), 0U);
  EXPECT_EQ(NavScreenRenderer::remainingMinutes(30'000, 0xFFFFFFFFu, 0xFFFFFFFFu), 30'000U);
}

}  // namespace
