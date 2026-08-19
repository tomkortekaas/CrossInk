#pragma once

#include <array>

#include "DashboardWidgetGridV2.h"

namespace dashboard {
namespace v2 {

struct WidgetRectV2 {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// Gespiegeld naar computeGridLayout van template 3, met de constanten van
// template 4: 12 kolommen en 12 rijen, dus een cel van 42x64 op het gebruikelijke
// canvas van 510x768. Plaatsing is vrij: elk rechthoekje komt alleen uit de
// eigen column/row/span van het widget, nooit uit de volgorde in de array.
void computeGridLayoutV2(const WidgetGridPackageV2& package, int canvasWidth, int canvasHeight,
                         std::array<WidgetRectV2, MAX_WIDGETS>& rectsOut, int originX, int originY);

}  // namespace v2
}  // namespace dashboard
