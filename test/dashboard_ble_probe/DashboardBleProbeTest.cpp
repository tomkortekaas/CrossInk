#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "DashboardBleProbe.h"
#include "activities/boot_sleep/DashboardSleepScreen.h"

namespace {

class FakeScreen final : public probe::ProbeScreen {
 public:
  bool result = true;
  int renderCalls = 0;
  uint32_t lastMessageId = 0;
  uint8_t lastPayloadLength = 0;

  bool renderAccepted(const uint32_t messageId, const uint8_t payloadLength) override {
    ++renderCalls;
    lastMessageId = messageId;
    lastPayloadLength = payloadLength;
    return result;
  }
};

class FakeStatusSink final : public probe::ProbeStatusSink {
 public:
  int publishCalls = 0;
  probe::Status last = probe::Status::InvalidLength;
  bool lastHasMessageId = false;
  uint32_t lastMessageId = 0;

  void publish(const probe::Status status, const bool hasMessageId, const uint32_t messageId) override {
    ++publishCalls;
    last = status;
    lastHasMessageId = hasMessageId;
    lastMessageId = messageId;
  }
};

std::vector<uint8_t> makeFrame(const uint32_t messageId, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> frame = {'X', '3', 'B', 'P', 1,
                                static_cast<uint8_t>(messageId), static_cast<uint8_t>(messageId >> 8),
                                static_cast<uint8_t>(messageId >> 16), static_cast<uint8_t>(messageId >> 24),
                                static_cast<uint8_t>(payload.size()), 0};
  frame.insert(frame.end(), payload.begin(), payload.end());
  const uint32_t crc = probe::crc32(payload.data(), payload.size());
  frame.push_back(static_cast<uint8_t>(crc));
  frame.push_back(static_cast<uint8_t>(crc >> 8));
  frame.push_back(static_cast<uint8_t>(crc >> 16));
  frame.push_back(static_cast<uint8_t>(crc >> 24));
  return frame;
}

}  // namespace

TEST(DashboardSleepScreen, UsesProviderOnlyWhenPersonalDashboardIsSelected) {
  class FakeDashboard final : public DashboardSleepScreen {
   public:
    bool renderForSleep() override {
      ++calls;
      return result;
    }

    bool result = true;
    int calls = 0;
  } dashboard;

  EXPECT_FALSE(tryRenderDashboardSleepScreen(false, &dashboard));
  EXPECT_EQ(dashboard.calls, 0);
  EXPECT_FALSE(tryRenderDashboardSleepScreen(true, nullptr));
  EXPECT_TRUE(tryRenderDashboardSleepScreen(true, &dashboard));
  EXPECT_EQ(dashboard.calls, 1);
  dashboard.result = false;
  EXPECT_FALSE(tryRenderDashboardSleepScreen(true, &dashboard));
}

TEST(DashboardBleProbe, BeginPublishesReady) {
  FakeScreen screen;
  FakeStatusSink status;
  probe::DashboardBleProbe controller(screen, status);
  controller.begin();
  EXPECT_EQ(status.publishCalls, 1);
  EXPECT_EQ(status.last, probe::Status::Ready);
  EXPECT_FALSE(status.lastHasMessageId);
}

TEST(DashboardBleProbe, LoopWithoutWorkIsNoOp) {
  FakeScreen screen;
  FakeStatusSink status;
  probe::DashboardBleProbe controller(screen, status);
  controller.loop();
  EXPECT_EQ(screen.renderCalls, 0);
  EXPECT_EQ(status.publishCalls, 0);
}

TEST(DashboardBleProbe, BadCrcDoesNotRender) {
  FakeScreen screen;
  FakeStatusSink status;
  probe::DashboardBleProbe controller(screen, status);
  auto frame = makeFrame(17, {'h', 'e', 'l', 'l', 'o'});
  frame.back() ^= 1;
  ASSERT_TRUE(controller.enqueue(frame.data(), frame.size()));
  controller.loop();
  EXPECT_EQ(screen.renderCalls, 0);
  EXPECT_EQ(status.last, probe::Status::BadCrc);
  EXPECT_TRUE(status.lastHasMessageId);
  EXPECT_EQ(status.lastMessageId, 17u);
}

TEST(DashboardBleProbe, ValidFrameRendersBeforeAccepted) {
  FakeScreen screen;
  FakeStatusSink status;
  probe::DashboardBleProbe controller(screen, status);
  const auto frame = makeFrame(23, {'a', 'b', 'c'});
  ASSERT_TRUE(controller.enqueue(frame.data(), frame.size()));
  controller.loop();
  EXPECT_EQ(screen.renderCalls, 1);
  EXPECT_EQ(screen.lastMessageId, 23u);
  EXPECT_EQ(screen.lastPayloadLength, 3);
  EXPECT_EQ(status.last, probe::Status::Accepted);
  EXPECT_EQ(status.lastMessageId, 23u);
}

TEST(DashboardBleProbe, FailedRenderPublishesRenderFailed) {
  FakeScreen screen;
  screen.result = false;
  FakeStatusSink status;
  probe::DashboardBleProbe controller(screen, status);
  const auto frame = makeFrame(29, {'x'});
  ASSERT_TRUE(controller.enqueue(frame.data(), frame.size()));
  controller.loop();
  EXPECT_EQ(screen.renderCalls, 1);
  EXPECT_EQ(status.last, probe::Status::RenderFailed);
  EXPECT_EQ(status.lastMessageId, 29u);
}

TEST(DashboardBleProbe, RejectsSecondWriteWhileOneIsPending) {
  FakeScreen screen;
  FakeStatusSink status;
  probe::DashboardBleProbe controller(screen, status);
  const auto first = makeFrame(31, {'a'});
  const auto second = makeFrame(32, {'b'});
  ASSERT_TRUE(controller.enqueue(first.data(), first.size()));
  EXPECT_FALSE(controller.enqueue(second.data(), second.size()));
  controller.loop();
  EXPECT_EQ(screen.lastMessageId, 31u);
}

TEST(DashboardBleProbe, RejectsNullEmptyAndOversizedWrites) {
  FakeScreen screen;
  FakeStatusSink status;
  probe::DashboardBleProbe controller(screen, status);
  std::vector<uint8_t> oversized(probe::kMaxFrameBytes + 1, 0);
  EXPECT_FALSE(controller.enqueue(nullptr, 1));
  EXPECT_FALSE(controller.enqueue(oversized.data(), 0));
  EXPECT_FALSE(controller.enqueue(oversized.data(), oversized.size()));
}
