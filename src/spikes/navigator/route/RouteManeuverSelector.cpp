#include "RouteManeuverSelector.h"

namespace navigator {
namespace {

// The validated wire kind byte (0..6) maps 1:1 onto navigator::Maneuver (the
// Route Package v1 decoder contract in RoutePackageV1.h). The explicit switch
// keeps that mapping readable and fails loudly if an enumerator is ever
// removed; validated routes can never carry a kind outside 0..6, so the
// default is unreachable defensive fallback.
Maneuver maneuverFromKind(uint8_t kind) {
  switch (kind) {
    case 0:
      return Maneuver::Straight;
    case 1:
      return Maneuver::Left;
    case 2:
      return Maneuver::Right;
    case 3:
      return Maneuver::SlightLeft;
    case 4:
      return Maneuver::SlightRight;
    case 5:
      return Maneuver::UTurn;
    case 6:
      return Maneuver::Arrive;
    default:
      return Maneuver::Straight;
  }
}

bool isArrival(const RouteIndex::Maneuver& maneuver) { return maneuverFromKind(maneuver.type) == Maneuver::Arrive; }

// distance + kPassHysteresisMeters saturated at UINT32_MAX so a maneuver at
// the very top of the uint32 range is only passed by progress UINT32_MAX
// itself, never by a wrapped-around tiny threshold.
uint32_t passThreshold(uint32_t distanceFromStartMeters) {
  constexpr uint32_t kMaxUint32 = ~uint32_t{0};
  return distanceFromStartMeters > kMaxUint32 - RouteManeuverSelector::kPassHysteresisMeters
             ? kMaxUint32
             : distanceFromStartMeters + RouteManeuverSelector::kPassHysteresisMeters;
}

// Remaining distance from `progress` to `maneuverDistance`, saturated to zero
// once the maneuver is behind and clamped to the UInt16 display range.
uint16_t remainingDistanceMeters(uint32_t maneuverDistanceFromStartMeters, uint32_t progress) {
  if (progress >= maneuverDistanceFromStartMeters) return 0;
  const uint32_t remaining = maneuverDistanceFromStartMeters - progress;
  return remaining > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(remaining);
}

}  // namespace

ManeuverSelection RouteManeuverSelector::update(const RouteIndex& route, uint32_t distanceFromStartMeters,
                                                bool trusted) {
  // Only a validated, maneuver-carrying route can drive a selection. An empty
  // (or implausible) maneuver list clears any previous route's selection so a
  // reused selector never reports a stale index or distance.
  if (route.maneuverCount == 0 || route.maneuverCount > RouteIndex::kMaxManeuvers) {
    reset();
    return ManeuverSelection{};
  }

  // Route identity is the validated route id. A different route id means the
  // caller handed us a different RouteIndex: start a fresh selection instead
  // of carrying an index that may point at another route's maneuver.
  if (!hasSelection_ || routeId_ != route.routeId) {
    routeId_ = route.routeId;
    index_ = 0;
    hasSelection_ = true;
  }
  // Defensive invariant: a same-id payload whose validated maneuver list
  // shrank must not leave the stored index out of range (a validated route
  // cannot shrink, but a reused RouteIndex can be reloaded from a different
  // payload before the caller resets the selector explicitly).
  if (index_ >= route.maneuverCount) {
    index_ = 0;
  }

  if (trusted) {
    // Advance monotonically through every maneuver whose distance is more
    // than kPassHysteresisMeters behind the trusted progress (several may
    // pass in one update). The arrival maneuver is terminal: once selected it
    // is never advanced past, so BESTEMMING stays on screen through and after
    // the destination and an earlier arrival stops progression without any
    // scan beyond maneuverCount.
    while (index_ + 1 < route.maneuverCount && !isArrival(route.maneuvers[index_]) &&
           distanceFromStartMeters >= passThreshold(route.maneuvers[index_].distanceFromStartMeters)) {
      ++index_;
    }
  }

  ManeuverSelection selection;
  selection.valid = true;
  selection.routeManeuverIndex = index_;
  selection.maneuver = maneuverFromKind(route.maneuvers[index_].type);
  selection.distanceMeters =
      remainingDistanceMeters(route.maneuvers[index_].distanceFromStartMeters, distanceFromStartMeters);
  return selection;
}

}  // namespace navigator
