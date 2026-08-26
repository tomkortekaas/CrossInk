#pragma once

#include <cstdint>

#include "DashboardV3.h"
#include "DashboardV3Layout.h"

class GfxRenderer;

namespace dashboard::v3 {

// The rungs V3 draws on, named for their job rather than their nominal font
// size. The nominal sizes are misleading: the built-in "Lexend 10" face has a
// 21 px ascender and a 26 px line height, so a ladder chosen by name puts text
// at roughly twice the intended size and every box overflows. The comment on
// each rung records the measured ascender and line height on the 528x792 panel;
// tools/dashboard-v3-preview prints the same table.
//
// All six map to full Lexend faces. The subsetted "dash" faces cover only ASCII
// plus the degree sign, which turns the U+2026 that truncatedText() appends into
// a replacement box on the panel.
enum class FontRole : uint8_t {
  Micro,    ///< Lexend 8  - ascender 17, line 21: ALL-CAPS captions, units, timestamps
  Small,    ///< Lexend 9  - ascender 19, line 23: dense list rows
  Body,     ///< Lexend 10 - ascender 21, line 26: agenda titles, chat names, quote
  Heading,  ///< Lexend 12 - ascender 25, line 31: section headings, header values
  Value,    ///< Lexend 14 - ascender 30, line 36: step count
  Hero,     ///< Lexend 16 - ascender 34, line 42: travel minutes, congestion km
};

enum class TextAlign : uint8_t { Left, Center, Right };

struct TextSpec {
  Rect bounds{};
  FontRole font = FontRole::Body;
  TextAlign align = TextAlign::Left;
  bool bold = false;
  bool black = true;
};

/// Ink coverage for an area that has to read as a shade rather than a solid.
/// The panel is one bit deep, so the shades are ordered dither patterns; the
/// mock-up's four rain levels map onto None/Quarter/Half/Solid.
enum class Shade : uint8_t {
  None,     ///< paper
  Quarter,  ///< every other pixel on every other row
  Half,     ///< checkerboard
  Solid,    ///< full ink
};

/// Whether a shade puts ink on this canvas pixel. Every canvas shares this so
/// the preview, the host tests and the panel agree on the same pattern.
constexpr bool shadeCoversPixel(const Shade level, const int x, const int y) {
  switch (level) {
    case Shade::None: return false;
    case Shade::Quarter: return (x % 2 == 0) && (y % 2 == 0);
    case Shade::Half: return (x + y) % 2 == 0;
    case Shade::Solid: return true;
  }
  return false;
}

class DashboardV3Canvas {
 public:
  virtual ~DashboardV3Canvas() = default;
  virtual int width() const = 0;
  virtual int height() const = 0;
  virtual void fill(Rect rect, bool black) = 0;
  virtual void line(int x1, int y1, int x2, int y2, bool black) = 0;
  virtual void rect(Rect rect, bool black) = 0;
  virtual void text(const TextSpec& spec, const char* value) = 0;
  virtual void icon(uint8_t iconId, Rect bounds, bool black) = 0;
  /// Fills `bounds` at the given coverage. The dither origin is the canvas, not
  /// the rectangle, so neighbouring shaded blocks line up instead of showing a
  /// seam where their patterns fall out of phase.
  virtual void shade(Rect bounds, Shade level) = 0;
};

/// `utcOffsetQ` is `SETTINGS.clockUtcOffsetQ`: quarter hours biased by 48, so
/// 48 is UTC and 56 is +2h. The package timestamp is UTC epoch seconds and the
/// wire carries no timezone, so without this the header date and the "ververst"
/// time are drawn in UTC while the clock beside them is local. The default
/// keeps host tests on UTC, where their fixtures are written.
constexpr uint8_t UTC_OFFSET_Q_UTC = 48;

void renderDashboardV3(DashboardV3Canvas& canvas, const DashboardV3Package& package, uint16_t minuteOfDay,
                       uint8_t utcOffsetQ = UTC_OFFSET_Q_UTC);
void renderDashboardV3(GfxRenderer& renderer, const DashboardV3Package& package, uint16_t minuteOfDay,
                       uint8_t utcOffsetQ = UTC_OFFSET_Q_UTC);
void applyDashboardV3DeviceBattery(DashboardV3Package& package, uint16_t percentage);

}  // namespace dashboard::v3
