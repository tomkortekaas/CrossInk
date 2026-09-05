#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "NavigationRefreshPolicy.h"
using namespace navigator;
int main() {
  NavigationRefreshPolicy p;
  LivePosition a{523700000, 49000000, 5, false}, b{523720000, 49000000, 5, false};
  assert(p.decide(100, &a, false, false) == NavigationRefresh::None);
  assert(p.decide(100, &a, true, false) == NavigationRefresh::Full);
  p.rendered(100, &a, NavigationRefresh::Full);
  for (int i = 1; i < 50; ++i) assert(p.decide(100 + i * 1000, &b, false, false) == NavigationRefresh::None);
  p.setActiveView(true);
  assert(p.decide(5000, &b, false, false) == NavigationRefresh::None);
  assert(p.decide(10100, &b, false, false) == NavigationRefresh::None);
  assert(p.decide(30100, &b, false, false) == NavigationRefresh::Fast);
  p.rendered(30100, &b, NavigationRefresh::Fast);
  assert(p.decide(60000, &b, false, false) == NavigationRefresh::None);
  assert(p.decide(60100, nullptr, false, false) == NavigationRefresh::Full);
  p.rendered(60100, nullptr, NavigationRefresh::Full);
  assert(p.decide(60101, &a, false, false) == NavigationRefresh::Full);  // first fresh fix is immediate
  p.rendered(60101, &a, NavigationRefresh::Full);
  LivePosition off = a;
  off.offRoute = true;
  assert(p.decide(60102, &off, false, false) == NavigationRefresh::Full);  // safety state bypasses timer
  assert(p.decide(60102, &a, true, true) == NavigationRefresh::Full);
  for (int i = 0; i < 20; ++i) p.rendered(50000 + i, &a, NavigationRefresh::Fast);
  assert(p.decide(60000, &b, false, false) == NavigationRefresh::None);  // ordinary fix stays throttled
  assert(p.decide(60000, &b, true, false) == NavigationRefresh::Full);
  // A forced (manual) refresh bypasses the 30-second wait even without real
  // movement; an ordinary fix in the same window remains throttled.
  NavigationRefreshPolicy forced;
  forced.setActiveView(true);
  LivePosition near{523700000, 49000000, 5, false};
  assert(forced.decide(1000, &near, false, false) == NavigationRefresh::Full);  // first fix is immediate
  forced.rendered(1000, &near, NavigationRefresh::Full);
  LivePosition moved = near;
  moved.latitudeE7 += 20000;
  assert(forced.decide(5000, &moved, false, false) == NavigationRefresh::None);  // throttled
  assert(forced.decide(5000, &moved, true, false) == NavigationRefresh::Fast);   // forced bypasses
  forced.rendered(5000, &moved, NavigationRefresh::Fast);
  assert(forced.decide(5100, &near, false, false) == NavigationRefresh::None);   // still throttled
  assert(forced.decide(5100, &near, true, false) == NavigationRefresh::Fast);    // forced needs no movement
}
