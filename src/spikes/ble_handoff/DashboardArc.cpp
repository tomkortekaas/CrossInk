#include "DashboardArc.h"

#include <algorithm>
#include <cmath>

namespace dashboard {
namespace {

constexpr double DEGREES_TO_RADIANS = 3.14159265358979323846 / 180.0;

// De hoek van (dx, dy) ten opzichte van het middelpunt, genormaliseerd naar
// 0..360 in hetzelfde stelsel als de doc-comment: y naar beneden, met de klok mee.
double angleOf(const double dx, const double dy) {
  double degrees = std::atan2(dy, dx) / DEGREES_TO_RADIANS;
  if (degrees < 0.0) degrees += 360.0;
  return degrees;
}

}  // namespace

int arcSweepForFill(const uint8_t fill, const int fullSweepDegrees) {
  const int bounded = std::min<int>(fill, 100);
  return fullSweepDegrees * bounded / 100;
}

void forEachArcPixel(const int centerX, const int centerY, const int radius, const int thickness,
                     const int startDegrees, const int sweepDegrees,
                     void (*plot)(int x, int y, void* context), void* const context) {
  if (plot == nullptr || sweepDegrees <= 0 || radius <= 0 || thickness <= 0) return;

  const double half = thickness / 2.0;
  const double inner = std::max(0.0, radius - half);
  const double outer = radius + half;
  const int reach = static_cast<int>(std::ceil(outer)) + 1;

  // Een scan over het omhullende vierkant in plaats van langs de boog lopen:
  // langs de boog stappen laat gaten vallen zodra de straal groter wordt, en
  // de gatgrootte hangt dan af van de stapgrootte. Dit is ~4x reach^2 tests op
  // een paneel dat hooguit per minuut hertekent - geen hot path.
  for (int dy = -reach; dy <= reach; ++dy) {
    for (int dx = -reach; dx <= reach; ++dx) {
      const double distance = std::hypot(static_cast<double>(dx), static_cast<double>(dy));
      if (distance < inner || distance > outer) continue;
      // Het middelpunt zelf heeft geen hoek; bij een straal groter dan de halve
      // dikte valt het toch al buiten de band.
      if (distance == 0.0) continue;

      const double angle = angleOf(static_cast<double>(dx), static_cast<double>(dy));
      double offset = angle - static_cast<double>(startDegrees);
      while (offset < 0.0) offset += 360.0;
      while (offset >= 360.0) offset -= 360.0;
      if (offset > static_cast<double>(sweepDegrees)) continue;

      plot(centerX + dx, centerY + dy, context);
    }
  }
}

}  // namespace dashboard
