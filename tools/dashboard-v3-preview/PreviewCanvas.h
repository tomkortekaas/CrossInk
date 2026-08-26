#pragma once

// The DashboardV3Canvas the host preview draws into: real Lexend metrics, real
// glyphs, real dashboard icons. Every text call is also recorded so the tool can
// report which strings the renderer had to truncate and which ones fall back to
// the U+FFFD replacement box — the two failure modes that are invisible in a
// screenshot until you already know where to look.

#include <string>
#include <vector>

#include "DashboardV3Renderer.h"
#include "PreviewFonts.h"

namespace dashboard::preview {

struct TextObservation {
  std::string requested;   ///< what the renderer asked for
  std::string drawn;       ///< what fitted, ellipsis included
  int fontId = 0;
  int boundsWidth = 0;
  int measuredWidth = 0;   ///< width of `requested` at this font
  bool bold = false;
  bool truncated = false;
  bool missingGlyph = false;  ///< `drawn` contains a codepoint this font lacks
};

class PreviewCanvas final : public dashboard::v3::DashboardV3Canvas {
 public:
  PreviewCanvas(Framebuffer& framebuffer, const FontBook& fonts);

  int width() const override;
  int height() const override;
  void fill(dashboard::v3::Rect rect, bool black) override;
  void line(int x1, int y1, int x2, int y2, bool black) override;
  void rect(dashboard::v3::Rect rect, bool black) override;
  void text(const dashboard::v3::TextSpec& spec, const char* value) override;
  void icon(uint8_t iconId, dashboard::v3::Rect bounds, bool black) override;
  void shade(dashboard::v3::Rect bounds, dashboard::v3::Shade level) override;

  const std::vector<TextObservation>& observations() const { return observations_; }

 private:
  Framebuffer& framebuffer_;
  const FontBook& fonts_;
  std::vector<TextObservation> observations_;
};

}  // namespace dashboard::preview
