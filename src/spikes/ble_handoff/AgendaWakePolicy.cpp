#include "AgendaWakePolicy.h"

namespace dashboard {
namespace {

constexpr uint64_t AGENDA_WAKE_INTERVAL_US = 15ULL * 60ULL * 1000000ULL;
constexpr uint32_t RECEIVER_RESULT_WORD_MAGIC = 0xA63E4D00U;

bool isPersistableResult(const ReceiverResult result) {
  return result == ReceiverResult::AwaitingWindow || result == ReceiverResult::Accepted ||
         result == ReceiverResult::TimedOut;
}

}  // namespace

uint64_t sleepTimerIntervalUs(const bool agendaSleep) { return agendaSleep ? AGENDA_WAKE_INTERVAL_US : 0; }

AgendaBootRoute chooseAgendaBootRoute(const bool agendaCycleArmed, const WakeSource wakeSource) {
  return agendaCycleArmed && wakeSource == WakeSource::Timer ? AgendaBootRoute::Receiver
                                                             : AgendaBootRoute::NormalReader;
}

uint32_t encodeReceiverResultWord(const ReceiverResult result) {
  return isPersistableResult(result) ? RECEIVER_RESULT_WORD_MAGIC | static_cast<uint32_t>(result) : 0;
}

ReceiverResult decodeReceiverResultWord(const uint32_t word) {
  if ((word & 0xFFFFFF00U) != RECEIVER_RESULT_WORD_MAGIC) return ReceiverResult::None;
  const auto result = static_cast<ReceiverResult>(word & 0xFFU);
  return isPersistableResult(result) ? result : ReceiverResult::None;
}

}  // namespace dashboard
