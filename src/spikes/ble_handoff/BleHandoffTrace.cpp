#include "BleHandoffTrace.h"

#ifdef CROSSINK_BLE_HANDOFF_READER

#include <HalClock.h>
#include <HalStorage.h>

#include <cstdio>

#include "Logging.h"

namespace dashboard {
namespace {

constexpr const char* TRACE_PATH = "/crossink-ble-trace.txt";
// Roughly 1200 lines at ~70 bytes. A minute-interval test env fills that in a
// day; past it the file is restarted rather than grown without bound, because
// nothing here is worth more than a day of history and an SD card that fills up
// is a worse failure than a truncated log.
constexpr uint32_t MAX_TRACE_BYTES = 80000;

const char* wakeName(const uint8_t reason) {
  // Mirrors HalGPIO::WakeupReason's order. Taken as a uint8_t so this file does
  // not pull in the GPIO HAL just to name a value.
  switch (reason) {
    case 0:
      return "button";
    case 1:
      return "timer";
    case 2:
      return "afterflash";
    case 3:
      return "usbpower";
    default:
      return "other";
  }
}

const char* resultName(const ReceiverResult result) {
  switch (result) {
    case ReceiverResult::Accepted:
      return "accepted";
    case ReceiverResult::TimedOut:
      return "timedout";
    case ReceiverResult::AwaitingWindow:
      return "awaiting";
    default:
      return "none";
  }
}

}  // namespace

void appendBootTrace(const uint8_t wakeupReason, const ReceiverResult retainedResult, const bool storageReady) {
  if (!storageReady) return;

  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  // UTC, deliberately: SETTINGS is not loaded this early in boot, so the offset
  // is unavailable. A consistent clock beats a local one that silently reads as
  // UTC on the boots where settings failed to load.
  const bool haveClock = halClock.getDateTime(year, month, day, hour, minute);

  char line[96];
  const int written =
      haveClock ? snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u UTC wake=%s receiver=%s\n", year, month, day,
                           hour, minute, wakeName(wakeupReason), resultName(retainedResult))
                : snprintf(line, sizeof(line), "(no clock) +%lums wake=%s receiver=%s\n",
                           static_cast<unsigned long>(millis()), wakeName(wakeupReason), resultName(retainedResult));
  if (written <= 0) return;

  if (Storage.exists(TRACE_PATH)) {
    HalFile existing = Storage.open(TRACE_PATH, O_RDONLY);
    const bool oversized = existing && existing.size() > MAX_TRACE_BYTES;
    if (existing) existing.close();
    if (oversized && !Storage.remove(TRACE_PATH)) {
      LOG_ERR("BLETRACE", "Could not rotate oversized trace file");
    }
  }

  HalFile file = Storage.open(TRACE_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("BLETRACE", "Could not open %s for append", TRACE_PATH);
    return;
  }
  file.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(written));
  file.close();
}

}  // namespace dashboard

#endif
