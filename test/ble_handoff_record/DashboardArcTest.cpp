#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "DashboardArc.h"

namespace {

struct Plotted {
  std::vector<std::pair<int, int>> points;
};

void collect(int x, int y, void* ctx) {
  static_cast<Plotted*>(ctx)->points.emplace_back(x, y);
}

bool hasPointNear(const Plotted& plotted, int x, int y, int tolerance) {
  for (const auto& [px, py] : plotted.points) {
    if (std::abs(px - x) <= tolerance && std::abs(py - y) <= tolerance) return true;
  }
  return false;
}

TEST(DashboardArc, ZeroSweepDrawsNothing) {
  Plotted plotted;
  dashboard::forEachArcPixel(100, 100, 40, 10, 135, 0, collect, &plotted);
  EXPECT_TRUE(plotted.points.empty());
}

// Een volle cirkel raakt ook de onderkant; de meterboog juist niet. Dat verschil
// is precies wat deze twee tests samen vastleggen.
TEST(DashboardArc, FullSweepReachesTheBottom) {
  Plotted plotted;
  dashboard::forEachArcPixel(100, 100, 40, 10, 0, 360, collect, &plotted);
  EXPECT_TRUE(hasPointNear(plotted, 100, 140, 3));
}

TEST(DashboardArc, GaugeSweepLeavesTheBottomOpen) {
  Plotted plotted;
  dashboard::forEachArcPixel(100, 100, 40, 10, 135, 270, collect, &plotted);
  // boven wordt wel geraakt
  EXPECT_TRUE(hasPointNear(plotted, 100, 60, 3));
  // recht onder het middelpunt blijft de opening
  EXPECT_FALSE(hasPointNear(plotted, 100, 140, 3));
}

// Elke pixel moet in de band tussen binnen- en buitenstraal liggen. Zonder deze
// test kan een fout in de dikte er op het paneel uitzien als een dikke of
// rafelige boog zonder dat iets faalt.
TEST(DashboardArc, EveryPixelSitsInsideTheBand) {
  Plotted plotted;
  const int radius = 40;
  const int thickness = 10;
  dashboard::forEachArcPixel(100, 100, radius, thickness, 135, 270, collect, &plotted);
  ASSERT_FALSE(plotted.points.empty());
  for (const auto& [px, py] : plotted.points) {
    const double distance = std::hypot(px - 100.0, py - 100.0);
    EXPECT_GE(distance, radius - thickness / 2.0 - 1.5);
    EXPECT_LE(distance, radius + thickness / 2.0 + 1.5);
  }
}

TEST(DashboardArc, FillMapsOntoTheSweep) {
  EXPECT_EQ(dashboard::arcSweepForFill(0, 270), 0);
  EXPECT_EQ(dashboard::arcSweepForFill(100, 270), 270);
  EXPECT_EQ(dashboard::arcSweepForFill(50, 270), 135);
}

// Een vulling boven 100 mag de boog niet voorbij zijn eindpunt laten lopen.
TEST(DashboardArc, FillIsClampedToTheFullSweep) {
  EXPECT_EQ(dashboard::arcSweepForFill(200, 270), 270);
}

}  // namespace
