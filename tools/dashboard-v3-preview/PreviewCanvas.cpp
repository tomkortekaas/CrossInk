#include "PreviewCanvas.h"

#include "components/icons/dashboardIconTable.h"
#include "fontIds.h"

namespace dashboard::preview {
namespace {

int fontIdFor(const dashboard::v3::FontRole role) {
  switch (role) {
    // Must stay identical to fontId() in DashboardV3Renderer.cpp: a preview that
    // resolves a rung differently from the firmware measures the wrong font.
    case dashboard::v3::FontRole::Micro: return LEXENDDECA_8_FONT_ID;
    case dashboard::v3::FontRole::Small: return LEXENDDECA_9_FONT_ID;
    case dashboard::v3::FontRole::Body: return LEXENDDECA_10_FONT_ID;
    case dashboard::v3::FontRole::Heading: return LEXENDDECA_12_FONT_ID;
    case dashboard::v3::FontRole::Value: return LEXENDDECA_14_FONT_ID;
    case dashboard::v3::FontRole::Hero: return LEXENDDECA_16_FONT_ID;
  }
  return LEXENDDECA_10_FONT_ID;
}

// Dashboard icons are 1-bpp with inverted ink: a zero bit is the drawn pixel.
// Same convention as drawNaturalIcon in the firmware renderer.
void drawIcon(Framebuffer& canvas, const freeink::Icon& icon, const dashboard::v3::Rect bounds, const bool black) {
  const int stride = (icon.w + 7) / 8;
  const int x = bounds.x + (bounds.width - icon.w) / 2;
  const int y = bounds.y + (bounds.height - icon.h) / 2;
  for (int row = 0; row < icon.h; ++row) {
    const uint8_t* source = icon.bits + row * stride;
    for (int column = 0; column < icon.w; ++column) {
      if ((source[column >> 3] & static_cast<uint8_t>(0x80U >> (column & 7))) == 0) {
        canvas.setPixel(x + column, y + row, black);
      }
    }
  }
}

}  // namespace

PreviewCanvas::PreviewCanvas(Framebuffer& framebuffer, const FontBook& fonts)
    : framebuffer_(framebuffer), fonts_(fonts) {}

int PreviewCanvas::width() const { return framebuffer_.width(); }

int PreviewCanvas::height() const { return framebuffer_.height(); }

void PreviewCanvas::fill(const dashboard::v3::Rect rect, const bool black) {
  framebuffer_.fillRect(rect.x, rect.y, rect.width, rect.height, black);
}

void PreviewCanvas::line(const int x1, const int y1, const int x2, const int y2, const bool black) {
  framebuffer_.drawLine(x1, y1, x2, y2, black);
}

void PreviewCanvas::rect(const dashboard::v3::Rect rect, const bool black) {
  framebuffer_.drawRect(rect.x, rect.y, rect.width, rect.height, black);
}

void PreviewCanvas::text(const dashboard::v3::TextSpec& spec, const char* value) {
  const int fontId = fontIdFor(spec.font);
  const auto style = spec.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const std::string bounded = fonts_.truncated(fontId, value, spec.bounds.width, style);
  const int drawnWidth = fonts_.textWidth(fontId, bounded.c_str(), style);

  int x = spec.bounds.x;
  if (spec.align == dashboard::v3::TextAlign::Center) x += (spec.bounds.width - drawnWidth) / 2;
  if (spec.align == dashboard::v3::TextAlign::Right) x += spec.bounds.width - drawnWidth;
  fonts_.drawText(framebuffer_, fontId, x, spec.bounds.y, bounded.c_str(), spec.black, style);

  TextObservation observation;
  observation.requested = value == nullptr ? "" : value;
  observation.drawn = bounded;
  observation.fontId = fontId;
  observation.boundsWidth = spec.bounds.width;
  observation.measuredWidth = fonts_.textWidth(fontId, observation.requested.c_str(), style);
  observation.bold = spec.bold;
  observation.truncated = bounded != observation.requested;
  observation.missingGlyph = hasMissingGlyph(fonts_, fontId, bounded.c_str(), style);
  observations_.push_back(std::move(observation));
}

void PreviewCanvas::shade(const dashboard::v3::Rect bounds, const dashboard::v3::Shade level) {
  if (level == dashboard::v3::Shade::None) return;
  for (int y = bounds.y; y < bounds.y + bounds.height; ++y) {
    for (int x = bounds.x; x < bounds.x + bounds.width; ++x) {
      if (dashboard::v3::shadeCoversPixel(level, x, y)) framebuffer_.setPixel(x, y, true);
    }
  }
}

void PreviewCanvas::icon(const uint8_t iconId, const dashboard::v3::Rect bounds, const bool black) {
  if (iconId == 0 || iconId > dashboard::DASHBOARD_ICON_COUNT) return;
  const freeink::Icon* selected = bounds.width >= 40 && bounds.height >= 40 ? dashboard::DASHBOARD_ICONS_48[iconId]
                                                                           : dashboard::DASHBOARD_ICONS_32[iconId];
  if (selected != nullptr) drawIcon(framebuffer_, *selected, bounds, black);
}

}  // namespace dashboard::preview
