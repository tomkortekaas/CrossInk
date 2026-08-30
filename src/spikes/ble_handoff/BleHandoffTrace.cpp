#include "BleHandoffTrace.h"

#ifdef CROSSINK_BLE_HANDOFF_READER

#include <BoardConfig.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Wire.h>

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

// BQ27220 standard command registers. RemainingCapacity is what makes a battery
// question answerable at all: the gauge is a coulomb counter and keeps
// integrating while the ESP32 is in deep sleep, so the difference between two
// wakes is the charge that whole interval cost, sleep included. State-of-charge
// is only whole percents — about 15 mAh a step here — which is coarser than a
// night of standby, so it would read the same before and after and say nothing.
//
// Read straight off the bus rather than through BatteryMonitor because that
// lives in the freeink-sdk submodule, which has no fork and carries changes as
// patches; diagnostics are not worth another one. Safe here because the RTC
// read just above shares this bus and has already brought Wire up.
constexpr uint8_t GAUGE_VOLTAGE_MV = 0x08;
constexpr uint8_t GAUGE_REMAINING_MAH = 0x10;

bool readGaugeWord(const uint8_t reg, uint16_t& out) {
  const auto& gauge = BoardConfig::ACTIVE.batteryGauge;
  if (gauge.gaugeAddr == 0) return false;
  const auto addr = static_cast<uint8_t>(gauge.gaugeAddr);
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2)) != 2) return false;
  const uint8_t low = static_cast<uint8_t>(Wire.read());
  const uint8_t high = static_cast<uint8_t>(Wire.read());
  out = static_cast<uint16_t>(low | (high << 8));
  return true;
}

// -1 for a gauge that did not answer, so a bus failure reads differently from a
// genuine zero.
int gaugeValueOrUnknown(const uint8_t reg) {
  uint16_t value = 0;
  return readGaugeWord(reg, value) ? static_cast<int>(value) : -1;
}

const char* stageName(const BootTraceStage stage) {
  switch (stage) {
    case BootTraceStage::ReceiverHandoff:
      return "handoff";
    case BootTraceStage::ReceiverTimedOut:
      return "timedout";
    case BootTraceStage::PowerButtonRejected:
      return "rejected";
    case BootTraceStage::StandbyRefreshRequested:
      return "standby";
    default:
      return "full";
  }
}

}  // namespace

void appendBootTrace(const uint8_t wakeupReason, const ReceiverResult retainedResult, const BootTraceStage stage,
                     const char* resetName, const bool storageReady) {
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

  const int milliVolts = gaugeValueOrUnknown(GAUGE_VOLTAGE_MV);
  const int remainingMah = gaugeValueOrUnknown(GAUGE_REMAINING_MAH);

  char line[160];
  const char* reset = resetName != nullptr ? resetName : "?";
  const int written =
      haveClock ? snprintf(line, sizeof(line),
                           "%04u-%02u-%02u %02u:%02u UTC wake=%s receiver=%s stage=%s reset=%s mv=%d mah=%d\n", year,
                           month, day, hour, minute, wakeName(wakeupReason), resultName(retainedResult),
                           stageName(stage), reset, milliVolts, remainingMah)
                : snprintf(line, sizeof(line), "(no clock) +%lums wake=%s receiver=%s stage=%s reset=%s mv=%d mah=%d\n",
                           static_cast<unsigned long>(millis()), wakeName(wakeupReason), resultName(retainedResult),
                           stageName(stage), reset, milliVolts, remainingMah);
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

void appendEarlyBootTrace(const uint8_t wakeupReason, const ReceiverResult retainedResult, const BootTraceStage stage,
                          const char* resetName) {
  if (!Storage.begin()) {
    LOG_ERR("BLETRACE", "Could not mount storage for an early boot trace");
    return;
  }
  appendBootTrace(wakeupReason, retainedResult, stage, resetName, true);
}

}  // namespace dashboard

#endif
