# Standby Dashboard Refresh Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When a session ends and the dashboard card is stale, sleep for two seconds instead of to the next quarter-hour tick, so the existing timer-wake path opens one BLE window and the user's sleep screen is fresh.

**Architecture:** Four pure functions in `AgendaWakePolicy` carry every decision, so they are checkable on the host. `main.cpp` only wires them: `enterDeepSleepInternal` picks a different `timerWakeUs`, and the in-process receiver branch gains the sleep-to-next-tick handler its partition-route sibling already has. No new receive code and no new render code.

**Tech Stack:** C++20, ESP32-C3 (Arduino/ESP-IDF via pioarduino), GoogleTest on the host via CMake.

**Spec:** `docs/superpowers/specs/2026-08-30-standby-dashboard-refresh-design.md`

---

## Baseline (measured 2026-08-30)

- Host tests: **194 tests, 193 passed, 1 skipped** (`DashboardV3Renderer.MaximumContentWritesPbmArtifactWhenEnvDirIsSet`, skipped unless an env dir is set). Green.
- Build and run:
  ```bash
  cd build/tests && cmake --build . --target BleHandoffRecordTest -j8 && ./ble_handoff_record/BleHandoffRecordTest
  ```
- Firmware build: `pio run -e dashboard-x3` (runs `scripts/check_firmware_size.py` post-build; app0 is 6,553,600 bytes and the reader was last measured at 6,335,728).

## Rules for whoever implements this

- **Suspect this plan.** If a step contradicts the code you are looking at, stop and report BLOCKED with the file and line as evidence. Do not improvise a fix and do not adjust a test to make it pass.
- **Stack discipline.** `PersistedPackage` is ~1050 bytes (`MAX_PACKAGE_SIZE = 1024`). CLAUDE.md line 47 says anything meaningfully over 256 bytes on the stack must be justified. Task 6 puts one in a **scoped block** on purpose, so it is released before `powerManager.startDeepSleep`. Do not hoist it to the top of the function.
- **Host tests are not proof for hardware.** Nothing here can be declared verified on the device; the hardware pass is the spec's, and the user runs it.
- Format touched files: `find src lib include test -name "*.cpp" -o -name "*.h" | xargs clang-format -i`.

## File structure

| File | Responsibility | Change |
| --- | --- | --- |
| `src/spikes/ble_handoff/AgendaWakePolicy.h` | Declarations of the wake-cycle pure functions | Modify: 3 functions, 1 enum, 1 constant |
| `src/spikes/ble_handoff/AgendaWakePolicy.cpp` | Their implementations | Modify |
| `test/ble_handoff_record/AgendaWakePolicyTest.cpp` | Host tests for the above | Modify |
| `src/spikes/ble_handoff/BleHandoffTrace.h` | Boot trace stages | Modify: 1 enum value |
| `src/spikes/ble_handoff/BleHandoffTrace.cpp` | Stage names in the trace line | Modify: 1 case |
| `src/main.cpp` | Wiring only | Modify: 2 sites |
| `platformio.ini` | Build flag | Modify: `env:dashboard-x3` |

---

### Task 1: UTC epoch seconds from the RTC's civil reading

`HalClock` has no epoch getter — only `getDateTime(year, month, day, hour, minute)` in UTC, at minute resolution. The staleness rule compares against `PackageHeader::generatedAt`, which is UTC epoch seconds. The conversion is the missing piece, and it belongs beside `localMinuteOfDay` for the reason that function documents: the caller runs before the SD mount, and a pure function is the part that can be checked on the host.

`DashboardV3Renderer.cpp:115` already has `civilFromDays` (Howard Hinnant). This is its inverse. It is in an anonymous namespace there, so it cannot be reused; do not try.

**Files:**
- Modify: `src/spikes/ble_handoff/AgendaWakePolicy.h`
- Modify: `src/spikes/ble_handoff/AgendaWakePolicy.cpp`
- Test: `test/ble_handoff_record/AgendaWakePolicyTest.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `test/ble_handoff_record/AgendaWakePolicyTest.cpp`:

```cpp
TEST(UtcEpochSecondsFromCivil, ConvertsAKnownInstant) {
  // 2026-08-30T12:00:00Z. Day 20695 since the epoch: 20695 * 86400 + 43200.
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2026, 8, 30, 12, 0), 1788091200ULL);
}

TEST(UtcEpochSecondsFromCivil, ConvertsTheEpochItself) {
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(1970, 1, 1, 0, 0), 0ULL);
}

TEST(UtcEpochSecondsFromCivil, HandlesALeapDay) {
  // 2024-02-29T00:00:00Z = 1709164800.
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2024, 2, 29, 0, 0), 1709164800ULL);
}

TEST(UtcEpochSecondsFromCivil, ReadsTheUnsetRtcDateAsAVeryOldTime) {
  // An RTC that was never set reads 2000-01-01 on this hardware. That is a
  // valid date, so it converts; the staleness rule is what rejects it, by
  // finding a package generated after it.
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2000, 1, 1, 0, 0), 946684800ULL);
}

TEST(UtcEpochSecondsFromCivil, RejectsOutOfRangeFieldsWithZero) {
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2026, 0, 30, 12, 0), 0ULL);
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2026, 13, 30, 12, 0), 0ULL);
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2026, 8, 0, 12, 0), 0ULL);
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2026, 8, 32, 12, 0), 0ULL);
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2026, 8, 30, 24, 0), 0ULL);
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(2026, 8, 30, 12, 60), 0ULL);
  EXPECT_EQ(dashboard::utcEpochSecondsFromCivil(1969, 8, 30, 12, 0), 0ULL);
}
```

- [ ] **Step 2: Run to verify they fail**

```bash
cd build/tests && cmake --build . --target BleHandoffRecordTest -j8
```
Expected: compile error, `utcEpochSecondsFromCivil` is not a member of `dashboard`.

- [ ] **Step 3: Declare it**

In `src/spikes/ble_handoff/AgendaWakePolicy.h`, after the `localMinuteOfDay` declaration:

```cpp
// The RTC's UTC reading as Unix epoch seconds, so it can be compared against a
// package's `generatedAt`. The RTC exposes no seconds field, so the result
// lands on the minute - far finer than the staleness rule needs.
//
// Returns 0 for any field out of range, and for years before 1970. A caller
// cannot tell that apart from midnight on 1970-01-01, which is deliberate:
// both mean "no usable time", and the one caller treats 0 as unknown.
uint64_t utcEpochSecondsFromCivil(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute);
```

- [ ] **Step 4: Implement it**

In `src/spikes/ble_handoff/AgendaWakePolicy.cpp`, inside `namespace dashboard`, in the file's anonymous namespace for the helper:

```cpp
namespace {

// Howard Hinnant's days_from_civil against the Unix epoch: the inverse of the
// civil_from_days in DashboardV3Renderer.cpp. Pure integer arithmetic, no
// <ctime> and no timezone database, so it runs identically on the host and on
// the C3. Valid for any proleptic Gregorian date; the caller range-checks.
int64_t daysFromCivil(int64_t y, const unsigned m, const unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

}  // namespace
```

If the file already has an anonymous namespace, add `daysFromCivil` to it rather than opening a second one.

Then the public function:

```cpp
uint64_t utcEpochSecondsFromCivil(const uint16_t year, const uint8_t month, const uint8_t day, const uint8_t hour,
                                  const uint8_t minute) {
  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59) return 0;
  const int64_t days = daysFromCivil(static_cast<int64_t>(year), month, day);
  if (days < 0) return 0;
  return static_cast<uint64_t>(days) * 86400ULL + static_cast<uint64_t>(hour) * 3600ULL +
         static_cast<uint64_t>(minute) * 60ULL;
}
```

- [ ] **Step 5: Run to verify they pass**

```bash
cd build/tests && cmake --build . --target BleHandoffRecordTest -j8 && ./ble_handoff_record/BleHandoffRecordTest
```
Expected: PASS, 199 tests ran, 198 passed, 1 skipped (194 + the 5 added here).

- [ ] **Step 6: Commit**

```bash
git add src/spikes/ble_handoff/AgendaWakePolicy.h src/spikes/ble_handoff/AgendaWakePolicy.cpp test/ble_handoff_record/AgendaWakePolicyTest.cpp
git commit -m "feat: convert the RTC's civil UTC reading to epoch seconds"
```

---

### Task 2: The staleness rule

**Files:**
- Modify: `src/spikes/ble_handoff/AgendaWakePolicy.h`
- Modify: `src/spikes/ble_handoff/AgendaWakePolicy.cpp`
- Test: `test/ble_handoff_record/AgendaWakePolicyTest.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `test/ble_handoff_record/AgendaWakePolicyTest.cpp`:

```cpp
namespace {
// 2026-08-30T12:00:00Z, the instant used as "now" throughout these tests.
constexpr uint64_t NOW = 1788091200ULL;
}  // namespace

TEST(ShouldRefreshAtStandby, RefreshesWhenTheCardIsOlderThanTheInterval) {
  EXPECT_TRUE(dashboard::shouldRefreshAtStandby(true, true, NOW, NOW - 3600, 15));
}

TEST(ShouldRefreshAtStandby, RefreshesExactlyAtTheInterval) {
  EXPECT_TRUE(dashboard::shouldRefreshAtStandby(true, true, NOW, NOW - 15 * 60, 15));
}

TEST(ShouldRefreshAtStandby, LeavesAFreshCardAlone) {
  // A glance: picked up and put down again well inside the interval.
  EXPECT_FALSE(dashboard::shouldRefreshAtStandby(true, true, NOW, NOW - 14 * 60, 15));
}

TEST(ShouldRefreshAtStandby, DoesNotRefreshAfterAJustAcceptedPackage) {
  // This is what stops the loop: the boot that renders an accepted package
  // sleeps through this same rule, seconds after the package was generated.
  EXPECT_FALSE(dashboard::shouldRefreshAtStandby(true, true, NOW, NOW - 5, 15));
}

TEST(ShouldRefreshAtStandby, NeverRefreshesOnANonAgendaSleep) {
  EXPECT_FALSE(dashboard::shouldRefreshAtStandby(false, true, NOW, NOW - 3600, 15));
}

TEST(ShouldRefreshAtStandby, NeverRefreshesWithoutAClock) {
  EXPECT_FALSE(dashboard::shouldRefreshAtStandby(true, false, NOW, NOW - 3600, 15));
}

TEST(ShouldRefreshAtStandby, NeverRefreshesWithoutAPackage) {
  EXPECT_FALSE(dashboard::shouldRefreshAtStandby(true, true, NOW, 0, 15));
}

TEST(ShouldRefreshAtStandby, NeverRefreshesWhenTheClockIsBehindThePackage) {
  // An unset RTC reads 2000-01-01, which is before any package it holds.
  EXPECT_FALSE(dashboard::shouldRefreshAtStandby(true, true, 946684800ULL, NOW, 15));
}

TEST(ShouldRefreshAtStandby, NeverRefreshesOnAZeroInterval) {
  EXPECT_FALSE(dashboard::shouldRefreshAtStandby(true, true, NOW, NOW - 3600, 0));
}

TEST(ShouldRefreshAtStandby, RespectsAPhoneSuppliedInterval) {
  // clampWakeSettings allows 1-60 minutes; a 60-minute interval means a
  // 30-minute-old card is still fresh.
  EXPECT_FALSE(dashboard::shouldRefreshAtStandby(true, true, NOW, NOW - 30 * 60, 60));
  EXPECT_TRUE(dashboard::shouldRefreshAtStandby(true, true, NOW, NOW - 61 * 60, 60));
}
```

- [ ] **Step 2: Run to verify they fail**

```bash
cd build/tests && cmake --build . --target BleHandoffRecordTest -j8
```
Expected: compile error, `shouldRefreshAtStandby` is not a member of `dashboard`.

- [ ] **Step 3: Declare it**

In `src/spikes/ble_handoff/AgendaWakePolicy.h`, after `utcEpochSecondsFromCivil`:

```cpp
// Microseconds of deep sleep before the window a standby refresh asks for. Long
// enough that the sleep card has been painted and the panel has settled, short
// enough that the fresh card lands while the user is still putting the device
// down.
constexpr uint64_t STANDBY_REFRESH_DELAY_US = 2ULL * 1000ULL * 1000ULL;

// Whether the sleep now being entered should be cut short to one immediate
// agenda window instead of running to the next tick of the grid.
//
// Every uncertainty resolves to false. A wrong `true` wakes the device every two
// seconds and empties the battery overnight; a wrong `false` leaves a card
// exactly as stale as it is today. The two mistakes are not comparable, so the
// rule declines whenever it cannot establish the age: no clock, no package, or
// a clock reading earlier than the package it holds.
//
// Declining once the package is fresh is also what ends the cycle. The boot that
// renders an accepted package sleeps through this same rule (main.cpp calls
// enterDeepSleepInternal after rendering), and by then the package is seconds
// old - so one put-down buys exactly one window, with no flag to keep in step.
bool shouldRefreshAtStandby(bool agendaSleep, bool clockAvailable, uint64_t nowEpochSeconds,
                            uint64_t packageGeneratedAt, uint32_t intervalMinutes);
```

- [ ] **Step 4: Implement it**

In `src/spikes/ble_handoff/AgendaWakePolicy.cpp`:

```cpp
bool shouldRefreshAtStandby(const bool agendaSleep, const bool clockAvailable, const uint64_t nowEpochSeconds,
                            const uint64_t packageGeneratedAt, const uint32_t intervalMinutes) {
  if (!agendaSleep || !clockAvailable) return false;
  if (packageGeneratedAt == 0 || intervalMinutes == 0) return false;
  if (nowEpochSeconds < packageGeneratedAt) return false;
  return (nowEpochSeconds - packageGeneratedAt) >= static_cast<uint64_t>(intervalMinutes) * 60ULL;
}
```

- [ ] **Step 5: Run to verify they pass**

```bash
cd build/tests && cmake --build . --target BleHandoffRecordTest -j8 && ./ble_handoff_record/BleHandoffRecordTest
```
Expected: PASS, 209 tests ran, 208 passed, 1 skipped (199 + the 10 added here).

- [ ] **Step 6: Commit**

```bash
git add src/spikes/ble_handoff/AgendaWakePolicy.h src/spikes/ble_handoff/AgendaWakePolicy.cpp test/ble_handoff_record/AgendaWakePolicyTest.cpp
git commit -m "feat: decide when a standby sleep is worth one agenda window"
```

---

### Task 3: What the reader does after an in-process window

**Files:**
- Modify: `src/spikes/ble_handoff/AgendaWakePolicy.h`
- Modify: `src/spikes/ble_handoff/AgendaWakePolicy.cpp`
- Test: `test/ble_handoff_record/AgendaWakePolicyTest.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `test/ble_handoff_record/AgendaWakePolicyTest.cpp`:

```cpp
TEST(ActionAfterInProcessWindow, AnAcceptedPackageEarnsAFullBoot) {
  EXPECT_EQ(dashboard::actionAfterInProcessWindow(dashboard::ReceiverResult::Accepted),
            dashboard::InProcessWindowAction::ContinueBoot);
}

TEST(ActionAfterInProcessWindow, ATimedOutWindowSleepsToTheNextTick) {
  EXPECT_EQ(dashboard::actionAfterInProcessWindow(dashboard::ReceiverResult::TimedOut),
            dashboard::InProcessWindowAction::SleepToNextTick);
}

TEST(ActionAfterInProcessWindow, AVerdictlessWindowSleepsToTheNextTick) {
  // The receiver produced no verdict at all. From the user's side nothing
  // arrived, which is the same outcome as a timeout.
  EXPECT_EQ(dashboard::actionAfterInProcessWindow(dashboard::ReceiverResult::None),
            dashboard::InProcessWindowAction::SleepToNextTick);
  EXPECT_EQ(dashboard::actionAfterInProcessWindow(dashboard::ReceiverResult::AwaitingWindow),
            dashboard::InProcessWindowAction::SleepToNextTick);
}
```

- [ ] **Step 2: Run to verify they fail**

```bash
cd build/tests && cmake --build . --target BleHandoffRecordTest -j8
```
Expected: compile error, `InProcessWindowAction` is not a member of `dashboard`.

- [ ] **Step 3: Declare it**

In `src/spikes/ble_handoff/AgendaWakePolicy.h`, beside the other enums at the top of the namespace:

```cpp
enum class InProcessWindowAction : uint8_t { ContinueBoot, SleepToNextTick };
```

and after `shouldRefreshAtStandby`:

```cpp
// What the reader does once an in-process agenda window has closed.
//
// Only an accepted package earns the rest of the boot, because only then is
// there a new card to render. Every other outcome means nothing arrived, and
// booting on would land the reader's home screen on a panel whose whole purpose
// is to show the dashboard - and hold the device awake until the inactivity
// timeout. The partition route has always done this (main.cpp, the
// returnedFromReceiver branch); the in-process route did not.
InProcessWindowAction actionAfterInProcessWindow(ReceiverResult result);
```

- [ ] **Step 4: Implement it**

In `src/spikes/ble_handoff/AgendaWakePolicy.cpp`:

```cpp
InProcessWindowAction actionAfterInProcessWindow(const ReceiverResult result) {
  return result == ReceiverResult::Accepted ? InProcessWindowAction::ContinueBoot
                                            : InProcessWindowAction::SleepToNextTick;
}
```

- [ ] **Step 5: Run to verify they pass**

```bash
cd build/tests && cmake --build . --target BleHandoffRecordTest -j8 && ./ble_handoff_record/BleHandoffRecordTest
```
Expected: PASS, 212 tests ran, 211 passed, 1 skipped (209 + the 3 added here).

- [ ] **Step 6: Commit**

```bash
git add src/spikes/ble_handoff/AgendaWakePolicy.h src/spikes/ble_handoff/AgendaWakePolicy.cpp test/ble_handoff_record/AgendaWakePolicyTest.cpp
git commit -m "feat: decide what follows an in-process agenda window"
```

---

### Task 4: A trace stage for a standby-requested window

Without this the trace cannot tell a standby window from a grid tick, and a week of running produces no evidence about whether the feature works.

The line is written at standby time, before the sleep, not on the wake that follows. That is why no RTC flag is needed: the standby line sits immediately before the `handoff` line it caused, about two seconds earlier, which is unmistakable in the file.

**Files:**
- Modify: `src/spikes/ble_handoff/BleHandoffTrace.h:14-19`
- Modify: `src/spikes/ble_handoff/BleHandoffTrace.cpp:89-99`

- [ ] **Step 1: Add the enum value**

In `src/spikes/ble_handoff/BleHandoffTrace.h`, extend the enum. Append it last, so no existing value's number shifts and old trace files stay comparable:

```cpp
enum class BootTraceStage : uint8_t {
  Full,                     // reached the normal SD mount and carried on booting
  ReceiverHandoff,          // timer wake, about to restart into the receiver partition
  ReceiverTimedOut,         // came back from the receiver empty-handed
  PowerButtonRejected,      // wake failed its hold check; sleeps again WITHOUT a timer
  StandbyRefreshRequested,  // going to sleep on a stale card; asking for one window first
};
```

- [ ] **Step 2: Add the stage name**

In `src/spikes/ble_handoff/BleHandoffTrace.cpp`, in `stageName`:

```cpp
    case BootTraceStage::StandbyRefreshRequested:
      return "standby";
```

**This step is not optional and the compiler will not catch it.** `stageName` ends in `default: return "full";`, so a missing case silently labels every standby line as an ordinary full boot — the exact confusion the stage exists to prevent.

- [ ] **Step 3: Verify the host tests still build and pass**

```bash
cd build/tests && cmake --build . --target BleHandoffRecordTest -j8 && ./ble_handoff_record/BleHandoffRecordTest
```
Expected: PASS, 212 tests ran, 211 passed, 1 skipped — unchanged from Task 3. (`BleHandoffTrace.cpp` is not in the host target, so this only proves nothing regressed.)

- [ ] **Step 4: Commit**

```bash
git add src/spikes/ble_handoff/BleHandoffTrace.h src/spikes/ble_handoff/BleHandoffTrace.cpp
git commit -m "feat: name a standby-requested window in the boot trace"
```

---

### Task 5: Send an empty in-process window straight back to sleep

This is a fix to existing behaviour, wanted whether or not the rest of the feature ships. It is therefore **not** behind the build flag.

It mirrors `src/main.cpp:990-1004`, the partition route's handler, and must keep all three things that sibling does and that are easy to drop: write a trace line, re-arm `AwaitingWindow` before sleeping, and read the UTC offset from the NVS mirror because the SD card is not mounted yet.

**Files:**
- Modify: `src/main.cpp:1073-1088` (the `CROSSINK_IN_PROCESS_RECEIVER` branch)

- [ ] **Step 1: Read the sibling first**

Read `src/main.cpp:988-1010`. Confirm for yourself that `resolveWakeSettings()` and `readClockUtcOffsetQFromNvs()` are both already called there, before the SD mount. If either is not, report BLOCKED — the new handler runs in the same pre-mount context and depends on that being true.

- [ ] **Step 2: Replace the branch**

Replace the body of the `else if (agendaRoute == dashboard::AgendaBootRoute::InProcessReceiver)` branch with:

```cpp
  else if (agendaRoute == dashboard::AgendaBootRoute::InProcessReceiver) {
    LOG_INF("BLEPAY", "Agenda timer wake; running in-process dashboard receiver");
    // Same trace stage as the partition route so old and new traces keep the
    // same shape and can be compared side by side.
    dashboard::appendEarlyBootTrace(static_cast<uint8_t>(wakeupReason), retainedResult,
                                    dashboard::BootTraceStage::ReceiverHandoff, resetReasonName(rawResetReason));
    const auto result = dashboard::runReceiverWindow(20000);
    if (result == dashboard::ReceiverResult::Accepted) {
      dashboard::notifyReceiverStatus(0x03);
      resumeAgendaAfterAccepted = true;
    }
    dashboard::teardownReceiver();
    dashboard::retainReceiverResult(result);

    // Nothing arrived, so there is no new card to render and no reason to bring
    // the reader up. Booting on would leave the home screen on the panel and
    // hold the device awake until the inactivity timeout - once every four or
    // five wakes, at the acceptance rates measured in late August.
    if (dashboard::actionAfterInProcessWindow(result) == dashboard::InProcessWindowAction::SleepToNextTick) {
      LOG_INF("BLEPAY", "In-process window closed empty; returning directly to Agenda sleep");
      dashboard::appendEarlyBootTrace(static_cast<uint8_t>(wakeupReason), result,
                                      dashboard::BootTraceStage::ReceiverTimedOut, resetReasonName(rawResetReason));
      // Re-armed before sleeping, exactly as the partition route does: without
      // this the next timer wake finds no armed cycle and skips its window.
      dashboard::retainReceiverResult(dashboard::ReceiverResult::AwaitingWindow);
      uint8_t agendaWakeHour = 12;
      uint8_t agendaWakeMinute = 0;
      // Pre-SD: SETTINGS is not loaded here, so the offset comes from its NVS mirror.
      resolveAgendaWakeLocalTime(agendaWakeHour, agendaWakeMinute, readClockUtcOffsetQFromNvs());
      const dashboard::WakeSettings wakeSettings = resolveWakeSettings();
      powerManager.startDeepSleep(gpio, dashboard::sleepTimerIntervalUs(true, agendaWakeHour, agendaWakeMinute,
                                                                        wakeSettings.intervalMinutes,
                                                                        wakeSettings.windowStartHour,
                                                                        wakeSettings.windowEndHour));
    }
  }
```

- [ ] **Step 3: Build the firmware**

```bash
pio run -e dashboard-x3
```
Expected: SUCCESS, and `check_firmware_size.py` reports the app fits in 6,553,600 bytes.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "fix: send an empty in-process window back to sleep instead of booting the reader"
```

---

### Task 6: Ask for one window when the card is stale at standby

**Files:**
- Modify: `src/main.cpp:824-834` (the `CROSSINK_BLE_HANDOFF_READER` block in `enterDeepSleepInternal`). `uint64_t timerWakeUs = 0;` at `src/main.cpp:822` sits **outside** the `#ifdef` and stays where it is.

- [ ] **Step 1: Replace the block**

Replace the block that currently reads `const bool agendaSleep = ...` through `dashboard::retainReceiverResult(...)` with:

```cpp
  const bool agendaSleep = SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::AGENDA_SLEEP;
  uint8_t agendaWakeHour = 12;
  uint8_t agendaWakeMinute = 0;
  // SETTINGS is loaded on this path, and the mirror above was just written from it.
  if (agendaSleep) resolveAgendaWakeLocalTime(agendaWakeHour, agendaWakeMinute, SETTINGS.clockUtcOffsetQ);

  // Scoped: PersistedPackage is ~1050 bytes and CLAUDE.md caps unjustified
  // stack locals at 256. Reading it here rather than calling resolveWakeSettings()
  // keeps it to one copy, and the block ends before the sleep call so the frame
  // is back down by then.
  dashboard::WakeSettings wakeSettings{};
  bool refreshAtStandby = false;
  {
    dashboard::PersistedPackage persisted;
    const bool packageRead = dashboard::readLastKnownGood(persisted) == dashboard::PersistStatus::Ok;
    if (packageRead) {
      wakeSettings = dashboard::clampWakeSettings(persisted.header.refreshIntervalMinutes,
                                                  persisted.header.wakeWindowStartHour,
                                                  persisted.header.wakeWindowEndHour);
    }
#ifdef CROSSINK_STANDBY_REFRESH
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t utcHour = 0;
    uint8_t utcMinute = 0;
    // getDateTime returns the raw RTC reading, which this device keeps in UTC -
    // the same basis as the package's generatedAt, so no offset is involved.
    const bool clockAvailable =
        halClock.isAvailable() && halClock.getDateTime(year, month, day, utcHour, utcMinute);
    const uint64_t nowEpochSeconds =
        clockAvailable ? dashboard::utcEpochSecondsFromCivil(year, month, day, utcHour, utcMinute) : 0;
    refreshAtStandby =
        dashboard::shouldRefreshAtStandby(agendaSleep, clockAvailable, nowEpochSeconds,
                                          packageRead ? persisted.header.generatedAt : 0,
                                          wakeSettings.intervalMinutes);
#endif
  }

  if (refreshAtStandby) {
    LOG_INF("BLEPAY", "Stale card at standby; asking for one window before the grid resumes");
    dashboard::appendBootTrace(static_cast<uint8_t>(HalGPIO::WakeupReason::Other),
                               dashboard::ReceiverResult::AwaitingWindow,
                               dashboard::BootTraceStage::StandbyRefreshRequested, "standby", true);
    timerWakeUs = dashboard::STANDBY_REFRESH_DELAY_US;
  } else {
    timerWakeUs = dashboard::sleepTimerIntervalUs(agendaSleep, agendaWakeHour, agendaWakeMinute,
                                                  wakeSettings.intervalMinutes, wakeSettings.windowStartHour,
                                                  wakeSettings.windowEndHour);
  }
  dashboard::retainReceiverResult(agendaSleep ? dashboard::ReceiverResult::AwaitingWindow
                                              : dashboard::ReceiverResult::None);
```

Two things to preserve, both easy to lose while moving this code:

- `retainReceiverResult` still runs on **both** paths. Without an armed cycle the shortened sleep wakes into `chooseAgendaBootRoute` and takes the plain reader route, opening no window at all — the feature would appear to do nothing while looking correct.
- When `refreshAtStandby` is false the behaviour must be byte-for-byte what it is today, including for a non-agenda sleep, where `sleepTimerIntervalUs(false, ...)` returns 0 and the device sleeps without a timer.

- [ ] **Step 2: Confirm the include is present**

`enterDeepSleepInternal` now calls `dashboard::readLastKnownGood` and `dashboard::clampWakeSettings` directly. Check that `src/main.cpp` already includes the headers declaring them (`BleHandoffNvs.h` and `AgendaWakePolicy.h`). `resolveWakeSettings()` at `src/main.cpp:778` already uses both, so they should be there; if either is missing, add it beside the existing `spikes/ble_handoff/` includes near `src/main.cpp:115`.

- [ ] **Step 3: Build the firmware**

```bash
pio run -e dashboard-x3
```
Expected: SUCCESS. `CROSSINK_STANDBY_REFRESH` is not defined yet, so the guarded block compiles out and `refreshAtStandby` stays false — the build must still succeed, with no unused-variable warning for `refreshAtStandby` since it is read below.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat: ask for one agenda window when the card is stale at standby"
```

---

### Task 7: Turn the feature on

**Files:**
- Modify: `platformio.ini`, `env:dashboard-x3`

- [ ] **Step 1: Add the flag**

In `platformio.ini`, in `[env:dashboard-x3]`, extend `build_flags`:

```ini
build_flags =
  ${env:spike-ble-reader-x3.build_flags}
  -DCROSSINK_IN_PROCESS_RECEIVER=1
; A session that ends on a card older than the wake interval buys one immediate
; window instead of waiting for the next grid tick. Behind a flag because the
; failure mode is a battery-draining wake loop, so rolling back has to be a
; rebuild rather than a revert.
  -DCROSSINK_STANDBY_REFRESH=1
```

- [ ] **Step 2: Build with the feature on**

```bash
pio run -e dashboard-x3
```
Expected: SUCCESS, and `check_firmware_size.py` reports the app still fits in 6,553,600 bytes. Report the new firmware size; the last measurement was 6,335,728 bytes, leaving 217,872.

- [ ] **Step 3: Build once with the feature off, to prove the flag works**

```bash
pio run -e spike-ble-reader-x3
```
Expected: SUCCESS. This env has neither `CROSSINK_IN_PROCESS_RECEIVER` nor `CROSSINK_STANDBY_REFRESH`, so it proves the guarded code compiles out cleanly.

- [ ] **Step 4: Run the full host suite one more time**

```bash
cd build/tests && cmake --build . --target BleHandoffRecordTest -j8 && ./ble_handoff_record/BleHandoffRecordTest
```
Expected: PASS, 212 tests ran, 211 passed, 1 skipped — unchanged from Task 3.

- [ ] **Step 5: Commit**

```bash
git add platformio.ini
git commit -m "feat: enable the standby dashboard refresh on the dashboard build"
```

---

## Not in this plan

- **Hardware verification.** The spec lists it and the user runs it on the device. Nothing above may be reported as confirmed on hardware.
- **The 16 MB flash backup.** Must be taken before this is flashed; it does not exist yet.
- **Keeping the radio alive while reading.** A separate measurement, described at the end of the spec. Do not start it here, and do not change `teardownReceiver`, `releaseBluetoothMemory`, or their one-shot guards.
