// Host tests for the phase-0 navigator splash (NavSplash).
//
// NavSplash draws a static route-only navigation screen straight into the
// FreeInk row-major 1bpp MSB-first framebuffer (bit 7 of each byte is the
// leftmost pixel; 1 = white, 0 = black). The panel is physically landscape and
// the buffer passed to draw() is widthPx x heightPx (X3: 792x528; host tests
// also use 416x240). NavSplash authors the screen in logical PORTRAIT
// coordinates of heightPx x widthPx and stores every logical pixel (lx, ly)
// at the physical pixel (ly, heightPx - 1 - lx) -- the repository GfxRenderer
// Portrait transform (90 degrees clockwise). All pixel probes in this file
// therefore use the physX()/physY() helpers or precomputed rotated regions.
//
// Layout contract (must match NavSplash.cpp), in logical portrait space with
// logical width LW = heightPx and logical height LH = widthPx:
//   border  t   = clamp(LW/300 + 1, 1, 4)
//   mH          = LH*38/100                      maneuver band height (~38%)
//   sepY        = t + mH, separator thickness max(1, LH/200)
//   fonts scale with LH; large/mid lines right-aligned at LW - t - max(4, LW/60)
//   route: bend at (LW*45/100, mapTop + mapH*38/100), marker ring on the bend,
//   filled current-position dot below the bend, route leaves the ring left.
//   bottom status line centered at y = LH - t - 7*statusScale.
//
// Tests allocate a real framebuffer with guard bytes, run NavSplash::draw(),
// and probe pixels. Guard-byte / padding / overrun tests are dimension
// independent. Content tests prove the portrait mapping with feature anchors
// and region-level text evidence instead of incidental pixels.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "NavSplash.h"

namespace {

constexpr int kGuard = 16;
constexpr uint8_t kSentinel = 0xA5;

constexpr size_t rowBytes(int widthPx) { return (static_cast<size_t>(widthPx) + 7U) / 8U; }

struct Frame {
  int width;
  int height;
  std::vector<uint8_t> bytes;  // [guard][framebuffer][guard]

  Frame(int w, int h, uint8_t fill) : width(w), height(h) {
    bytes.assign(static_cast<size_t>(kGuard) + rowBytes(w) * static_cast<size_t>(h) + static_cast<size_t>(kGuard),
                 fill);
  }

  uint8_t* pixels() { return bytes.data() + kGuard; }
  const uint8_t* pixels() const { return bytes.data() + kGuard; }
};

bool pixelIsBlack(const Frame& frame, int x, int y) {
  if (x < 0 || y < 0 || x >= frame.width || y >= frame.height) {
    return false;
  }
  const uint8_t* p = frame.pixels();
  const size_t wb = rowBytes(frame.width);
  const uint8_t byte = p[static_cast<size_t>(y) * wb + static_cast<size_t>(x) / 8U];
  return ((byte >> (7 - (x & 7))) & 1U) == 0U;
}

int countBlack(const Frame& frame, int x0, int y0, int x1, int y1) {
  int black = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      black += pixelIsBlack(frame, x, y) ? 1 : 0;
    }
  }
  return black;
}

void expectBlack(const Frame& frame, int x, int y, const char* label) {
  SCOPED_TRACE(label);
  EXPECT_TRUE(pixelIsBlack(frame, x, y)) << "expected black at (" << x << "," << y << ")";
}

void expectWhite(const Frame& frame, int x, int y, const char* label) {
  SCOPED_TRACE(label);
  EXPECT_FALSE(pixelIsBlack(frame, x, y)) << "expected white at (" << x << "," << y << ")";
}

void expectGuardsUntouched(const Frame& frame) {
  for (int i = 0; i < kGuard; ++i) {
    EXPECT_EQ(frame.bytes[static_cast<size_t>(i)], kSentinel) << "leading guard " << i;
    EXPECT_EQ(frame.bytes[frame.bytes.size() - 1U - static_cast<size_t>(i)], kSentinel) << "trailing guard " << i;
  }
}

// A "text evidence" box must contain a meaningful amount of black (glyph
// strokes) and white (background between strokes) and must not be a solid
// block or empty. Thresholds are roughly half the measured stroke coverage,
// so they stay robust to sub-pixel rounding differences on glyph edges.
void expectTextEvidence(const Frame& frame, int x0, int y0, int x1, int y1, int minBlack, int maxBlack,
                        const char* label) {
  SCOPED_TRACE(label);
  const int total = (x1 - x0) * (y1 - y0);
  const int black = countBlack(frame, x0, y0, x1, y1);
  EXPECT_GE(black, minBlack) << "not enough black strokes in " << label;
  EXPECT_LE(black, maxBlack) << "region in " << label << " is too solid";
  EXPECT_LT(black, total) << "region in " << label << " should not be fully black";
  EXPECT_GT(total - black, 0) << "region in " << label << " has no white evidence";
}

// Portrait mapping helpers (must match NavSplash.cpp / GfxRenderer::Portrait):
// logical (lx, ly) -> physical (ly, physH - 1 - lx).
int physX(int ly) { return ly; }
int physY(int lx, int physH) { return physH - 1 - lx; }

// ---------------------------------------------------------------------------
// Guard bytes / overrun safety
// ---------------------------------------------------------------------------

TEST(NavSplashGuardBytes, UnchangedFor792x528) {
  Frame frame(792, 528, kSentinel);
  NavSplash::draw(frame.pixels(), frame.width, frame.height);
  expectGuardsUntouched(frame);
}

TEST(NavSplashGuardBytes, UnchangedFor416x240) {
  Frame frame(416, 240, kSentinel);
  NavSplash::draw(frame.pixels(), frame.width, frame.height);
  expectGuardsUntouched(frame);
}

TEST(NavSplashGuardBytes, UnchangedFor17x9) {
  Frame frame(17, 9, kSentinel);
  NavSplash::draw(frame.pixels(), frame.width, frame.height);
  expectGuardsUntouched(frame);
}

TEST(NavSplashGuardBytes, UnchangedForWidthNotDivisibleByEight) {
  Frame frame(101, 37, kSentinel);
  NavSplash::draw(frame.pixels(), frame.width, frame.height);
  expectGuardsUntouched(frame);
}

TEST(NavSplashOverrun, NullZeroAndTinyInputsAreSafe) {
  NavSplash::draw(nullptr, 792, 528);

  std::vector<uint8_t> tiny(64, kSentinel);
  NavSplash::draw(tiny.data(), 0, 0);
  NavSplash::draw(tiny.data(), 792, 0);
  NavSplash::draw(tiny.data(), 0, 528);

  struct Size {
    int w;
    int h;
  };
  const Size sizes[] = {{1, 1}, {2, 2}, {3, 5}, {5, 3}, {7, 9}, {9, 7}, {17, 9}, {15, 1}, {1, 15}};
  for (const Size s : sizes) {
    Frame frame(s.w, s.h, kSentinel);
    NavSplash::draw(frame.pixels(), frame.width, frame.height);
    expectGuardsUntouched(frame);
  }
}

TEST(NavSplashPadding, UnusedBitsInFinalByteStayWhite) {
  const int widths[] = {13, 17, 53, 101};
  for (const int w : widths) {
    Frame frame(w, 21, kSentinel);
    // Start the whole framebuffer black; a correct draw() must clear every
    // row including the trailing padding byte, then never paint padding bits.
    std::fill(frame.pixels(), frame.pixels() + rowBytes(w) * 21, 0x00);
    NavSplash::draw(frame.pixels(), frame.width, frame.height);

    const size_t wb = rowBytes(w);
    const int padBits = w % 8 == 0 ? 0 : 8 - (w % 8);
    if (padBits == 0) {
      continue;
    }
    const uint8_t padMask = static_cast<uint8_t>(0xFFU >> (w % 8));
    for (int y = 0; y < frame.height; ++y) {
      const uint8_t lastByte = frame.pixels()[static_cast<size_t>(y) * wb + wb - 1U];
      EXPECT_EQ(lastByte & padMask, padMask) << "padding bits of width " << w << " row " << y << " not white";
    }
    expectGuardsUntouched(frame);
  }
}

// ---------------------------------------------------------------------------
// 792 x 528 physical panel -> 528 x 792 logical portrait
// ---------------------------------------------------------------------------
//
// Physical layout summary (see probe/geometry notes in NavSplash.cpp):
//   border t = 2  -> physical cols 0..1 & 790..791, rows 0..1 & 526..527.
//   separator     -> logical row 302..304 -> physical vertical bar at
//                    columns 302..304 running rows 2..525.
//   maneuver text -> logical rows 94..209 -> physical left strip
//                    ("180 M": cols 94..149, "DUINWEG": cols 168..209).
//   arrow shaft   -> logical (146..171, 108..260) -> physical cols 108..259,
//                    rows 357..381; head apex logical (32,108) -> (108,495).
//   route bend    -> logical (237,481) -> physical (481,290), marker ring r26;
//                    dot logical (237,592) -> physical (592,290), r16.
//   status line   -> logical rows 769..790 -> physical right strip
//                    cols 769..789, rows 158..370.

// TDD red step kept as a regression guard: regions the native-landscape
// splash painted (right-aligned maneuver text and the centered status line)
// must be empty once the screen is authored in portrait and rotated onto the
// panel. The route bar now legitimately crosses the old status band at
// columns 477..484, so the old status check covers the left part of that band.
TEST(NavSplashPortrait792x528, OldLandscapeHotspotsAreEmpty) {
  Frame frame(792, 528, kSentinel);
  NavSplash::draw(frame.pixels(), frame.width, frame.height);

  EXPECT_EQ(countBlack(frame, 620, 61, 776, 145), 0) << "old right-aligned maneuver text spot not empty";
  EXPECT_EQ(countBlack(frame, 306, 505, 470, 524), 0) << "old centered status spot not empty";
}

TEST(NavSplashPortrait792x528, WhiteBackgroundAndThinOuterBorder) {
  Frame frame(792, 528, kSentinel);
  NavSplash::draw(frame.pixels(), frame.width, frame.height);

  // White background at representative points away from every drawn element.
  expectWhite(frame, 400, 240, "bg map upper-left");
  expectWhite(frame, 700, 260, "bg map right");
  expectWhite(frame, 650, 470, "bg map lower-right");
  expectWhite(frame, 100, 480, "bg icon hollow / map");
  expectWhite(frame, 700, 150, "bg map upper area");

  // Thin black outer border (t = 2 on this panel) with white just inside.
  expectBlack(frame, 0, 100, "left border");
  expectBlack(frame, 791, 100, "right border");
  expectBlack(frame, 100, 0, "top border");
  expectBlack(frame, 100, 527, "bottom border");
  expectWhite(frame, 2, 100, "inside left border");
  expectWhite(frame, 100, 2, "inside top border");
  expectWhite(frame, 789, 100, "inside right border");
  expectWhite(frame, 100, 525, "inside bottom border");
}

TEST(NavSplashPortrait792x528, SeparatorRunsVerticallyAtTheLogicalTopEdge) {
  Frame frame(792, 528, kSentinel);
  NavSplash::draw(frame.pixels(), frame.width, frame.height);

  // The logical separator is a horizontal line under the ~38% maneuver band
  // (logical row 302). Under the Portrait mapping logical rows become physical
  // columns, so it renders as a full-height vertical bar at columns 302..304.
  expectBlack(frame, 302, 100, "separator col 302");
  expectBlack(frame, 303, 300, "separator col 303");
  expectBlack(frame, 304, 500, "separator col 304");
  expectWhite(frame, 301, 300, "left of separator");
  expectWhite(frame, 305, 300, "right of separator");
}

TEST(NavSplashPortrait792x528, ManeuverTextOccupiesTheLeftStrip) {
  Frame frame(792, 528, kSentinel);
  NavSplash::draw(frame.pixels(), frame.width, frame.height);

  // "180 M" (large, scale 8) and "DUINWEG" (mid, scale 6) are authored in the
  // logical top band and therefore land in the physical LEFT strip, before the
  // separator at column 302. Their physical rows (10..243) span the logical
  // text width, so the two lines are separated in the rotated buffer by their
  // physical columns (94..149 vs 168..209).
  expectTextEvidence(frame, 94, 10, 150, 218, 2000, 8000, "180 M bbox");
  expectTextEvidence(frame, 168, 10, 210, 244, 2000, 7500, "DUINWEG bbox");
  EXPECT_EQ(countBlack(frame, 150, 9, 168, 244), 0) << "inter-line gap not white";
  // Nothing from the top band may leak into the physical right strip.
  EXPECT_EQ(countBlack(frame, 306, 61, 776, 145), 0) << "right strip not empty";
}

TEST(NavSplashPortrait792x528, BoldTurnArrowSitsInTheLeftStrip) {
  Frame frame(792, 528, kSentinel);
  NavSplash::draw(frame.pixels(), frame.width, frame.height);

  // Logical shaft rect (146..171, 108..260) maps to a physical shaft at
  // columns 108..259, rows 357..381 (the turn arrow reads below the text in
  // the left strip).
  expectBlack(frame, 183, 369, "shaft center");
  expectBlack(frame, 108, 369, "shaft left edge");
  expectWhite(frame, 183, 356, "white above shaft");

  // Arrowhead apex logical (32,108) -> physical (108,495); the head is a
  // triangle whose widest (base) row is at physical row 381 (logical x 146).
  expectBlack(frame, 108, 495, "arrowhead apex");
  expectBlack(frame, 108, 450, "arrowhead lower body");
  expectBlack(frame, 108, 390, "arrowhead upper body");
  expectWhite(frame, 108, 350, "white above arrowhead");
  expectWhite(frame, 60, 400, "white left of arrowhead");
  expectWhite(frame, 260, 400, "white right of arrowhead");
}

TEST(NavSplashPortrait792x528, RouteBendMarkerAndPositionDot) {
  Frame frame(792, 528, kSentinel);
  NavSplash::draw(frame.pixels(), frame.width, frame.height);

  // Ring marker: logical center (237,481) -> physical (481,290), radius 26;
  // the inner disc (radius 24) is white so the ring reads as an outline.
  expectBlack(frame, 481, 264, "ring top");
  expectBlack(frame, 455, 290, "ring left");
  expectBlack(frame, 507, 290, "ring right");
  expectBlack(frame, 481, 316, "ring bottom");
  expectWhite(frame, 481, 290, "ring interior");
  expectWhite(frame, 481, 250, "white above ring");
  expectWhite(frame, 440, 290, "white left of ring");

  // Route, left entry: the logical horizontal segment (15..211, ~481) becomes
  // a physical vertical bar at columns 477..484, rows 317..512.
  expectBlack(frame, 481, 400, "route vertical bar");
  expectBlack(frame, 481, 512, "route vertical bar bottom");
  expectWhite(frame, 481, 513, "white below route bar");

  // Route, downward exit: the logical vertical segment becomes a physical
  // horizontal bar at rows 287..294, columns 507..712.
  expectBlack(frame, 650, 290, "route horizontal bar");
  expectBlack(frame, 700, 290, "route horizontal bar far");
  expectWhite(frame, 714, 290, "white right of route bar");

  // Filled current-position dot: logical (237,592) -> physical (592,290),
  // radius 16, drawn on top of the route bar.
  expectBlack(frame, 592, 290, "dot center");
  expectBlack(frame, 592, 274, "dot top");
  expectBlack(frame, 592, 306, "dot bottom");
  expectBlack(frame, 576, 290, "dot left");
  expectBlack(frame, 608, 290, "dot right");
  expectWhite(frame, 560, 280, "white left of dot (off route row)");
  expectWhite(frame, 530, 270, "white right of ring (off route row)");
}

TEST(NavSplashPortrait792x528, BottomStatusLineOccupiesTheRightStrip) {
  Frame frame(792, 528, kSentinel);
  NavSplash::draw(frame.pixels(), frame.width, frame.height);

  // Logical status rows 769..790 (bottom of the portrait screen) map to the
  // physical RIGHT strip: columns 769..789, rows 158..370.
  expectTextEvidence(frame, 769, 158, 790, 371, 600, 3500, "status bbox");
  expectWhite(frame, 760, 300, "white left of status");
  expectWhite(frame, 775, 100, "white above status");
  expectWhite(frame, 775, 390, "white below status");
  EXPECT_EQ(countBlack(frame, 306, 505, 470, 524), 0) << "old landscape status spot not empty";
}

TEST(NavSplashPortrait792x528, LogicalCornersMapToTheFourPanelCorners) {
  Frame frame(792, 528, kSentinel);
  NavSplash::draw(frame.pixels(), frame.width, frame.height);

  // Every logical corner is inside the thin border, so all four panel corners
  // are black...
  expectBlack(frame, 0, 0, "corner top-left");
  expectBlack(frame, 791, 0, "corner top-right");
  expectBlack(frame, 0, 527, "corner bottom-left");
  expectBlack(frame, 791, 527, "corner bottom-right");

  // ...and the border is a full frame: top/bottom rows 0..1 and 526..527 are
  // black across the whole width, proving the logical left/right edges (which
  // map to physical bottom/top) reach from one side of the panel to the other.
  EXPECT_EQ(countBlack(frame, 2, 0, 790, 2), 2 * (790 - 2)) << "top border rows not full width";
  EXPECT_EQ(countBlack(frame, 2, 526, 790, 528), 2 * (790 - 2)) << "bottom border rows not full width";
}

TEST(NavSplashPortraitMapping, FeatureAnchorsFollowTheDocumentedFormula) {
  // logical (lx, ly) -> physical (ly, physH - 1 - lx). Re-deriving the
  // feature anchors from the formula keeps the probe coordinates honest:
  // separator logical row 302 -> physical column 302; ring center logical
  // (237,481) -> (481,290); arrow apex logical (32,108) -> (108,495); dot
  // logical (237,592) -> (592,290).
  EXPECT_EQ(physX(302), 302);
  EXPECT_EQ(physX(592), 592);
  EXPECT_EQ(physY(237, 528), 290);
  EXPECT_EQ(physY(32, 528), 495);
  EXPECT_EQ(physY(108, 240), 131);
}

// ---------------------------------------------------------------------------
// 416 x 240 physical panel -> 240 x 416 logical portrait
// ---------------------------------------------------------------------------
//
// Physical layout summary for this size (logical LW = 240, LH = 416):
//   border t = 1   -> physical col 0 & 415, row 0 & 239.
//   separator      -> logical row 159 -> physical vertical bar columns
//                     159..160, rows 1..238.
//   maneuver text  -> "180 M" cols 46..80 rows 5..134; "DUINWEG" cols 92..112
//                     rows 5..121; arrow apex logical (16,56) -> (56,223).
//   route          -> ring center logical (108,249) -> physical (249,131) r13;
//                     dot logical (108,305) -> physical (305,131) r8.
//   status line    -> logical rows 394..414 -> physical right strip
//                     columns 394..414, rows 14..226.

TEST(NavSplashPortrait416x240, BackgroundBorderAndSeparator) {
  Frame frame(416, 240, kSentinel);
  NavSplash::draw(frame.pixels(), frame.width, frame.height);

  // Background white points in the map area.
  expectWhite(frame, 200, 120, "bg map mid");
  expectWhite(frame, 350, 200, "bg map lower-right");

  // Single-pixel border frame.
  expectBlack(frame, 0, 100, "left border");
  expectBlack(frame, 415, 100, "right border");
  expectBlack(frame, 100, 0, "top border");
  expectBlack(frame, 100, 239, "bottom border");
  expectWhite(frame, 1, 100, "inside left border");
  expectWhite(frame, 200, 1, "inside top border");
  expectWhite(frame, 414, 5, "inside right border");
  expectWhite(frame, 100, 238, "inside bottom border");

  // Vertical separator bar at columns 159..160.
  expectBlack(frame, 159, 50, "separator");
  expectBlack(frame, 160, 150, "separator");
  expectBlack(frame, 160, 200, "separator");
  expectWhite(frame, 158, 150, "left of separator");
  expectWhite(frame, 161, 150, "right of separator");
}

TEST(NavSplashPortrait416x240, TextArrowRouteDotAndStatusScaleProportionally) {
  Frame frame(416, 240, kSentinel);
  NavSplash::draw(frame.pixels(), frame.width, frame.height);

  // Maneuver text in the physical left strip.
  expectTextEvidence(frame, 46, 5, 81, 135, 600, 3200, "180 M bbox (416)");
  expectTextEvidence(frame, 92, 5, 113, 122, 400, 1900, "DUINWEG bbox (416)");
  EXPECT_EQ(countBlack(frame, 81, 5, 92, 135), 0) << "inter-line gap not white (416)";
  // Old landscape maneuver text spot (right of center) stays empty.
  EXPECT_EQ(countBlack(frame, 333, 26, 390, 68), 0) << "right strip not empty (416)";

  // Arrow: logical shaft (76..89, 56..136) -> physical shaft columns 56..135,
  // rows 151..163; apex logical (16,56) -> physical (56,223).
  expectBlack(frame, 95, 157, "shaft (416)");
  expectBlack(frame, 56, 157, "head top at apex column (416)");
  expectBlack(frame, 56, 223, "arrowhead apex (416)");
  expectWhite(frame, 56, 150, "white above arrow (416)");
  expectWhite(frame, 90, 200, "white right of head (416)");

  // Ring marker at logical (108,249) -> physical (249,131), radius 13.
  expectBlack(frame, 249, 118, "ring top (416)");
  expectBlack(frame, 249, 144, "ring bottom (416)");
  expectBlack(frame, 236, 131, "ring left (416)");
  expectBlack(frame, 262, 131, "ring right (416)");
  expectWhite(frame, 249, 131, "ring interior (416)");
  expectWhite(frame, 249, 110, "white above ring (416)");

  // Route: left-entry bar at columns 248..250 rows 145..232 (logical
  // horizontal) and the down-exit bar at rows 130..132 columns 262..366
  // (logical vertical); the dot (305,131) r8 rides the down-exit bar.
  expectBlack(frame, 249, 150, "route left-entry bar (416)");
  expectWhite(frame, 249, 235, "white below route bar (416)");
  expectBlack(frame, 350, 131, "route down-exit bar (416)");
  expectWhite(frame, 380, 131, "white right of route bar (416)");
  expectBlack(frame, 305, 131, "dot center (416)");
  expectBlack(frame, 305, 123, "dot top (416)");
  expectBlack(frame, 305, 139, "dot bottom (416)");
  expectBlack(frame, 297, 131, "dot left (416)");
  expectBlack(frame, 313, 131, "dot right (416)");
  expectWhite(frame, 290, 120, "white left of dot (416)");

  // Status line: logical rows 394..414 -> physical right strip columns
  // 394..414, rows 14..226; the old landscape centered status strip is empty.
  expectTextEvidence(frame, 394, 14, 415, 227, 600, 3200, "status bbox (416)");
  EXPECT_EQ(countBlack(frame, 137, 224, 158, 238), 0) << "old status spot left of sep not empty (416)";
  EXPECT_EQ(countBlack(frame, 161, 224, 240, 238), 0) << "old status spot right of sep not empty (416)";
}

}  // namespace
