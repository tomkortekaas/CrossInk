#pragma once

#include <cstdint>

#include "DashboardV3.h"
#include "DashboardV3Layout.h"

class GfxRenderer;

namespace dashboard::v3 {

enum class FontRole : uint8_t {
  Utility10,
  Utility12,
  Heading14,
  Heading16,
  Value18,
  Value22,
  Value28,
  Value34,
};

enum class TextAlign : uint8_t { Left, Center, Right };

struct TextSpec {
  Rect bounds{};
  FontRole font = FontRole::Utility10;
  TextAlign align = TextAlign::Left;
  bool bold = false;
  bool black = true;
};

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
};

void renderDashboardV3(DashboardV3Canvas& canvas, const DashboardV3Package& package, uint16_t minuteOfDay);
void renderDashboardV3(GfxRenderer& renderer, const DashboardV3Package& package, uint16_t minuteOfDay);

}  // namespace dashboard::v3
