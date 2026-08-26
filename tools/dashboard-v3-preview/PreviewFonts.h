#pragma once

// Host-side text rendering with the real built-in Lexend Deca bitmap fonts.
//
// The dashboard V3 renderer draws through an abstract canvas, so the host can
// substitute this rasterizer for GfxRenderer. The point is typography: the
// firmware's own font data decides how wide "NOG 27 MIN DROOG" really is and
// when truncatedText() starts eating characters, and no test glyph table can
// answer that. Everything here mirrors the GfxRenderer path the X3 uses for
// plain left-to-right Latin text; bidi, small caps, SD-card fonts, combining
// marks and sub/superscript are deliberately absent because dashboard V3 never
// draws them.

#include <EpdFontFamily.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dashboard::preview {

// A 1-bit framebuffer with the same "true means black ink" convention the
// firmware renderer uses. Pixels outside the buffer are dropped rather than
// wrapped, which is how a clipped layout shows up as missing ink instead of
// garbage on the opposite edge.
class Framebuffer {
 public:
  Framebuffer(int width, int height);

  int width() const { return width_; }
  int height() const { return height_; }

  void clear(bool black);
  void setPixel(int x, int y, bool black);
  bool pixel(int x, int y) const;
  void fillRect(int x, int y, int width, int height, bool black);
  void drawRect(int x, int y, int width, int height, bool black);
  void drawLine(int x1, int y1, int x2, int y2, bool black);

  /// Writes the buffer as an 8-bit greyscale PNG (black ink on white).
  bool writePng(const std::string& path) const;

 private:
  int width_;
  int height_;
  std::vector<uint8_t> pixels_;  // one byte per pixel; 1 = black
};

/// The built-in Lexend families the dashboard uses, keyed by the same font ids
/// `fontIds.h` defines so the preview and the firmware agree on what "Lexend
/// 14 bold" means.
class FontBook {
 public:
  FontBook();

  const EpdFontFamily* family(int fontId) const;

  /// Mirrors GfxRenderer::getTextWidth for built-in fonts: the family's own
  /// getTextDimensions, kerning and differential rounding included.
  int textWidth(int fontId, const char* text, EpdFontFamily::Style style) const;

  /// Mirrors GfxRenderer::getFontAscenderSize: drawText treats y as the top of
  /// the ascender box, not the baseline.
  int ascender(int fontId) const;

  /// Mirrors GfxRenderer::getLineHeight.
  int lineHeight(int fontId) const;

  /// Byte-for-byte the same ellipsis logic as GfxRenderer::truncatedText,
  /// including the U+2026 that a subsetted font cannot render.
  std::string truncated(int fontId, const char* text, int maxWidth, EpdFontFamily::Style style) const;

  /// Draws `text` with its ascender box top at `y`, mirroring
  /// GfxRenderer::drawText for plain Latin text.
  void drawText(Framebuffer& canvas, int fontId, int x, int y, const char* text, bool black,
                EpdFontFamily::Style style) const;

 private:
  struct Entry {
    int fontId;
    const EpdFontFamily* family;
  };

  std::vector<Entry> entries_;
};

/// True when `text` contains a codepoint the given font cannot render, which
/// the firmware would draw as a U+FFFD replacement box. Subsetted fonts (the
/// bold "dash" ladder) only carry ASCII plus the degree sign, so an ellipsis or
/// an em dash silently becomes a black lozenge on the panel.
bool hasMissingGlyph(const FontBook& fonts, int fontId, const char* text, EpdFontFamily::Style style);

}  // namespace dashboard::preview
