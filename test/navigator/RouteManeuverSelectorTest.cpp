// Host tests for the fixed-size, allocation-free next-maneuver selector
// (src/spikes/navigator/route/RouteManeuverSelector.h/.cpp).
//
// Task 7 (firmware core) - strict TDD: this file was written first against the
// monotonic-selection contract and only compiles once RouteManeuverSelector
// exists. The selector must:
//   * select maneuver index 0 first when a route carries maneuvers, then
//     advance monotonically within a route/session, only while progress is
//     trusted and only after progress passes a maneuver's distance by the
//     exact kPassHysteresisMeters = 12 (multiple passed maneuvers may advance
//     in one update and the comparison is overflow-safe / saturating);
//   * never move its stored index backward and never advance while progress
//     is untrusted, while still reporting a deterministic, saturating
//     remaining distance computed from the supplied progress;
//   * treat the arrival maneuver as terminal: it stays selectable and is
//     never advanced past merely by +12, through and after the destination,
//     and an arrival placed earlier in the list is handled deterministically
//     without ever scanning beyond maneuverCount;
//   * report valid=false and clear any prior-route selection for an empty (or
//     beyond-capacity) maneuver list, and detect route identity changes (the
//     validated RouteIndex::routeId) so a selector reused for a different
//     RouteIndex never retains a stale or out-of-range maneuver;
//   * stay fixed-size (no heap, no strings), with
//     static_assert(sizeof(RouteManeuverSelector) <= 8).

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "route/RouteManeuverSelector.h"

using namespace navigator;

// The selector's whole state must stay tiny so it can live in static storage
// on the ESP32-C3 next to the route index, never on a task stack.
static_assert(sizeof(RouteManeuverSelector) <= 8, "selector must stay <= 8 bytes");
static_assert(RouteManeuverSelector::kPassHysteresisMeters == 12, "pass hysteresis must stay exactly 12 meters");

namespace {

// Wire maneuver kind bytes (Route Package v1 maneuver-record type 0..6).
// RoutePackageV1.h documents that they map 1:1 onto navigator::Maneuver, so
// the fixtures below build validated RouteIndex maneuver lists from these
// bytes exactly as the decoder would fill them.
constexpr uint8_t kStraight = 0;
constexpr uint8_t kLeft = 1;
constexpr uint8_t kRight = 2;
constexpr uint8_t kSlightLeft = 3;
constexpr uint8_t kSlightRight = 4;
constexpr uint8_t kUTurn = 5;
constexpr uint8_t kArrive = 6;

struct TurnSpec {
  uint8_t type;
  uint32_t distanceFromStartMeters;
};

// Builds a zero-initialized RouteIndex whose only meaningful fields are the
// route id and the [0, maneuverCount) maneuver records, matching what the
// decoder publishes into a validated index. The remaining geometry/header
// fields stay zero because the selector may only consume validated maneuver
// fields and never touches the rest of the index.
RouteIndex routeWithTurns(uint32_t routeId, const TurnSpec* begin, const TurnSpec* end) {
  RouteIndex route{};
  route.routeId = routeId;
  for (const TurnSpec* it = begin; it != end && route.maneuverCount < RouteIndex::kMaxManeuvers; ++it) {
    RouteIndex::Maneuver& maneuver = route.maneuvers[route.maneuverCount];
    maneuver.type = it->type;
    maneuver.distanceFromStartMeters = it->distanceFromStartMeters;
    ++route.maneuverCount;
  }
  return route;
}

RouteIndex routeWith(uint32_t routeId, std::initializer_list<TurnSpec> turns) {
  return routeWithTurns(routeId, turns.begin(), turns.end());
}

RouteIndex routeWith(uint32_t routeId, const std::vector<TurnSpec>& turns) {
  return routeWithTurns(routeId, turns.data(), turns.data() + turns.size());
}

RouteIndex emptyRoute(uint32_t routeId) { return routeWith(routeId, {}); }

}  // namespace

TEST(RouteManeuverSelectorTest, AdvancesOnlyAfterTwelveMetersPastTurn) {
  const RouteIndex route = routeWith(1, {{kLeft, 100}, {kRight, 220}, {kUTurn, 400}});
  RouteManeuverSelector selector;
  const auto sel = [&](uint32_t meters, bool trusted) { return selector.update(route, meters, trusted); };

  EXPECT_EQ(sel(99, true).routeManeuverIndex, 0);
  EXPECT_EQ(sel(111, true).routeManeuverIndex, 0);  // 100 + 12 boundary not crossed yet
  EXPECT_EQ(sel(112, true).routeManeuverIndex, 1);  // 100 + 12 crossed: advance
  EXPECT_EQ(sel(231, true).routeManeuverIndex, 1);  // 220 + 12 boundary not crossed yet
  EXPECT_EQ(sel(232, true).routeManeuverIndex, 2);  // 220 + 12 crossed: advance
  EXPECT_EQ(sel(411, true).routeManeuverIndex, 2);  // 400 + 12 boundary not crossed yet
  EXPECT_EQ(sel(412, true).routeManeuverIndex, 2);  // last maneuver never advances past itself
}

TEST(RouteManeuverSelectorTest, UntrustedProgressDoesNotAdvance) {
  const RouteIndex route = routeWith(1, {{kLeft, 100}, {kRight, 220}, {kUTurn, 400}});
  RouteManeuverSelector selector;
  const auto sel = [&](uint32_t meters, bool trusted) { return selector.update(route, meters, trusted); };

  EXPECT_EQ(sel(0, true).routeManeuverIndex, 0);
  EXPECT_EQ(sel(111, true).routeManeuverIndex, 0);
  // 112 m passes turn zero by exactly the 12 m hysteresis, but an untrusted
  // fix (missing/stale/too far off route) must never advance the selection.
  EXPECT_EQ(sel(112, false).routeManeuverIndex, 0);
  EXPECT_EQ(sel(5000, false).routeManeuverIndex, 0);
  // Trust recovery advances through every passed maneuver in one update.
  EXPECT_EQ(sel(5000, true).routeManeuverIndex, 2);
  // Once advanced, a later untrusted fix can never pull the index backward.
  EXPECT_EQ(sel(0, false).routeManeuverIndex, 2);
}

TEST(RouteManeuverSelectorTest, FirstSelectionIsManeuverIndexZeroWhenManeuversExist) {
  const RouteIndex route = routeWith(1, {{kLeft, 100}, {kRight, 220}});
  RouteManeuverSelector selector;
  const ManeuverSelection selection = selector.update(route, 0, true);

  EXPECT_TRUE(selection.valid);
  EXPECT_EQ(selection.routeManeuverIndex, 0);
  EXPECT_EQ(selection.maneuver, Maneuver::Left);
  EXPECT_EQ(selection.distanceMeters, 100);
}

TEST(RouteManeuverSelectorTest, MultiplePassedManeuversAdvanceInOneTrustedUpdate) {
  // A fresh selector first fed a mid-route trusted progress (a DEBUG halfway
  // jump or a fix that recovers after a gap) must land on the correct current
  // maneuver: it starts at index 0 and advances monotonically through every
  // maneuver already more than 12 m behind 620 m of progress.
  const RouteIndex route = routeWith(1, {{kLeft, 100}, {kRight, 220}, {kSlightLeft, 300}, {kSlightRight, 1000}});
  RouteManeuverSelector selector;
  const ManeuverSelection selection = selector.update(route, 620, true);

  EXPECT_TRUE(selection.valid);
  EXPECT_EQ(selection.routeManeuverIndex, 3);  // 100, 220 and 300 all passed; 1000 not yet
  EXPECT_EQ(selection.maneuver, Maneuver::SlightRight);
}

TEST(RouteManeuverSelectorTest, DistanceIsSaturatingRemainingMetersToSelectedManeuver) {
  const RouteIndex route = routeWith(1, {{kLeft, 100}, {kRight, 220}});
  RouteManeuverSelector selector;
  const auto sel = [&](uint32_t meters, bool trusted) { return selector.update(route, meters, trusted); };

  EXPECT_EQ(sel(50, true).distanceMeters, 50);
  // Between 100 and 112 m the first maneuver is already behind by up to 12 m
  // but is still selected, so its remaining distance saturates to zero.
  EXPECT_EQ(sel(111, true).distanceMeters, 0);
  EXPECT_EQ(sel(112, true).distanceMeters, 108);  // 220 - 112
  EXPECT_EQ(sel(219, true).distanceMeters, 1);
  EXPECT_EQ(sel(10'000, true).distanceMeters, 0);  // last maneuver passed long ago
}

TEST(RouteManeuverSelectorTest, DistanceClampsToUint16DisplayRange) {
  const RouteIndex route = routeWith(1, {{kLeft, 4'000'000'000u}, {kRight, 4'000'000'100u}});
  RouteManeuverSelector selector;
  const ManeuverSelection selection = selector.update(route, 50, true);

  EXPECT_TRUE(selection.valid);
  EXPECT_EQ(selection.routeManeuverIndex, 0);
  EXPECT_EQ(selection.distanceMeters, UINT16_MAX);  // ~4e9 - 50 saturates at 65535
}

TEST(RouteManeuverSelectorTest, UntrustedProgressKeepsSelectionButReportsSaturatingDistance) {
  const RouteIndex route = routeWith(1, {{kLeft, 100}, {kRight, 220}});
  RouteManeuverSelector selector;
  const auto sel = [&](uint32_t meters, bool trusted) { return selector.update(route, meters, trusted); };

  EXPECT_EQ(sel(50, true).routeManeuverIndex, 0);
  // An untrusted fix past the turn keeps index 0, but the reported remaining
  // distance stays a safe deterministic value derived from the supplied
  // progress (100 - 150 saturates to zero), never a stale or garbage value.
  const ManeuverSelection passed = sel(150, false);
  EXPECT_EQ(passed.routeManeuverIndex, 0);
  EXPECT_EQ(passed.distanceMeters, 0);
  const ManeuverSelection behind = sel(80, false);
  EXPECT_EQ(behind.routeManeuverIndex, 0);
  EXPECT_EQ(behind.distanceMeters, 20);
}

TEST(RouteManeuverSelectorTest, EmptyManeuverRouteIsInvalidAndClearsPriorSelection) {
  RouteManeuverSelector selector;
  const RouteIndex withTurns = routeWith(1, {{kLeft, 100}, {kRight, 220}});
  const auto pick = [&](const RouteIndex& route, uint32_t meters) { return selector.update(route, meters, true); };

  const ManeuverSelection none = pick(emptyRoute(9), 0);
  EXPECT_FALSE(none.valid);
  EXPECT_EQ(none.routeManeuverIndex, 0);
  EXPECT_EQ(none.maneuver, Maneuver::Straight);
  EXPECT_EQ(none.distanceMeters, 0);

  // A usable route after an empty one starts a fresh selection at index zero.
  EXPECT_EQ(pick(withTurns, 0).routeManeuverIndex, 0);
  EXPECT_EQ(pick(withTurns, 232).routeManeuverIndex, 1);

  // Going back to an empty route must not leave the index-1 selection behind.
  EXPECT_FALSE(pick(emptyRoute(9), 0).valid);
  EXPECT_EQ(pick(withTurns, 0).routeManeuverIndex, 0);  // starts over, no stale index 1
}

TEST(RouteManeuverSelectorTest, RouteIdentityChangeResetsStaleSelection) {
  const RouteIndex first = routeWith(1, {{kLeft, 100}, {kRight, 220}, {kUTurn, 400}});
  const RouteIndex second = routeWith(2, {{kSlightRight, 5000}});
  RouteManeuverSelector selector;
  const auto pick = [&](const RouteIndex& route, uint32_t meters) { return selector.update(route, meters, true); };

  EXPECT_EQ(pick(first, 0).routeManeuverIndex, 0);
  EXPECT_EQ(pick(first, 231).routeManeuverIndex, 1);  // passed turn 0 (112), not turn 1 (232)

  // A different validated RouteIndex (different route id) must never reuse the
  // stored index-1 selection: the selector resets and starts at zero on the
  // new route instead of reporting a stale or out-of-range maneuver.
  const ManeuverSelection switched = pick(second, 0);
  EXPECT_TRUE(switched.valid);
  EXPECT_EQ(switched.routeManeuverIndex, 0);
  EXPECT_EQ(switched.maneuver, Maneuver::SlightRight);
  EXPECT_EQ(switched.distanceMeters, 5000);
}

TEST(RouteManeuverSelectorTest, SameRouteIdWithShrunkManeuverListCannotRetainOutOfRangeIndex) {
  const RouteIndex full = routeWith(7, {{kLeft, 100}, {kRight, 220}, {kUTurn, 400}});
  const RouteIndex shrunk = routeWith(7, {{kSlightLeft, 500}});
  RouteManeuverSelector selector;
  const auto pick = [&](const RouteIndex& route, uint32_t meters) { return selector.update(route, meters, true); };

  EXPECT_EQ(pick(full, 0).routeManeuverIndex, 0);
  EXPECT_EQ(pick(full, 231).routeManeuverIndex, 1);  // passed turn 0 (112), not turn 1 (232)

  // A same-id payload whose validated maneuver list shrank must not leave the
  // stored index 1 dangling beyond the new count; the selector falls back to
  // a fresh selection on the current list.
  const ManeuverSelection selection = pick(shrunk, 0);
  EXPECT_TRUE(selection.valid);
  EXPECT_EQ(selection.routeManeuverIndex, 0);
  EXPECT_EQ(selection.maneuver, Maneuver::SlightLeft);
}

TEST(RouteManeuverSelectorTest, ArrivalIsTerminalAndStaysSelectedThroughAndAfterDestination) {
  const RouteIndex route = routeWith(1, {{kLeft, 100}, {kArrive, 220}});
  RouteManeuverSelector selector;
  const auto sel = [&](uint32_t meters, bool trusted) { return selector.update(route, meters, trusted); };

  EXPECT_EQ(sel(99, true).routeManeuverIndex, 0);
  const ManeuverSelection arrived = sel(112, true);  // passed the turn; arrival is next
  EXPECT_EQ(arrived.routeManeuverIndex, 1);
  EXPECT_EQ(arrived.maneuver, Maneuver::Arrive);
  // Passing the arrival by exactly the 12 m hysteresis must never advance
  // past it: arrival remains selected through and after the destination.
  EXPECT_EQ(sel(231, true).routeManeuverIndex, 1);
  EXPECT_EQ(sel(232, true).routeManeuverIndex, 1);
  const ManeuverSelection after = sel(9'999'999, true);
  EXPECT_EQ(after.routeManeuverIndex, 1);
  EXPECT_EQ(after.distanceMeters, 0);
}

TEST(RouteManeuverSelectorTest, ArrivalEarlierInManeuverListIsHandledDeterministically) {
  // A malformed-but-validated ordering (arrival before later turns) must never
  // scan past maneuverCount nor select a maneuver that follows the arrival.
  const RouteIndex route = routeWith(1, {{kArrive, 100}, {kRight, 220}, {kSlightLeft, 400}});
  RouteManeuverSelector selector;
  const auto sel = [&](uint32_t meters, bool trusted) { return selector.update(route, meters, trusted); };

  const ManeuverSelection first = sel(0, true);
  EXPECT_EQ(first.routeManeuverIndex, 0);
  EXPECT_EQ(first.maneuver, Maneuver::Arrive);
  EXPECT_EQ(sel(150, true).routeManeuverIndex, 0);     // never jumps to the turn at 220
  EXPECT_EQ(sel(50'000, true).routeManeuverIndex, 0);  // arrival stays selected forever
}

TEST(RouteManeuverSelectorTest, AdvancingIntoArrivalStopsBeforeLaterManeuvers) {
  const RouteIndex route = routeWith(1, {{kLeft, 100}, {kArrive, 220}, {kRight, 400}});
  RouteManeuverSelector selector;
  const auto sel = [&](uint32_t meters, bool trusted) { return selector.update(route, meters, trusted); };

  EXPECT_EQ(sel(0, true).routeManeuverIndex, 0);
  EXPECT_EQ(sel(112, true).routeManeuverIndex, 1);     // passes the turn; arrival is next
  EXPECT_EQ(sel(232, true).routeManeuverIndex, 1);     // +12 past arrival: still arrival
  EXPECT_EQ(sel(50'000, true).routeManeuverIndex, 1);  // the turn at 400 is never selected
}

TEST(RouteManeuverSelectorTest, ResetReturnsToInitialBehavior) {
  const RouteIndex route = routeWith(1, {{kLeft, 100}, {kRight, 220}, {kUTurn, 400}});
  RouteManeuverSelector selector;
  const auto sel = [&](uint32_t meters, bool trusted) { return selector.update(route, meters, trusted); };

  EXPECT_EQ(sel(0, true).routeManeuverIndex, 0);
  EXPECT_EQ(sel(231, true).routeManeuverIndex, 1);  // passed turn 0 (112), not turn 1 (232)
  selector.reset();
  EXPECT_EQ(sel(99, true).routeManeuverIndex, 0);  // starts over from index zero
  EXPECT_EQ(sel(112, true).routeManeuverIndex, 1);
  selector.reset();
  EXPECT_EQ(sel(0, true).routeManeuverIndex, 0);
}

TEST(RouteManeuverSelectorTest, RouteAtMaxManeuverCapacityStaysSelectable) {
  std::vector<TurnSpec> turns;
  turns.reserve(RouteIndex::kMaxManeuvers);
  for (uint16_t i = 0; i < RouteIndex::kMaxManeuvers; ++i) {
    // Cycle through turn kinds; keep the final slot arrival to mirror the
    // iPhone encoder contract of reserving one slot for the destination.
    const uint8_t kind = i == RouteIndex::kMaxManeuvers - 1 ? kArrive : static_cast<uint8_t>(1 + i % (kArrive - 1));
    turns.push_back({kind, uint32_t(i) * 100u});
  }
  const RouteIndex route = routeWith(1, turns);
  ASSERT_EQ(route.maneuverCount, RouteIndex::kMaxManeuvers);

  RouteManeuverSelector selector;
  const auto sel = [&](uint32_t meters) { return selector.update(route, meters, true); };

  EXPECT_EQ(sel(0).routeManeuverIndex, 0);
  EXPECT_EQ(sel(0).maneuver, Maneuver::Left);  // kind byte 1 round-trips to the enum
  const ManeuverSelection last = sel(7000);    // passes all 63 turn thresholds
  EXPECT_TRUE(last.valid);
  EXPECT_EQ(last.routeManeuverIndex, RouteIndex::kMaxManeuvers - 1);
  EXPECT_EQ(last.maneuver, Maneuver::Arrive);
  EXPECT_EQ(last.distanceMeters, 0);  // 6300 - 7000 saturates to zero
}

TEST(RouteManeuverSelectorTest, BeyondCapacityManeuverCountIsTreatedAsInvalid) {
  RouteIndex route{};  // zero-initialized, decoder-shaped index
  route.routeId = 3;
  route.maneuverCount = RouteIndex::kMaxManeuvers + 1;  // impossible for a validated route
  RouteManeuverSelector selector;

  const ManeuverSelection selection = selector.update(route, 0, true);
  EXPECT_FALSE(selection.valid);
}

TEST(RouteManeuverSelectorTest, PassComparisonSaturatesInsteadOfOverflowing) {
  // distanceFromStartMeters at the top of the uint32 range: distance + 12 must
  // saturate at UINT32_MAX instead of wrapping around to a tiny threshold.
  const RouteIndex route = routeWith(1, {{kLeft, 0xFFFFFFFFu}, {kRight, 0xFFFFFFFEu}});
  RouteManeuverSelector selector;
  const auto sel = [&](uint32_t meters, bool trusted) { return selector.update(route, meters, trusted); };

  // UINT32_MAX - 1 is still below the saturated threshold UINT32_MAX.
  const ManeuverSelection before = sel(0xFFFFFFFEu, true);
  EXPECT_EQ(before.routeManeuverIndex, 0);
  EXPECT_EQ(before.distanceMeters, 1);
  // Only UINT32_MAX itself reaches the saturated threshold and passes.
  EXPECT_EQ(sel(0xFFFFFFFFu, true).routeManeuverIndex, 1);
}

TEST(RouteManeuverSelectorTest, SelectedManeuverMapsWireKindOntoNavigatorEnum) {
  const RouteIndex route = routeWith(
      1, {{kLeft, 100}, {kRight, 220}, {kSlightLeft, 340}, {kSlightRight, 460}, {kUTurn, 580}, {kArrive, 700}});
  RouteManeuverSelector selector;
  const auto sel = [&](uint32_t meters, bool trusted) { return selector.update(route, meters, trusted); };

  EXPECT_EQ(sel(0, true).maneuver, Maneuver::Left);
  EXPECT_EQ(sel(111, true).maneuver, Maneuver::Left);
  EXPECT_EQ(sel(112, true).maneuver, Maneuver::Right);
  EXPECT_EQ(sel(231, true).maneuver, Maneuver::Right);
  EXPECT_EQ(sel(232, true).maneuver, Maneuver::SlightLeft);
  EXPECT_EQ(sel(352, true).maneuver, Maneuver::SlightRight);
  EXPECT_EQ(sel(472, true).maneuver, Maneuver::UTurn);
  EXPECT_EQ(sel(592, true).maneuver, Maneuver::Arrive);
  EXPECT_EQ(sel(9'999'999, true).maneuver, Maneuver::Arrive);
}

TEST(RouteManeuverSelectorTest, StraightWireKindMapsOntoStraightEnum) {
  const RouteIndex route = routeWith(1, {{kStraight, 0}});
  RouteManeuverSelector selector;
  const ManeuverSelection selection = selector.update(route, 0, true);

  EXPECT_TRUE(selection.valid);
  EXPECT_EQ(selection.routeManeuverIndex, 0);
  EXPECT_EQ(selection.maneuver, Maneuver::Straight);
  EXPECT_EQ(selection.distanceMeters, 0);
}
