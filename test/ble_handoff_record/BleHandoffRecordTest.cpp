#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

#include "BleHandoffRecord.h"

namespace {

using ble_handoff::DecodedRecord;
using ble_handoff::RecordBytes;
using ble_handoff::Status;

constexpr std::array<uint8_t, 8> HELLO_X3 = {'h', 'e', 'l', 'l', 'o', ' ', 'x', '3'};

RecordBytes validRecord() {
  RecordBytes bytes{};
  EXPECT_EQ(ble_handoff::buildRecord(HELLO_X3.data(), HELLO_X3.size(), 7, bytes), Status::Ok);
  return bytes;
}

TEST(BleHandoffRecord, UsesStandardCrc32) {
  constexpr std::array<uint8_t, 9> input = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(ble_handoff::crc32(input.data(), input.size()), 0xCBF43926U);
}

TEST(BleHandoffRecord, RoundTripsHelloX3) {
  const RecordBytes bytes = validRecord();
  DecodedRecord decoded{};

  ASSERT_EQ(ble_handoff::validateRecord(bytes.data(), bytes.size(), decoded), Status::Ok);
  EXPECT_EQ(decoded.sequence, 7U);
  EXPECT_EQ(decoded.length, HELLO_X3.size());
  EXPECT_TRUE(std::equal(HELLO_X3.begin(), HELLO_X3.end(), decoded.payload.begin()));
  EXPECT_EQ(decoded.crc, 0x52C9C233U);
}

TEST(BleHandoffRecord, UsesSpecifiedPersistedByteLayout) {
  RecordBytes expected{};
  expected[0] = 0x45;
  expected[1] = 0x4C;
  expected[2] = 0x42;
  expected[3] = 0x58;
  expected[4] = 0x01;
  expected[5] = 0x00;
  expected[6] = 0x07;
  expected[10] = 0x08;
  std::copy(HELLO_X3.begin(), HELLO_X3.end(), expected.begin() + 11);
  expected[75] = 0x33;
  expected[76] = 0xC2;
  expected[77] = 0xC9;
  expected[78] = 0x52;

  RecordBytes actual{};
  ASSERT_EQ(ble_handoff::buildRecord(HELLO_X3.data(), HELLO_X3.size(), 7, actual), Status::Ok);
  EXPECT_EQ(actual, expected);
}

TEST(BleHandoffRecord, RejectsChangedCoveredByte) {
  RecordBytes bytes = validRecord();
  bytes[11] ^= 0x01;
  DecodedRecord decoded{};
  EXPECT_EQ(ble_handoff::validateRecord(bytes.data(), bytes.size(), decoded), Status::InvalidCrc);
}

TEST(BleHandoffRecord, RejectsChangedSequenceOrLength) {
  DecodedRecord decoded{};
  RecordBytes changedSequence = validRecord();
  changedSequence[6] ^= 0x01;
  EXPECT_EQ(ble_handoff::validateRecord(changedSequence.data(), changedSequence.size(), decoded), Status::InvalidCrc);

  RecordBytes changedLength = validRecord();
  changedLength[10] = 9;
  EXPECT_EQ(ble_handoff::validateRecord(changedLength.data(), changedLength.size(), decoded), Status::InvalidCrc);
}

TEST(BleHandoffRecord, RejectsWrongRecordSize) {
  const RecordBytes bytes = validRecord();
  DecodedRecord decoded{};
  EXPECT_EQ(ble_handoff::validateRecord(bytes.data(), bytes.size() - 1, decoded), Status::InvalidSize);
  EXPECT_EQ(ble_handoff::validateRecord(bytes.data(), bytes.size() + 1, decoded), Status::InvalidSize);
}

TEST(BleHandoffRecord, RejectsWrongMagicAndVersion) {
  DecodedRecord decoded{};
  RecordBytes badMagic = validRecord();
  badMagic[0] ^= 0x01;
  EXPECT_EQ(ble_handoff::validateRecord(badMagic.data(), badMagic.size(), decoded), Status::InvalidMagic);

  RecordBytes badVersion = validRecord();
  badVersion[4] ^= 0x01;
  EXPECT_EQ(ble_handoff::validateRecord(badVersion.data(), badVersion.size(), decoded), Status::InvalidVersion);
}

TEST(BleHandoffRecord, RejectsInvalidPayloadBounds) {
  RecordBytes bytes{};
  std::array<uint8_t, ble_handoff::MAX_PAYLOAD_SIZE + 1> oversized{};
  EXPECT_EQ(ble_handoff::buildRecord(nullptr, 0, 1, bytes), Status::InvalidLength);
  EXPECT_EQ(ble_handoff::buildRecord(oversized.data(), oversized.size(), 1, bytes), Status::InvalidLength);
  EXPECT_EQ(ble_handoff::buildRecord(nullptr, 1, 1, bytes), Status::InvalidArgument);
}

TEST(BleHandoffRecord, GeneratesInitialAndIncrementedSequences) {
  uint32_t next = 99;
  EXPECT_EQ(ble_handoff::nextSequence(false, 99, next), Status::Ok);
  EXPECT_EQ(next, 1U);
  EXPECT_EQ(ble_handoff::nextSequence(true, 41, next), Status::Ok);
  EXPECT_EQ(next, 42U);
}

TEST(BleHandoffRecord, RejectsSequenceOverflow) {
  uint32_t next = 99;
  EXPECT_EQ(ble_handoff::nextSequence(true, std::numeric_limits<uint32_t>::max(), next),
            Status::SequenceOverflow);
  EXPECT_EQ(next, 99U);
}

}  // namespace
