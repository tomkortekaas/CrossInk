#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "LiveFrameMailbox.h"

namespace {

std::array<uint8_t, 20> fix(uint16_t sequence, int32_t latitudeE7) {
  std::array<uint8_t, 20> frame{};
  frame[0] = 0x09;
  frame[1] = 7;
  frame[5] = static_cast<uint8_t>(sequence);
  frame[6] = static_cast<uint8_t>(sequence >> 8);
  const uint32_t latitude = static_cast<uint32_t>(latitudeE7);
  for (int byte = 0; byte < 4; ++byte) frame[7 + byte] = static_cast<uint8_t>(latitude >> (8 * byte));
  frame[11] = 64;
  frame[12] = 246;
  frame[13] = 235;
  frame[14] = 2;
  frame[15] = 5;
  return frame;
}

TEST(LiveFrameMailboxTest, NewestFixReplacesPendingFixWithoutOverflow) {
  navigator::LiveFrameMailbox mailbox;
  const auto first = fix(41, 523700000);
  const auto newest = fix(42, 523700123);

  EXPECT_TRUE(mailbox.push(first.data(), first.size()));
  EXPECT_TRUE(mailbox.push(newest.data(), newest.size()));
  EXPECT_FALSE(mailbox.overflowed());

  std::array<uint8_t, 512> received{};
  const size_t length = mailbox.take(received.data(), received.size());
  ASSERT_EQ(length, newest.size());
  EXPECT_EQ(received[5], 42);
  EXPECT_EQ(received[6], 0);
  EXPECT_EQ(received[7], newest[7]);
  EXPECT_EQ(received[8], newest[8]);
  EXPECT_EQ(received[9], newest[9]);
  EXPECT_EQ(received[10], newest[10]);
}

TEST(LiveFrameMailboxTest, NonFixCollisionRemainsAnOverflow) {
  navigator::LiveFrameMailbox mailbox;
  const std::array<uint8_t, 5> stop{0x0B, 1, 0, 0, 0};
  const auto pendingFix = fix(9, 523700000);

  EXPECT_TRUE(mailbox.push(pendingFix.data(), pendingFix.size()));
  EXPECT_FALSE(mailbox.push(stop.data(), stop.size()));
  EXPECT_TRUE(mailbox.overflowed());
}

TEST(LiveFrameMailboxTest, MalformedFixCannotReplacePendingValidFix) {
  navigator::LiveFrameMailbox mailbox;
  const auto valid = fix(11, 523700000);
  auto malformed = fix(12, 523700100);
  malformed[15] = 0;  // accuracy must be 1..50 metres

  EXPECT_TRUE(mailbox.push(valid.data(), valid.size()));
  EXPECT_FALSE(mailbox.push(malformed.data(), malformed.size()));
  EXPECT_TRUE(mailbox.overflowed());

  std::array<uint8_t, 512> received{};
  ASSERT_EQ(mailbox.take(received.data(), received.size()), valid.size());
  EXPECT_EQ(received[5], 11);
}

TEST(LiveFrameMailboxTest, FixFromAnotherSessionCannotReplacePendingFix) {
  navigator::LiveFrameMailbox mailbox;
  const auto current = fix(20, 523700000);
  auto otherSession = fix(21, 523700100);
  otherSession[1] = 8;

  EXPECT_TRUE(mailbox.push(current.data(), current.size()));
  EXPECT_FALSE(mailbox.push(otherSession.data(), otherSession.size()));
  EXPECT_TRUE(mailbox.overflowed());
}

}  // namespace
