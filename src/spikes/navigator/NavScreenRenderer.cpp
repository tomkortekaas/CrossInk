#include "NavScreenRenderer.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "NavFontData.h"
#include "map/GrayMap.h"
#include "map/RouteMapRenderer.h"
#include "map/RouteViewport.h"
#include "route/RoutePackageV1.h"

// Direct-to-framebuffer 1-bpp renderer for the X3 navigator env.
//
// Framebuffer layout matches FreeInkDisplay: row-major 1-bpp, MSB-first
// (bit 7 of each byte = leftmost pixel), 1 = white, 0 = black. Each row is
// stored in ceil(widthPx / 8) bytes; a row that is not a multiple of 8 keeps
// its unused trailing bits white and never writes into them.
//
// draw() receives the PHYSICAL panel dimensions (widthPx x heightPx; the X3
// panel is 792x528) but authors the screen in logical PORTRAIT coordinates of
// heightPx x widthPx. Every logical pixel (lx, ly) is stored at the physical
// pixel (ly, heightPx - 1 - lx) -- the same 90-degree-clockwise Portrait
// mapping the repository's GfxRenderer applies (rotateCoordinates() in
// lib/GfxRenderer/GfxRenderer.cpp). The screen therefore reads upright when
// the device is held in portrait, like the rest of the reader UI.
//
// Shared layout contract (logical portrait space; must match the geometry
// mirror in test/navigator/NavScreenRendererTest.cpp):
//   border  t  = clamp(LW/300 + 1, 1, 4)
//   maneuverH = LH*38/100, separator under it, map area, bottom status line.
//   fonts scale with LH: large/mid lines right-aligned at xRight, status line
//   centered at the bottom.
// For the default NavState (the legacy splash example) the output is
// byte-identical to the old static NavSplash. Dynamic states vary the
// maneuver icon, the formatted distances, the street name (bounded and
// truncated to stay clear of the icon), and the whole band for non-navigating
// statuses ('ROUTE VERLATEN', 'BEREKENEN', 'AANGEKOMEN').
//
// No heap allocation, no GfxRenderer, no reader/renderer/font linkage; glyphs
// are tiny static-const bitmaps and every primitive clips safely.
namespace navigator {
namespace {

constexpr uint8_t kWhite = 0xFF;
constexpr uint8_t kBlack = 0x00;

int clamp(int value, int low, int high) { return value < low ? low : (value > high ? high : value); }

// Layout metrics for a physical panel, derived exactly as the legacy splash
// did so the default screen stays byte-identical.
struct Layout {
  NavGrayPlane plane = NavGrayPlane::Base;
  int physW;  // physical panel width (framebuffer stride)
  int physH;  // physical panel height
  int LW;     // logical width  = physical height
  int LH;     // logical height = physical width
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
};

Layout makeLayout(int physW, int physH) {
  Layout g{};
  g.physW = physW;
  g.physH = physH;
  g.LW = physH;
  g.LH = physW;
  g.border = clamp(g.LW / 300 + 1, 1, 4);
  g.maneuverH = g.LH * 38 / 100;
  g.sepY = g.border + g.maneuverH;
  g.sepThick = g.LH / 200 < 1 ? 1 : g.LH / 200;
  g.mapTop = g.sepY + g.sepThick;
  g.largeScale = clamp((g.LH + 99) / 100, 2, 8);
  g.midScale = clamp((g.LH + 149) / 150, 2, 6);
  g.statusScale = clamp((g.LH + 199) / 200, 1, 3);
  g.blockGap = 7 * g.largeScale / 3 < 2 ? 2 : 7 * g.largeScale / 3;
  g.blockH = 7 * g.largeScale + g.blockGap + 7 * g.midScale;
  g.blockY = g.border + (g.maneuverH - g.blockH) / 2;
  g.padR = g.LW / 60 < 4 ? 4 : g.LW / 60;
  g.xRight = g.LW - g.border - g.padR;
  g.statusY = g.LH - g.border - 7 * g.statusScale;
  g.mapH = g.statusY - g.mapTop;
  g.iconH = g.maneuverH * 72 / 100;
  g.arrY0 = g.border + (g.maneuverH - g.iconH) / 2;
  g.arrY1 = g.arrY0 + g.iconH;
  g.arrowThick = g.maneuverH / 12 < 2 ? 2 : g.maneuverH / 12;
  g.boxW = g.maneuverH * 70 / 100;
  g.boxX0 = g.border + g.maneuverH / 10;
  g.boxX1 = g.boxX0 + g.boxW;
  g.cx = g.boxX0 + g.boxW / 2;
  g.cornerY = g.arrY0 + g.iconH * 30 / 100;
  g.headH = 6 > g.arrowThick * 2 ? 6 : g.arrowThick * 2;
  g.headHalf = g.headH / 2;
  g.routeThick = g.LW / 66 < 3 ? 3 : g.LW / 66;
  g.bendX = g.LW * 45 / 100;
  g.bendY = g.mapTop + g.mapH * 38 / 100;
  g.markerRadius = g.LH / 30 < 4 ? 4 : g.LH / 30;
  g.ringThick = g.routeThick / 3 < 2 ? 2 : g.routeThick / 3;
  g.dotY = g.mapTop + g.mapH * 62 / 100;
  g.dotRadius = g.LH / 48 < 3 ? 3 : g.LH / 48;
  g.routeBottom = g.mapTop + g.mapH * 88 / 100;
  g.xLeft = g.border + (g.LW / 40 < 4 ? 4 : g.LW / 40);
  return g;
}

// ---------------------------------------------------------------------------
// Minimal fixed bitmap glyphs (7 rows each, top to bottom). Bit c of each row
// encodes column c from the left; all are ASCII-uppercase because the tiny
// embedded set is uppercase only. The set covers A-Z, 0-9 and the punctuation
// needed by dynamic street names, distances, and the status screens. Unknown
// characters fall back to a blank space.
struct Glyph {
  char ch;
  uint8_t width;
  uint8_t rows[7];
};

constexpr Glyph kGlyphs[] = {
    {' ', 3, {0, 0, 0, 0, 0, 0, 0}},        {',', 3, {0, 0, 0, 0, 6, 4, 2}},
    {'.', 3, {0, 0, 0, 0, 0, 0, 2}},        {'-', 5, {0, 0, 0, 0, 14, 0, 0}},
    {'0', 5, {14, 17, 17, 17, 17, 17, 14}}, {'1', 4, {4, 6, 4, 4, 4, 4, 14}},
    {'2', 5, {31, 16, 16, 14, 1, 1, 31}},   {'3', 5, {15, 16, 16, 14, 16, 16, 15}},
    {'4', 5, {17, 17, 17, 31, 16, 16, 16}}, {'5', 5, {31, 1, 15, 16, 16, 17, 14}},
    {'6', 5, {14, 1, 1, 15, 17, 17, 14}},   {'7', 5, {31, 16, 8, 4, 2, 2, 2}},
    {'8', 5, {14, 17, 17, 14, 17, 17, 14}}, {'9', 5, {14, 17, 17, 30, 16, 16, 14}},
    {'A', 5, {14, 17, 17, 31, 17, 17, 17}}, {'B', 5, {15, 17, 17, 15, 17, 17, 15}},
    {'C', 5, {14, 17, 1, 1, 1, 17, 14}},    {'D', 5, {15, 17, 17, 17, 17, 17, 15}},
    {'E', 5, {31, 1, 1, 15, 1, 1, 31}},     {'F', 5, {31, 1, 1, 15, 1, 1, 1}},
    {'G', 5, {14, 17, 1, 29, 17, 17, 14}},  {'H', 5, {17, 17, 17, 31, 17, 17, 17}},
    {'I', 3, {7, 2, 2, 2, 2, 2, 7}},        {'J', 5, {30, 16, 16, 16, 16, 17, 14}},
    {'K', 5, {17, 9, 5, 3, 5, 9, 17}},      {'L', 5, {1, 1, 1, 1, 1, 1, 31}},
    {'M', 5, {17, 27, 21, 17, 17, 17, 17}}, {'N', 5, {17, 19, 21, 25, 17, 17, 17}},
    {'O', 5, {14, 17, 17, 17, 17, 17, 14}}, {'P', 5, {15, 17, 17, 15, 1, 1, 1}},
    {'Q', 5, {14, 17, 17, 17, 17, 18, 13}}, {'R', 5, {15, 17, 17, 15, 5, 9, 17}},
    {'S', 5, {30, 1, 1, 14, 16, 16, 15}},   {'T', 5, {31, 4, 4, 4, 4, 4, 4}},
    {'U', 5, {17, 17, 17, 17, 17, 17, 14}}, {'V', 5, {17, 17, 17, 17, 17, 10, 4}},
    {'W', 5, {17, 17, 21, 21, 21, 21, 10}}, {'X', 5, {17, 17, 10, 4, 10, 17, 17}},
    {'Y', 5, {17, 17, 10, 4, 4, 4, 4}},     {'Z', 5, {31, 1, 2, 4, 8, 16, 31}},
};

const Glyph& glyphFor(char ch) {
  if (ch >= 'a' && ch <= 'z') {
    ch = static_cast<char>(ch - 'a' + 'A');
  }
  for (const Glyph& glyph : kGlyphs) {
    if (glyph.ch == ch) {
      return glyph;
    }
  }
  return kGlyphs[0];  // space
}

int glyphWidth(char ch) { return glyphFor(ch).width; }

// ---------------------------------------------------------------------------
// Physical 1-bpp primitives
// ---------------------------------------------------------------------------
//
// Everything below that takes (widthPx, heightPx) addresses the *physical*
// row-major 1-bpp MSB-first buffer (1 = white, 0 = black): widthPx is the
// framebuffer stride width and clipping is against the physical panel.

// Clipped rectangle fill on the physical buffer. Fully covered bytes are
// written whole; edge bytes go through a bit mask so pixels outside the
// rectangle (including unused trailing bits of non-byte-aligned rows) are
// never touched.
void fillRect(uint8_t* frameBuffer, int widthPx, int heightPx, int x0, int y0, int w, int h, bool black) {
  if (w <= 0 || h <= 0) {
    return;
  }
  int x1 = x0 + w;
  int y1 = y0 + h;
  if (x0 < 0) {
    x0 = 0;
  }
  if (y0 < 0) {
    y0 = 0;
  }
  if (x1 > widthPx) {
    x1 = widthPx;
  }
  if (y1 > heightPx) {
    y1 = heightPx;
  }
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  const int widthBytes = (widthPx + 7) / 8;

  for (int y = y0; y < y1; ++y) {
    uint8_t* row = frameBuffer + static_cast<size_t>(y) * static_cast<size_t>(widthBytes);
    const int firstByte = x0 >> 3;
    const int lastByte = (x1 - 1) >> 3;
    if (firstByte == lastByte) {
      uint8_t mask = 0;
      for (int x = x0; x < x1; ++x) {
        mask = static_cast<uint8_t>(mask | static_cast<uint8_t>(0x80 >> (x & 7)));
      }
      row[firstByte] =
          black ? static_cast<uint8_t>(row[firstByte] & ~mask) : static_cast<uint8_t>(row[firstByte] | mask);
      continue;
    }
    const uint8_t leftMask = static_cast<uint8_t>(0xFF >> (x0 & 7));
    row[firstByte] =
        black ? static_cast<uint8_t>(row[firstByte] & ~leftMask) : static_cast<uint8_t>(row[firstByte] | leftMask);
    if (lastByte > firstByte + 1) {
      std::memset(row + firstByte + 1, black ? kBlack : kWhite, static_cast<size_t>(lastByte - firstByte - 1));
    }
    const int rightBits = x1 & 7;  // pixels used in the final byte; 0 = whole byte
    const uint8_t rightMask = rightBits == 0 ? 0xFF : static_cast<uint8_t>(0xFF << (8 - rightBits));
    row[lastByte] =
        black ? static_cast<uint8_t>(row[lastByte] & ~rightMask) : static_cast<uint8_t>(row[lastByte] | rightMask);
  }
}

uint32_t isqrt(uint32_t value) {
  uint32_t root = 0;
  uint32_t bit = 1U << 30;
  while (bit > value) {
    bit >>= 2;
  }
  while (bit != 0) {
    if (value >= root + bit) {
      value -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return root;
}

// Filled disc on the physical buffer (0 = not drawn) with full clipping.
void fillDisc(uint8_t* frameBuffer, int widthPx, int heightPx, int cx, int cy, int radius, bool black) {
  if (radius < 0) {
    return;
  }
  if (radius == 0) {
    fillRect(frameBuffer, widthPx, heightPx, cx, cy, 1, 1, black);
    return;
  }
  const uint32_t radiusSq = static_cast<uint32_t>(radius) * static_cast<uint32_t>(radius);
  for (int dy = -radius; dy <= radius; ++dy) {
    const int y = cy + dy;
    if (y < 0 || y >= heightPx) {
      continue;
    }
    const uint32_t dySq = static_cast<uint32_t>(dy) * static_cast<uint32_t>(dy);
    if (dySq > radiusSq) {
      continue;
    }
    const int span = static_cast<int>(isqrt(radiusSq - dySq));
    fillRect(frameBuffer, widthPx, heightPx, cx - span, y, 2 * span + 1, 1, black);
  }
}

// ---------------------------------------------------------------------------
// Portrait logical-space wrappers
// ---------------------------------------------------------------------------
//
// The navigation UI is authored in logical portrait coordinates. The physical
// panel is landscape (widthPx x heightPx), so the logical canvas is
// (heightPx x widthPx) and every logical pixel (lx, ly) is stored at the
// physical pixel (ly, heightPx - 1 - lx). This is the repository GfxRenderer
// Portrait transform (90 degrees clockwise, see rotateCoordinates() in
// lib/GfxRenderer/GfxRenderer.cpp). Rotation is an isometry, so an
// axis-aligned logical rectangle maps to an axis-aligned physical rectangle
// with swapped extents, and a logical disc maps to a physical disc of the
// same radius. The wrappers below exploit that so the byte-run optimized
// physical primitives stay untouched and no per-pixel logical loop is needed.

// Logical rectangle (x0, y0, w, h) -> physical rectangle.
void fillRectLog(uint8_t* frameBuffer, const Layout& g, int x0, int y0, int w, int h, bool black) {
  if (w <= 0 || h <= 0) {
    return;
  }
  fillRect(frameBuffer, g.physW, g.physH, y0, g.physH - 1 - (x0 + w - 1), h, w,
           g.plane == NavGrayPlane::Base ? black : true);
}

// Logical disc: rotate the center and keep the radius.
void fillDiscLog(uint8_t* frameBuffer, const Layout& g, int cx, int cy, int radius, bool black) {
  fillDisc(frameBuffer, g.physW, g.physH, cy, g.physH - 1 - cx, radius, g.plane == NavGrayPlane::Base ? black : true);
}

// Filled triangle head with a vertical base at x = baseX spanning
// apexY +/- headHalf and its apex at (apexX, apexY). Works pointing left
// (baseX > apexX, the legacy left-turn shape) and mirrored pointing right
// (baseX < apexX). Rows are emitted through fillRectLog so the whole head
// rotates with the rest of the frame.
void fillHeadLog(uint8_t* frameBuffer, const Layout& g, int apexX, int apexY, int baseX, int headHalf, bool black) {
  if (headHalf <= 0 || baseX == apexX) {
    return;
  }
  const int topY = apexY - headHalf;
  const int botY = apexY + headHalf;
  for (int y = topY; y <= botY; ++y) {
    int t;
    if (y <= apexY) {
      t = apexY - y;
    } else {
      t = y - apexY;
    }
    const int xEdge = apexX + (baseX - apexX) * t / headHalf;
    fillRectLog(frameBuffer, g, std::min(xEdge, baseX), y, std::abs(xEdge - baseX) + 1, 1, black);
  }
}

// Filled triangle head with a horizontal base at row baseY spanning
// apexX +/- halfW and its apex at (apexX, apexY); points up when baseY >
// apexY and down when baseY < apexY.
void fillHeadVLog(uint8_t* frameBuffer, const Layout& g, int apexX, int apexY, int baseY, int halfW, bool black) {
  const int topY = std::min(apexY, baseY);
  const int botY = std::max(apexY, baseY);
  const int span = botY - topY;
  if (halfW <= 0 || span <= 0) {
    return;
  }
  for (int y = topY; y <= botY; ++y) {
    const int t = std::abs(y - apexY);
    const int half = halfW * t / span;
    fillRectLog(frameBuffer, g, apexX - half, y, 2 * half + 1, 1, black);
  }
}

// Master (advance) width of a text run in unscaled units.
int textMasterWidth(const char* text) {
  int total = 0;
  int count = 0;
  for (const char* p = text; *p != '\0'; ++p) {
    total += glyphWidth(*p);
    ++count;
  }
  return count > 0 ? total + count - 1 : 0;
}

// Draw a horizontal line of glyphs in logical space at logical (x0, y0).
void drawText(uint8_t* frameBuffer, const Layout& g, int x0, int y0, const char* text, int scale, bool black) {
  int x = x0;
  for (const char* p = text; *p != '\0'; ++p) {
    const Glyph& glyph = glyphFor(*p);
    for (int row = 0; row < 7; ++row) {
      for (int col = 0; col < glyph.width; ++col) {
        if ((glyph.rows[row] & (1U << col)) != 0) {
          fillRectLog(frameBuffer, g, x + col * scale, y0 + row * scale, scale, scale, black);
        }
      }
    }
    x += (glyph.width + 1) * scale;
  }
}

// Proportional Noto Sans at native pixel sizes. Static flash data, no font
// engine, decoding buffer or runtime allocation. y is the top of capitals.
int normalTextWidth(const NavFont& font, const char* text) {
  int width = 0;
  if (!text) return 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p)
    width += font.glyphs[*p >= 32 && *p <= 126 ? *p - 32 : 0].advance;
  return width;
}
void drawNormalText(uint8_t* fb, const Layout& g, int x, int y, const char* text, const NavFont& font,
                    bool bold = false) {
  if (!text) return;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
    const auto& glyph = font.glyphs[*p >= 32 && *p <= 126 ? *p - 32 : 0];
    const int stride = (glyph.width + 7) / 8;
    for (int row = 0; row < glyph.height; ++row) {
      for (int col = 0; col < glyph.width; ++col) {
        if (font.data[glyph.offset + row * stride + col / 8] & (0x80 >> (col % 8)))
          fillRectLog(fb, g, x + glyph.x + col, y + glyph.y + row, bold ? 2 : 1, 1, true);
      }
    }
    x += glyph.advance;
  }
}

// ---------------------------------------------------------------------------
// Text formatting (stack buffers only, no heap).
// ---------------------------------------------------------------------------

// Decimal digits of value (no terminator); returns the number of chars.
int writeUint(char* out, uint32_t value) {
  char reversed[11];
  int n = 0;
  do {
    reversed[n++] = static_cast<char>('0' + static_cast<int>(value % 10));
    value /= 10;
  } while (value != 0);
  for (int i = 0; i < n; ++i) {
    out[i] = reversed[n - 1 - i];
  }
  return n;
}

// "180 M" under a kilometer, otherwise "4,2 KM" / "12 KM". Whole kilometers
// (no tenths, or >= 100 km) drop the decimal so the line stays compact.
int formatDistanceMeters(char* out, uint32_t meters) {
  int n;
  if (meters < 1000) {
    n = writeUint(out, meters);
    out[n++] = ' ';
    out[n++] = 'M';
  } else {
    const uint32_t km = meters / 1000;
    const uint32_t tenths = (meters % 1000) / 100;
    if (tenths != 0 && km < 100) {
      n = writeUint(out, km);
      out[n++] = ',';
      n += writeUint(out + n, tenths);
    } else {
      n = writeUint(out, km);
    }
    out[n++] = ' ';
    out[n++] = 'K';
    out[n++] = 'M';
  }
  out[n] = '\0';
  return n;
}

const char* statusMessage(NavStatus status) {
  switch (status) {
    case NavStatus::OffRoute:
      return "ROUTE VERLATEN";
    case NavStatus::Recalculating:
      return "BEREKENEN";
    case NavStatus::Arrived:
      return "AANGEKOMEN";
    default:
      return "";
  }
}

// ---------------------------------------------------------------------------
// Maneuver icons (logical space).
// ---------------------------------------------------------------------------

// Rightmost painted logical x of a maneuver icon; the fitted text never draws
// left of this edge so the street name cannot collide with the arrow.
int maneuverIconRightEdge(const Layout& g, Maneuver maneuver) {
  const int stemX = g.boxX0 + g.boxW * 60 / 100;
  const int poleHalf = g.arrowThick / 5 < 2 ? 2 : g.arrowThick / 5;
  switch (maneuver) {
    case Maneuver::Straight:
    case Maneuver::SlightLeft:
      return g.cx + g.arrowThick / 2;
    case Maneuver::Left:
      return stemX + g.arrowThick / 2;
    case Maneuver::Right:
      return g.boxX1;
    case Maneuver::SlightRight:
      return g.cx + g.boxW * 25 / 100;
    case Maneuver::UTurn:
      return g.cx + g.boxW * 20 / 100 + g.headHalf;
    case Maneuver::Arrive:
      return g.cx + poleHalf + 1 + g.boxW * 30 / 100;
    case Maneuver::kCount:
      break;
  }
  return g.boxX1;
}

void drawManeuverIcon(uint8_t* frameBuffer, const Layout& g, Maneuver maneuver) {
  const int L = g.boxX0;
  const int R = g.boxX1;
  const int T = g.arrY0;
  const int B = g.arrY1;
  const int thick = g.arrowThick;
  switch (maneuver) {
    case Maneuver::Straight: {
      // Up arrow in the center: triangle head over a vertical stem.
      const int apexY = T + g.iconH * 12 / 100;
      const int baseY = apexY + g.headH;
      fillHeadVLog(frameBuffer, g, g.cx, apexY, baseY, g.headHalf, true);
      fillRectLog(frameBuffer, g, g.cx - thick / 2, baseY, thick, B - baseY, true);
      break;
    }
    case Maneuver::Left: {
      // Legacy bold left turn: vertical shaft below a head pointing left.
      const int stemX = L + g.boxW * 60 / 100;
      const int shaftX = stemX - thick / 2;
      fillRectLog(frameBuffer, g, shaftX, g.cornerY, thick, B - g.cornerY, true);
      fillHeadLog(frameBuffer, g, L, g.cornerY, shaftX, g.headHalf, true);
      break;
    }
    case Maneuver::Right: {
      // Mirror of Left about the icon box's vertical center.
      const int stemX = L + g.boxW * 60 / 100;
      const int shaftX = stemX - thick / 2;
      const int shaftXr = L + R - (shaftX + thick);
      fillRectLog(frameBuffer, g, shaftXr, g.cornerY, thick, B - g.cornerY, true);
      fillHeadLog(frameBuffer, g, R, g.cornerY, L + R - shaftX, g.headHalf, true);
      break;
    }
    case Maneuver::SlightLeft: {
      // Center stem with a shallower head that stops short of the box edge.
      fillRectLog(frameBuffer, g, g.cx - thick / 2, g.cornerY, thick, B - g.cornerY, true);
      fillHeadLog(frameBuffer, g, g.cx - g.boxW * 25 / 100, g.cornerY, g.cx, g.headHalf, true);
      break;
    }
    case Maneuver::SlightRight: {
      fillRectLog(frameBuffer, g, g.cx - thick / 2, g.cornerY, thick, B - g.cornerY, true);
      fillHeadLog(frameBuffer, g, g.cx + g.boxW * 25 / 100, g.cornerY, g.cx, g.headHalf, true);
      break;
    }
    case Maneuver::UTurn: {
      // Drive up the center stem, loop right over the top bar, come back down
      // with a downward head: a 180-degree turn-around.
      const int topLoopY = T + g.iconH * 20 / 100;
      const int exitX = g.cx + g.boxW * 20 / 100;
      const int downApexY = T + g.iconH * 78 / 100;
      const int downBaseY = downApexY - g.headH;
      fillRectLog(frameBuffer, g, g.cx - thick / 2, topLoopY, thick, B - topLoopY, true);
      fillRectLog(frameBuffer, g, g.cx, topLoopY, exitX - g.cx, thick, true);
      fillRectLog(frameBuffer, g, exitX - thick / 2, topLoopY + thick, thick, downBaseY - (topLoopY + thick), true);
      fillHeadVLog(frameBuffer, g, exitX, downApexY, downBaseY, g.headHalf, true);
      break;
    }
    case Maneuver::Arrive: {
      // Destination flag: a pole with a small flag at the top.
      const int poleHalf = g.arrowThick / 5 < 2 ? 2 : g.arrowThick / 5;
      const int flagTopY = T + g.iconH * 18 / 100;
      fillRectLog(frameBuffer, g, g.cx - poleHalf, flagTopY, 2 * poleHalf + 1, B - flagTopY, true);
      fillRectLog(frameBuffer, g, g.cx + poleHalf + 1, flagTopY, g.boxW * 30 / 100, thick, true);
      break;
    }
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Screen regions
// ---------------------------------------------------------------------------

void drawOuterBorder(uint8_t* frameBuffer, const Layout& g) {
  fillRectLog(frameBuffer, g, 0, 0, g.LW, g.border, true);
  fillRectLog(frameBuffer, g, 0, g.LH - g.border, g.LW, g.border, true);
  fillRectLog(frameBuffer, g, 0, 0, g.border, g.LH, true);
  fillRectLog(frameBuffer, g, g.LW - g.border, 0, g.border, g.LH, true);
}

// Navigating band: right-aligned next-distance and street text plus the
// maneuver icon on the left. Text is fitted so it never collides with the
// icon strokes; street names are prefix-truncated at the icon edge.
void drawNavigatingBand(uint8_t* frameBuffer, const Layout& g, const NavState& state) {
  const int iconRight = maneuverIconRightEdge(g, state.maneuver);
  const int textLeftLimit = iconRight + 2;
  const int availPx = g.xRight - textLeftLimit;

  char distanceLine[24];
  formatDistanceMeters(distanceLine, state.nextDistanceMeters);
  const int distanceMaster = textMasterWidth(distanceLine);

  // Big numeric distance first; drop to a smaller scale instead of clipping.
  int distanceScale = g.largeScale;
  while (distanceScale > 1 && distanceMaster * distanceScale > availPx) {
    --distanceScale;
  }
  drawText(frameBuffer, g, g.xRight - distanceMaster * distanceScale, g.blockY, distanceLine, distanceScale, true);

  // Street name: right-aligned; draw the longest prefix that stays clear of
  // the icon (safe truncation at the display level).
  char streetBuf[NavState::kStreetCapacity];
  int streetMaster = 0;
  int streetChars = 0;
  for (const char* p = state.street; *p != '\0'; ++p) {
    const int nextMaster = streetMaster + glyphWidth(*p) + (streetChars == 0 ? 0 : 1);
    if (nextMaster * g.midScale > availPx) {
      break;
    }
    streetMaster = nextMaster;
    streetBuf[streetChars++] = *p;
  }
  streetBuf[streetChars] = '\0';
  if (streetChars > 0) {
    const int streetY = g.blockY + 7 * g.largeScale + g.blockGap;
    drawText(frameBuffer, g, g.xRight - streetMaster * g.midScale, streetY, streetBuf, g.midScale, true);
  }

  drawManeuverIcon(frameBuffer, g, state.maneuver);
}

// Non-navigating status band: one unmistakable message instead of the icon
// and maneuver text, scaled to fit the full band width.
void drawStatusBand(uint8_t* frameBuffer, const Layout& g, NavStatus status) {
  const char* message = statusMessage(status);
  const int messageMaster = textMasterWidth(message);
  const int availPx = g.LW - 2 * (g.border + g.padR);
  int scale = g.largeScale;
  while (scale > 1 && messageMaster * scale > availPx) {
    --scale;
  }
  const int x0 = (g.LW - messageMaster * scale) / 2;
  const int y0 = g.border + (g.maneuverH - 7 * scale) / 2;
  drawText(frameBuffer, g, x0, y0, message, scale, true);
}

// Lower map area. Navigating shows the full example route (ring at the bend,
// route bars, position dot). Recalculating keeps the route but no
// next-maneuver ring; off-route keeps only the current-position dot; arrived
// shows a hollow destination ring where the dot used to sit.
void drawMapArea(uint8_t* frameBuffer, const Layout& g, NavStatus status) {
  if (status == NavStatus::Arrived) {
    fillDiscLog(frameBuffer, g, g.bendX, g.dotY, g.markerRadius, true);
    fillDiscLog(frameBuffer, g, g.bendX, g.dotY, g.markerRadius - g.ringThick, false);
    return;
  }
  if (status == NavStatus::Navigating) {
    fillDiscLog(frameBuffer, g, g.bendX, g.bendY, g.markerRadius, true);
    fillDiscLog(frameBuffer, g, g.bendX, g.bendY, g.markerRadius - g.ringThick, false);
  }
  if (status == NavStatus::Navigating || status == NavStatus::Recalculating) {
    fillRectLog(frameBuffer, g, g.xLeft, g.bendY - g.routeThick / 2, g.bendX - g.markerRadius - g.xLeft, g.routeThick,
                true);
    fillRectLog(frameBuffer, g, g.bendX - g.routeThick / 2, g.bendY + g.markerRadius, g.routeThick,
                g.routeBottom - (g.bendY + g.markerRadius), true);
  }
  fillDiscLog(frameBuffer, g, g.bendX, g.dotY, g.dotRadius, true);
}

// ---------------------------------------------------------------------------
// Real-route canvas adapter (logical portrait map rectangle)
// ---------------------------------------------------------------------------
//
// MapRouteCanvas adapts RouteCanvas (map-local logical portrait map pixels,
// origin at the map's top-left corner) to the physical 1-bpp framebuffer
// through the existing portrait logical wrappers. The canvas logical size
// equals the routed viewport's map rectangle; every primitive is translated by
// that rectangle's screen origin and clipped to it, so route ink can never
// reach the border, separator, status band or bottom status text.

// Floor / ceil division by a positive denominator, correct for negative
// numerators (used to rasterize rational line-pen bounds).
int64_t floorDivPos(int64_t a, int64_t b) {
  int64_t q = a / b;
  if (a % b < 0) {
    --q;
  }
  return q;
}

int64_t ceilDivPos(int64_t a, int64_t b) { return -floorDivPos(-a, b); }

// 1-pixel Bresenham line, clipped per pixel to the inclusive logical box.
void fillThinLineLog(uint8_t* frameBuffer, const Layout& g, int x0, int y0, int x1, int y1, int clipX0, int clipY0,
                     int clipX1, int clipY1, bool black = true) {
  if (x0 == x1 && y0 == y1) {
    return;  // zero-length line draws nothing
  }
  int x = x0;
  int y = y0;
  const int dx = x0 < x1 ? x1 - x0 : x0 - x1;
  const int sx = x0 < x1 ? 1 : -1;
  const int dy = y0 < y1 ? y0 - y1 : y1 - y0;  // negative magnitude
  const int sy = y0 < y1 ? 1 : -1;
  int error = dx + dy;
  for (;;) {
    if (x >= clipX0 && x <= clipX1 && y >= clipY0 && y <= clipY1) {
      fillRectLog(frameBuffer, g, x, y, 1, 1, black);
    }
    if (x == x1 && y == y1) {
      break;
    }
    const int twice = 2 * error;
    if (twice >= dy) {
      error += dy;
      x += sx;
    }
    if (twice <= dx) {
      error += dx;
      y += sy;
    }
  }
}

// Square-pen thick line (width >= 2), rasterized as one clipped span per row
// of the pen band. The pen is the Minkowski sum of the segment and an
// axis-aligned (2*half + 1)-pixel square where half = width / 2, matching the
// RouteCanvas contract ("the pen may overhang endpoints by up to
// (widthPx - 1) / 2 pixels and the canvas clips it to its own bounds"). All
// arithmetic is integer; every span is clipped to the logical clip box so ink
// never leaves the map rectangle.
void fillWideLineLog(uint8_t* frameBuffer, const Layout& g, int x0, int y0, int x1, int y1, int width, int clipX0,
                     int clipY0, int clipX1, int clipY1, bool black = true) {
  if (width < 1 || (x0 == x1 && y0 == y1)) {
    return;
  }
  const int half = width / 2;

  // Axis-aligned fast paths: one clipped rectangle covers the whole pen band.
  if (y0 == y1) {
    const int xL = std::max(std::min(x0, x1) - half, clipX0);
    const int xR = std::min(std::max(x0, x1) + half, clipX1);
    const int yT = std::max(y0 - half, clipY0);
    const int yB = std::min(y0 + half, clipY1);
    if (xL <= xR && yT <= yB) {
      fillRectLog(frameBuffer, g, xL, yT, xR - xL + 1, yB - yT + 1, black);
    }
    return;
  }
  if (x0 == x1) {
    const int yT = std::max(std::min(y0, y1) - half, clipY0);
    const int yB = std::min(std::max(y0, y1) + half, clipY1);
    const int xL = std::max(x0 - half, clipX0);
    const int xR = std::min(x0 + half, clipX1);
    if (xL <= xR && yT <= yB) {
      fillRectLog(frameBuffer, g, xL, yT, xR - xL + 1, yB - yT + 1, black);
    }
    return;
  }

  // General case: walk the rows the pen can cover (segment's y span grown by
  // half, clipped to the box) and fill one clipped x span per row.
  if (y0 > y1) {
    std::swap(x0, x1);
    std::swap(y0, y1);  // symmetric region; dy becomes positive
  }
  const int64_t dx = static_cast<int64_t>(x1) - x0;
  const int64_t dy = static_cast<int64_t>(y1) - y0;  // > 0
  const int yFirst = std::max(clipY0, y0 - half);
  const int yLast = std::min(clipY1, y1 + half);
  for (int y = yFirst; y <= yLast; ++y) {
    const int64_t yOffset = static_cast<int64_t>(y) - y0;
    if (yOffset + half < 0 || yOffset - half > dy) {
      continue;
    }
    // Parameter interval [tA/dy, tB/dy] of the segment rows whose pen square
    // covers this row: |y - (y0 + t*dy)| <= half with t in [0, 1].
    const int64_t tA = std::max<int64_t>(yOffset - half, 0);
    const int64_t tB = std::min<int64_t>(yOffset + half, dy);
    if (tA > tB) {
      continue;
    }
    // x(t) = x0 + t*dx is monotone in t, so the pen's x interval over
    // [tA, tB] widened by half has integer columns [ceil(lo), floor(hi)].
    const int64_t xANum = static_cast<int64_t>(x0) * dy + tA * dx;
    const int64_t xBNum = static_cast<int64_t>(x0) * dy + tB * dx;
    int64_t loNum;
    int64_t hiNum;
    if (dx >= 0) {
      loNum = xANum - half * dy;
      hiNum = xBNum + half * dy;
    } else {
      loNum = xBNum - half * dy;
      hiNum = xANum + half * dy;
    }
    int64_t colLo = ceilDivPos(loNum, dy);
    int64_t colHi = floorDivPos(hiNum, dy);
    colLo = std::max(colLo, static_cast<int64_t>(clipX0));
    colHi = std::min(colHi, static_cast<int64_t>(clipX1));
    if (colLo <= colHi) {
      fillRectLog(frameBuffer, g, static_cast<int>(colLo), y, static_cast<int>(colHi - colLo + 1), 1, black);
    }
  }
}

// Route pen entry point: width 1 uses the thin Bresenham path, wider pens the
// square-pen band. All coordinates are logical screen pixels.
void fillRouteLineLog(uint8_t* frameBuffer, const Layout& g, int x0, int y0, int x1, int y1, int width, int clipX0,
                      int clipY0, int clipX1, int clipY1, bool black = true) {
  if (width <= 1) {
    fillThinLineLog(frameBuffer, g, x0, y0, x1, y1, clipX0, clipY0, clipX1, clipY1, black);
  } else {
    fillWideLineLog(frameBuffer, g, x0, y0, x1, y1, width, clipX0, clipY0, clipX1, clipY1, black);
  }
}

class MapRouteCanvas final : public RouteCanvas {
 public:
  MapRouteCanvas(uint8_t* frameBuffer, const Layout& g, int mapX, int mapY, int mapW, int mapH, int minStroke = 1,
                 bool styled = false)
      : frameBuffer_(frameBuffer),
        g_(g),
        mapX_(mapX),
        mapY_(mapY),
        mapW_(mapW),
        mapH_(mapH),
        minStroke_(minStroke),
        styled_(styled) {}

  void clear() override {
    placedCount_ = 0;
    // Background (white), restricted to the logical map rectangle only.
    fillRectLog(frameBuffer_, g_, mapX_, mapY_, mapW_, mapH_, false);
  }

  void line(int x0, int y0, int x1, int y1, int widthPx) override {
    if (widthPx < 1) {
      return;
    }
    if (styled_ && widthPx == RouteMapRenderer::kRouteLineWidthPx) {
      fillRouteLineLog(frameBuffer_, g_, x0 + mapX_, y0 + mapY_, x1 + mapX_, y1 + mapY_,
                       std::max(widthPx, minStroke_) + 4, mapX_, mapY_, mapX_ + mapW_ - 1, mapY_ + mapH_ - 1, false);
    }
    fillRouteLineLog(frameBuffer_, g_, x0 + mapX_, y0 + mapY_, x1 + mapX_, y1 + mapY_,
                     (widthPx == RouteMapRenderer::kRouteLineWidthPx ? std::max(widthPx, minStroke_) : widthPx), mapX_,
                     mapY_, mapX_ + mapW_ - 1, mapY_ + mapH_ - 1);
    if (styled_ && widthPx == RouteMapRenderer::kRouteLineWidthPx) {
      const int core = std::max(widthPx, minStroke_) / 2;
      fillDiscLog(frameBuffer_, g_, x0 + mapX_, y0 + mapY_, core, true);
      fillDiscLog(frameBuffer_, g_, x1 + mapX_, y1 + mapY_, core, true);
    }
  }

  void toneSpan(int x, int y, int width, uint8_t tone) override {
    if (y < 0 || y >= mapH_ || width <= 0) return;
    const int left = std::max(0, x), right = std::min(mapW_, x + width);
    if (right <= left) return;
    // Native light gray looks substantially darker on the panel than in the
    // desktop preview. Mix it with white in stable 2x2 blocks so buildings and
    // green areas recede. Dark-gray water keeps its solid tone.
    if (tone == 2) {
      for (int start = left; start < right;) {
        const bool lightPixel = ((start >> 1) + (y >> 1)) % 2 == 0;
        const int end = std::min(right, (start & ~1) + 2);
        const bool black = lightPixel ? g_.plane != NavGrayPlane::Msb : g_.plane != NavGrayPlane::Base;
        fillRect(frameBuffer_, g_.physW, g_.physH, mapY_ + y, g_.physH - (mapX_ + end), 1, end - start, black);
        start = end;
      }
      return;
    }
    const bool black = g_.plane == NavGrayPlane::Base  ? tone != 3
                       : g_.plane == NavGrayPlane::Lsb ? tone != 1
                                                       : tone != 1;
    // Bypass logical BW-only wrappers: this writes actual gray-mask bits.
    fillRect(frameBuffer_, g_.physW, g_.physH, mapY_ + y, g_.physH - (mapX_ + right), 1, right - left, black);
  }

  void disc(int centerX, int centerY, int radiusPx) override {
    const int radius = clampedRadius(centerX, centerY, radiusPx);
    if (radius < 0) {
      return;
    }
    fillDiscLog(frameBuffer_, g_, centerX + mapX_, centerY + mapY_, radius, true);
  }

  void ring(int centerX, int centerY, int radiusPx, int widthPx) override {
    if (radiusPx < 1 || widthPx < 1) {
      return;
    }
    const int radius = clampedRadius(centerX, centerY, radiusPx);
    if (radius < 1) {
      return;
    }
    // Ink annulus: outer filled disc, then the background disc inside. Both
    // stay clipped to the map rectangle.
    fillDiscLog(frameBuffer_, g_, centerX + mapX_, centerY + mapY_, radius, true);
    const int inner = radius - widthPx;
    if (inner >= 1) {
      fillDiscLog(frameBuffer_, g_, centerX + mapX_, centerY + mapY_, inner, false);
    }
  }

  void label(int x, int y, const char* text) override {
    if (!text || placedCount_ >= 12) return;
    char visible[45]{};
    std::strncpy(visible, text, sizeof(visible) - 1);
    while ((styled_ ? normalTextWidth(navFont18, visible) : textMasterWidth(visible) * 2) > mapW_ - 24 && visible[0])
      visible[std::strlen(visible) - 1] = '\0';
    const int w = (styled_ ? normalTextWidth(navFont18, visible) : textMasterWidth(visible) * 2) + 8, h = 22;
    if (w < 10 || y < 12 || y > mapH_ - 44) return;
    const int left = std::clamp(x - w / 2, 4, mapW_ - w - 4), top = y - h / 2;
    // The live marker and short route-direction arrow occupy the center.
    // Do not place a name underneath them and then erase half its letters.
    if (left < mapW_ / 2 + 85 && left + w > mapW_ / 2 - 85 && top < mapH_ / 2 + 85 && top + h > mapH_ / 2 - 85) return;
    for (uint8_t i = 0; i < placedCount_; ++i) {
      const auto& b = placed_[i];
      if (left < b[0] + b[2] + 8 && left + w + 8 > b[0] && top < b[1] + b[3] + 6 && top + h + 6 > b[1]) return;
    }
    auto& box = placed_[placedCount_++];
    box[0] = left;
    box[1] = top;
    box[2] = w;
    box[3] = h;
    fillRectLog(frameBuffer_, g_, mapX_ + left, mapY_ + top, w, h, false);
    if (styled_)
      drawNormalText(frameBuffer_, g_, mapX_ + left + 4, mapY_ + top + 4, visible, navFont18);
    else
      drawText(frameBuffer_, g_, mapX_ + left + 4, mapY_ + top + 3, visible, 2, true);
  }

 private:
  // 96 bytes of collision boxes, bounded independently of map density.
  int16_t placed_[12][4]{};
  uint8_t placedCount_ = 0;
  // RouteMapRenderer already guarantees disc/ring containment; this defensive
  // clamp keeps every circle primitive inside the map rectangle even if a
  // future caller violates the RouteCanvas contract. -1 = center outside.
  int clampedRadius(int centerX, int centerY, int radiusPx) const {
    if (centerX < 0 || centerY < 0 || centerX >= mapW_ || centerY >= mapH_ || radiusPx < 0) {
      return -1;
    }
    const int fit = std::min(std::min(centerX, mapW_ - 1 - centerX), std::min(centerY, mapH_ - 1 - centerY));
    return std::min(radiusPx, fit);
  }

  uint8_t* frameBuffer_;
  const Layout& g_;
  int mapX_;
  int mapY_;
  int mapW_;
  int mapH_;
  int minStroke_;
  bool styled_;
};

}  // namespace

uint16_t NavScreenRenderer::remainingMinutes(uint16_t estimatedMinutes, uint32_t remainingMeters,
                                             uint32_t totalMeters) {
  if (totalMeters == 0) return 0;
  const uint64_t scaled = (uint64_t(remainingMeters) * estimatedMinutes + totalMeters / 2) / totalMeters;
  const uint64_t clamped = scaled < uint64_t(estimatedMinutes) ? scaled : uint64_t(estimatedMinutes);
  return static_cast<uint16_t>(clamped);
}

NavFooterMetrics NavScreenRenderer::chooseFooterMetrics(const CurrentPosition* position,
                                                        const RouteProximity& proximity, const RouteIndex& route) {
  NavFooterMetrics metrics;
  metrics.distanceMeters = route.totalDistanceMeters;
  metrics.minutes = route.estimatedMinutes;
  // Without a fix, or when the route declares no total, only the whole-route
  // metrics are meaningful. A fix that is far enough from the route to draw
  // the off-route footer estimate is also too far for its projection along
  // the route to describe where the walk actually is: keep the totals.
  if (position == nullptr || !proximity.valid || route.totalDistanceMeters == 0) return metrics;
  if (proximity.distanceMeters > std::max<uint32_t>(40, 2u * position->accuracyMeters)) return metrics;
  metrics.mode = NavMetricMode::Remaining;
  metrics.distanceMeters = std::min(proximity.remainingDistanceMeters, route.totalDistanceMeters);
  metrics.minutes = remainingMinutes(route.estimatedMinutes, metrics.distanceMeters, route.totalDistanceMeters);
  return metrics;
}

bool NavScreenRenderer::hasReachedRouteEnd(const CurrentPosition* position, const RouteProximity& proximity,
                                           const RouteIndex& route) {
  // Without a live fix, a valid along-route proximity result or a declared
  // positive total there is nothing trustworthy to declare arrival from.
  if (position == nullptr || !proximity.valid || route.totalDistanceMeters == 0) return false;
  // A fix that is far enough from the route to distrust its along-route
  // projection cannot confirm the end either: reuse the off-route guard.
  if (proximity.distanceMeters > std::max<uint32_t>(40, 2u * position->accuracyMeters)) return false;
  // A coarse fix cannot prove the walk is at the end rather than still one
  // accuracy-sized step before it.
  if (position->accuracyMeters > kMaxArrivalAccuracyMeters) return false;
  // The fix may read up to roughly its reported accuracy short of the end
  // while the walker is actually there, so arrival is declared only while the
  // along-route remaining is inside a band of at most one accuracy step
  // (floored so the pixel-quantized remaining can reach it at the true end).
  // A looping route whose endpoint passes geographically near an earlier fix
  // still reports its full remaining distance here and never declares.
  const uint32_t arrivalBand = std::max<uint32_t>(kMinArrivalRemainingMeters, position->accuracyMeters);
  return proximity.remainingDistanceMeters <= arrivalBand;
}

void NavScreenRenderer::drawMessage(uint8_t* frameBuffer, uint16_t widthPx, uint16_t heightPx, const char* title,
                                    const char* detail) {
  if (!frameBuffer || !widthPx || !heightPx) return;
  const Layout g = makeLayout(widthPx, heightPx);
  std::memset(frameBuffer, kWhite, static_cast<size_t>((widthPx + 7) / 8) * heightPx);
  drawOuterBorder(frameBuffer, g);
  const char* lines[] = {title ? title : "", detail ? detail : "", "TERUG - READER"};
  const int ys[] = {g.LH / 3, g.LH / 2, g.LH - 50};
  for (int i = 0; i < 3; ++i) {
    const int width = textMasterWidth(lines[i]);
    const int scale = clamp((g.LW - 32) / (width ? width : 1), 1, i == 0 ? 4 : 3);
    drawText(frameBuffer, g, (g.LW - width * scale) / 2, ys[i], lines[i], scale, true);
  }
}

bool NavScreenRenderer::drawOverview(uint8_t* frameBuffer, uint16_t widthPx, uint16_t heightPx, RouteByteSource& source,
                                     const RouteIndex& index, WalkMapLayer* background, const CurrentPosition* position,
                                     const char* statusText, const char* routeDistanceTitle, GrayMapLayer* gray,
                                     NavGrayPlane plane, const NavMapText* mapText) {
  if (!frameBuffer || widthPx < 160 || heightPx < 120) return false;
  Layout g = makeLayout(widthPx, heightPx);
  g.plane = plane;
  std::memset(frameBuffer, plane == NavGrayPlane::Base ? kWhite : kBlack,
              static_cast<size_t>((widthPx + 7) / 8) * heightPx);
  drawOuterBorder(frameBuffer, g);
  if (!gray) drawText(frameBuffer, g, 20, 24, "ROUTE OP X3", 4, true);
  char name[65]{};
  const uint32_t count = std::min<uint32_t>(index.routeNameLength, sizeof(name) - 1);
  if (count && source.read(index.routeNameOffset, reinterpret_cast<uint8_t*>(name), count) != count) return false;
  // Existing tiny bitmap font is ASCII; unsupported UTF-8 bytes are blanks.
  // Limit the visible name to the width of the card; no unbounded strings.
  for (uint32_t i = 0; i < count; ++i) {
    if (static_cast<unsigned char>(name[i]) < 32) name[i] = ' ';
  }
  while ((gray ? normalTextWidth(navFont26, name) : textMasterWidth(name) * 3) > g.LW - 40 && name[0])
    name[std::strlen(name) - 1] = '\0';
  if (gray)
    drawNormalText(frameBuffer, g, (g.LW - normalTextWidth(navFont26, name)) / 2, 24, name, navFont26, true);
  else
    drawText(frameBuffer, g, 20, 72, name, 3, true);
  const int mapY = gray ? 64 : 120, mapHeight = g.LH - mapY - (gray ? 144 : 100);
  if (mapHeight <= 0) return false;
  MapRouteCanvas canvas(frameBuffer, g, 16, mapY, g.LW - 32, mapHeight, 5, gray != nullptr);
  const Rect mapRect{0, 0, g.LW - 32, mapHeight};
  const auto viewport = position ? RouteViewport::centered(position->point, mapRect, 400, 24)
                                 : RouteViewport::fitOverview(index, mapRect, 24);
  RouteProximity proximity;
  if (!viewport.isValid() || RouteMapRenderer::draw(canvas, source, index, viewport, position, background, &proximity,
                                                    gray) != RenderStatus::Ok)
    return false;
  if (position) {
    // Scale and north stay legible on a white panel over dense geometry.
    const int scaleWidth = viewport.pixelsForMeters(100), sx = g.LW - 32 - scaleWidth, sy = mapY + mapHeight - 16;
    fillRectLog(frameBuffer, g, sx - 6, sy - 24, scaleWidth + 12, 32, false);
    fillRectLog(frameBuffer, g, sx, sy, scaleWidth, 2, true);
    fillRectLog(frameBuffer, g, sx, sy - 5, 2, 7, true);
    fillRectLog(frameBuffer, g, sx + scaleWidth - 2, sy - 5, 2, 7, true);
    if (gray)
      drawNormalText(frameBuffer, g, sx, sy - 22, "100 m", navFont18);
    else
      drawText(frameBuffer, g, sx, sy - 22, "100 M", 2, true);
    fillRectLog(frameBuffer, g, g.LW - 48, mapY + 4, 28, 36, false);
    if (gray)
      drawNormalText(frameBuffer, g, g.LW - 43, mapY + 6, "N", navFont18);
    else
      drawText(frameBuffer, g, g.LW - 43, mapY + 6, "N", 2, true);
    fillHeadVLog(frameBuffer, g, g.LW - 36, mapY + 22, mapY + 34, 6, true);
  }
  if (gray) {
    if (gray->status == WalkMapStatus::Ok)
      drawNormalText(frameBuffer, g, 18, g.LH - 138, "(c) OpenStreetMap contributors", navFont14);
    fillRectLog(frameBuffer, g, 16, g.LH - 116, g.LW - 32, 1, true);
    fillRectLog(frameBuffer, g, g.LW / 2, g.LH - 106, 1, 62, true);
    // A live fix that is close enough to the route swaps the two metric
    // columns from the whole-route totals to the distance and minutes still
    // to walk; a fix that is absent or off-route keeps the totals (and the
    // off-route footer estimate below keeps its existing behavior).
    const NavFooterMetrics footerMetrics = chooseFooterMetrics(position, proximity, index);
    const bool remainingMetrics = footerMetrics.mode == NavMetricMode::Remaining;
    drawNormalText(frameBuffer, g, 22, g.LH - 103,
                   mapText ? (remainingMetrics ? mapText->remainingRoute : mapText->totalRoute) : nullptr, navFont14);
    drawNormalText(frameBuffer, g, g.LW / 2 + 18, g.LH - 103,
                   mapText ? (remainingMetrics ? mapText->remainingDuration : mapText->duration) : nullptr, navFont14);
    char value[32];
    formatDistanceMeters(value, footerMetrics.distanceMeters);
    drawNormalText(frameBuffer, g, 22, g.LH - 80, value, navFont34, true);
    const int n = writeUint(value, footerMetrics.minutes);
    std::memcpy(value + n, " MIN", 5);
    drawNormalText(frameBuffer, g, g.LW / 2 + 18, g.LH - 80, value, navFont34, true);
    if (position && routeDistanceTitle && proximity.valid &&
        proximity.distanceMeters > std::max<uint32_t>(40, 2u * position->accuracyMeters)) {
      char estimate[48];
      const size_t prefix = std::min<size_t>(std::strlen(routeDistanceTitle), 18);
      std::memcpy(estimate, routeDistanceTitle, prefix);
      estimate[prefix] = ' ';
      formatDistanceMeters(estimate + prefix + 1, ((proximity.distanceMeters + 5) / 10) * 10);
      drawNormalText(frameBuffer, g, 22, g.LH - 43, estimate, navFont18);
    }
    // A live fix that has reliably reached the route end swaps the bottom
    // footer status for the localized arrival message. A missing, off-route or
    // still-walking fix keeps the caller's status text exactly as before.
    const char* footerStatus = statusText;
    if (hasReachedRouteEnd(position, proximity, index) && mapText && mapText->arrivedStatus) {
      footerStatus = mapText->arrivedStatus;
    }
    drawNormalText(frameBuffer, g, 22, g.LH - 21, footerStatus, navFont14);
    return true;
  }
  if ((gray && gray->status == WalkMapStatus::Ok) ||
      (background && background->status == WalkMapStatus::Ok && background->edgeCount)) {
    drawText(frameBuffer, g, 20, g.LH - 98, "(C) OPENSTREETMAP CONTRIBUTORS", 1, true);
    drawText(frameBuffer, g, 20, g.LH - 89, "OPENSTREETMAP.ORG/COPYRIGHT", 1, true);
  }
  char metrics[40];
  int n = formatDistanceMeters(metrics, index.totalDistanceMeters);
  metrics[n++] = ' ';
  metrics[n++] = ' ';
  n += writeUint(metrics + n, index.estimatedMinutes);
  std::memcpy(metrics + n, " MIN", 5);
  if (position && routeDistanceTitle && proximity.valid &&
      proximity.distanceMeters > std::max<uint32_t>(40, 2u * position->accuracyMeters)) {
    const size_t prefix = std::min<size_t>(std::strlen(routeDistanceTitle), 18);
    std::memcpy(metrics, routeDistanceTitle, prefix);
    metrics[prefix] = ' ';
    // Round away false precision: straight-line estimate, not a walking route.
    formatDistanceMeters(metrics + prefix + 1, ((proximity.distanceMeters + 5) / 10) * 10);
  }
  drawText(frameBuffer, g, 20, g.LH - 80, metrics, 3, true);
  drawText(frameBuffer, g, 20, g.LH - 40, statusText ? statusText : "GPS NOG NIET ACTIEF", 3, true);
  return true;
}

void NavScreenRenderer::draw(uint8_t* frameBuffer, uint16_t widthPx, uint16_t heightPx, const NavState& state) {
  if (frameBuffer == nullptr || widthPx == 0 || heightPx == 0) {
    return;
  }

  const Layout g = makeLayout(widthPx, heightPx);
  const int widthBytes = (widthPx + 7) / 8;
  std::memset(frameBuffer, kWhite, static_cast<size_t>(widthBytes) * static_cast<size_t>(heightPx));

  // ---- 1. Thin black outer border ----
  drawOuterBorder(frameBuffer, g);

  // ---- 2. Maneuver / status band ----
  if (state.status == NavStatus::Navigating) {
    drawNavigatingBand(frameBuffer, g, state);
  } else {
    drawStatusBand(frameBuffer, g, state.status);
  }

  // ---- 3. Thin separator ----
  fillRectLog(frameBuffer, g, g.border, g.sepY, g.LW - 2 * g.border, g.sepThick, true);

  // ---- 4. Map area ----
  drawMapArea(frameBuffer, g, state.status);

  // ---- 5. Bottom status line, centered ----
  char statusLine[40];
  int n = formatDistanceMeters(statusLine, state.remainingDistanceMeters);
  statusLine[n++] = ' ';
  statusLine[n++] = ' ';
  n += writeUint(statusLine + n, state.remainingMinutes);
  statusLine[n++] = ' ';
  statusLine[n++] = 'M';
  statusLine[n++] = 'I';
  statusLine[n++] = 'N';
  statusLine[n] = '\0';
  const int statusX = (g.LW - textMasterWidth(statusLine) * g.statusScale) / 2;
  drawText(frameBuffer, g, statusX, g.statusY, statusLine, g.statusScale, true);
}

void NavScreenRenderer::draw(uint8_t* frameBuffer, uint16_t widthPx, uint16_t heightPx, const NavState& state,
                             RouteByteSource* routeSource, const RouteIndex* routeIndex,
                             const CurrentPosition* position) {
  // Base UI first (the exact legacy output: border, maneuver/status band,
  // separator, schematic map and bottom status line).
  draw(frameBuffer, widthPx, heightPx, state);
  if (frameBuffer == nullptr || widthPx == 0 || heightPx == 0) {
    return;
  }

  // A null/incomplete route or a non-navigating status screen keeps the exact
  // legacy screen: the schematic map stays and the status band stays dominant.
  if (routeSource == nullptr || routeIndex == nullptr || state.status != NavStatus::Navigating) {
    return;
  }

  const Layout g = makeLayout(widthPx, heightPx);
  const int mapX = g.border;
  const int mapY = g.mapTop;
  const int mapW = g.LW - 2 * g.border;
  const int mapH = g.mapH;
  if (mapW < 1 || mapH < 1) {
    return;
  }

  // The logical map rectangle sits inside the border and above the bottom
  // status text. fitOverview fits the validated route's overview into it with
  // equal safe padding on every side (the viewport's default).
  const Rect mapRect{0, 0, mapW, mapH};
  const RouteViewport viewport = RouteViewport::fitOverview(*routeIndex, mapRect, RouteViewport::kDefaultPaddingPx);
  if (!viewport.isValid()) {
    return;  // Unusable route/overview: keep the schematic map.
  }

  // Replace only the schematic map: the adapter clears the logical map
  // rectangle and RouteMapRenderer streams the detailed geometry, drawing the
  // current-position marker last.
  MapRouteCanvas canvas(frameBuffer, g, mapX, mapY, mapW, mapH);
  const RenderStatus status = RouteMapRenderer::draw(canvas, *routeSource, *routeIndex, viewport, position);
  if (status != RenderStatus::Ok) {
    // Fail visually safely without a second framebuffer: RouteMapRenderer
    // cleared the map rectangle first, so wipe it again and repaint the legacy
    // schematic map rather than leaving a partially drawn real route.
    fillRectLog(frameBuffer, g, mapX, mapY, mapW, mapH, false);
    drawMapArea(frameBuffer, g, state.status);
  }
}

}  // namespace navigator
