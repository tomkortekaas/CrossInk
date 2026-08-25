#pragma once

namespace dashboard::v3 {

struct Insets {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  constexpr bool operator==(const Rect& other) const {
    return x == other.x && y == other.y && width == other.width && height == other.height;
  }
};

struct DashboardV3Rects {
  Rect header{};
  Rect rain{};
  Rect traffic{};
  Rect body{};
  Rect footer{};
  Rect bodyLeft{};
  Rect bodyRight{};
};

DashboardV3Rects computeDashboardV3Layout(int width, int height, Insets safe);

}  // namespace dashboard::v3
