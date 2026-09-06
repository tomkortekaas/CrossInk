#include "NavigationRefreshPolicy.h"

#include "map/RouteViewport.h"
namespace navigator {
uint32_t NavigationRefreshPolicy::routineIntervalMs() const {
  switch (mode_) {
    case WalkingRefreshMode::Fast: return 10000;
    case WalkingRefreshMode::Balanced: return 20000;
    case WalkingRefreshMode::Economical: return 30000;
  }
  return 30000;  // impossible enum values fail closed to the Economical cadence
}
NavigationRefresh NavigationRefreshPolicy::decide(uint32_t now, const LivePosition* p, bool manual,
                                                  bool viewportChanged) const {
  if (!manual && !activeView_) return NavigationRefresh::None;
  if (!hasRendered_ || viewportChanged || (p != nullptr) != hadPosition_) return NavigationRefresh::Full;
  if (!manual && p && hadPosition_ && p->offRoute != last_.offRoute) return NavigationRefresh::Full;
  // E-ink navigation is a glanceable snapshot. Limit routine movement updates
  // to once per the selected walking mode's interval; first-fix and off-route
  // transitions above remain immediate.
  if (!manual && uint32_t(now - renderedAt_) < routineIntervalMs()) return NavigationRefresh::None;
  if (!manual) {
    if (!p) return NavigationRefresh::None;
    const auto view = RouteViewport::centered({last_.latitudeE7, last_.longitudeE7}, {0, 0, 1000, 1000}, 1000, 0);
    const auto a = view.project({last_.latitudeE7, last_.longitudeE7}),
               b = view.project({p->latitudeE7, p->longitudeE7});
    const int64_t dx = int64_t(a.x) - b.x, dy = int64_t(a.y) - b.y;
    if (dx * dx + dy * dy < 400) return NavigationRefresh::None;
  }
  return fastCount_ >= 20 ? NavigationRefresh::Full : NavigationRefresh::Fast;
}
void NavigationRefreshPolicy::rendered(uint32_t now, const LivePosition* p, NavigationRefresh mode) {
  if (mode == NavigationRefresh::None) return;
  hasRendered_ = true;
  renderedAt_ = now;
  hadPosition_ = p != nullptr;
  if (p) last_ = *p;
  if (mode == NavigationRefresh::Full)
    fastCount_ = 0;
  else if (fastCount_ < 20)
    ++fastCount_;
}
}  // namespace navigator
