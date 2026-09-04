#ifdef NDEBUG
#undef NDEBUG // Standalone acceptance checks must run in Release builds too.
#endif
#include "WalkMapViewport.h"
#include <cassert>
int main() {
  using namespace navigator;
  GeoBounds b{};
  auto v = RouteViewport::centered(GeoPoint{523700000, 49000000},
                                   Rect{0, 0, 480, 600}, 1000);
  assert(walkMapBounds(v, b));
  assert(b.southE7 < 523700000 && b.northE7 > 523700000 &&
         b.westE7 < 49000000 && b.eastE7 > 49000000);
  assert(v.project(GeoPoint{b.southE7, 49000000}).y >= 598);
  assert(v.project(GeoPoint{b.northE7, 49000000}).y <= 1);
  assert(v.project(GeoPoint{523700000, b.westE7}).x <= 1);
  assert(v.project(GeoPoint{523700000, b.eastE7}).x >= 478);
  assert(!walkMapBounds(RouteViewport{}, b));
  auto wrap = RouteViewport::centered(GeoPoint{0, 1799999990},
                                      Rect{0, 0, 480, 600}, 1000);
  assert(!walkMapBounds(wrap, b));
}
