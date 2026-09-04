// Host tests for the allocation-free navigation state model (NavState.h).
//
// The model is the single source of values the navigator screen renders:
// maneuver, next-maneuver distance in meters, a bounded street name, remaining
// distance in meters, remaining time in minutes, and navigation status. It
// must be plain data with a bounded, normalizing street setter -- no heap, no
// std::string -- so it stays safe on the constrained ESP32-C3.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "NavState.h"

namespace {

TEST(NavStateTest, DefaultsMatchVerifiedExampleState) {
  const navigator::NavState state;
  EXPECT_EQ(state.maneuver, navigator::Maneuver::Left);
  EXPECT_EQ(state.nextDistanceMeters, 180U);
  EXPECT_EQ(state.remainingDistanceMeters, 4200U);
  EXPECT_EQ(state.remainingMinutes, 52U);
  EXPECT_EQ(state.status, navigator::NavStatus::Navigating);
  EXPECT_STREQ(state.street, "DUINWEG");
}

TEST(NavStateTest, ManeuverEnumCoversApprovedSet) {
  using navigator::Maneuver;
  EXPECT_NE(Maneuver::Straight, Maneuver::Left);
  EXPECT_NE(Maneuver::Left, Maneuver::Right);
  EXPECT_NE(Maneuver::Right, Maneuver::SlightLeft);
  EXPECT_NE(Maneuver::SlightLeft, Maneuver::SlightRight);
  EXPECT_NE(Maneuver::SlightRight, Maneuver::UTurn);
  EXPECT_NE(Maneuver::UTurn, Maneuver::Arrive);
  EXPECT_EQ(static_cast<int>(Maneuver::kCount), 7);
  EXPECT_LT(static_cast<int>(Maneuver::Straight), static_cast<int>(Maneuver::kCount));
  EXPECT_LT(static_cast<int>(Maneuver::Arrive), static_cast<int>(Maneuver::kCount));
}

TEST(NavStateTest, NavStatusEnumCoversApprovedSet) {
  using navigator::NavStatus;
  EXPECT_NE(NavStatus::Navigating, NavStatus::Recalculating);
  EXPECT_NE(NavStatus::Recalculating, NavStatus::OffRoute);
  EXPECT_NE(NavStatus::OffRoute, NavStatus::Arrived);
  EXPECT_EQ(static_cast<int>(NavStatus::kCount), 4);
  EXPECT_LT(static_cast<int>(NavStatus::Navigating), static_cast<int>(NavStatus::kCount));
  EXPECT_LT(static_cast<int>(NavStatus::Arrived), static_cast<int>(NavStatus::kCount));
}

TEST(NavStateTest, StreetSetterTruncatesToCapacityAndStaysNullTerminated) {
  navigator::NavState state;
  const char* huge = "ZUIDELIJKE RINGWEG LAAN VAN DE EENDRACHT 12345";
  state.setStreet(huge);

  // Longest representable name: capacity minus the NUL terminator.
  EXPECT_EQ(std::strlen(state.street), static_cast<size_t>(navigator::NavState::kStreetCapacity - 1));
  EXPECT_EQ(state.street[navigator::NavState::kStreetCapacity - 1], '\0');
  // Every stored byte is part of the copied (normalized) name, never garbage.
  EXPECT_EQ(std::strncmp(state.street, huge, navigator::NavState::kStreetCapacity - 1), 0);
}

TEST(NavStateTest, StreetSetterNormalizesLowercaseToUppercase) {
  navigator::NavState state;
  state.setStreet("Duinweg");
  EXPECT_STREQ(state.street, "DUINWEG");
}

TEST(NavStateTest, StreetSetterKeepsSupportedPunctuationAndReplacesTheRest) {
  navigator::NavState state;
  state.setStreet("A. van der Bilt-12, Kanaaldijk");
  EXPECT_STREQ(state.street, "A. VAN DER BILT-12, KANAALDIJK");

  navigator::NavState unsupported;
  unsupported.setStreet("Zuid*weg & Polder");
  EXPECT_STREQ(unsupported.street, "ZUID WEG   POLDER");
}

TEST(NavStateTest, StreetSetterNullInputClears) {
  navigator::NavState state;
  state.setStreet(nullptr);
  EXPECT_STREQ(state.street, "");
}

TEST(NavStateTest, PlainDataFieldsHoldFullUnsignedRange) {
  navigator::NavState state;
  state.nextDistanceMeters = UINT16_MAX;
  state.remainingDistanceMeters = UINT32_MAX;
  state.remainingMinutes = UINT16_MAX;
  EXPECT_EQ(state.nextDistanceMeters, UINT16_MAX);
  EXPECT_EQ(state.remainingDistanceMeters, UINT32_MAX);
  EXPECT_EQ(state.remainingMinutes, UINT16_MAX);
  // The whole model (with its 32-byte street buffer) stays well under 96 bytes.
  static_assert(sizeof(navigator::NavState) < 96, "state model must stay compact");
}

}  // namespace
