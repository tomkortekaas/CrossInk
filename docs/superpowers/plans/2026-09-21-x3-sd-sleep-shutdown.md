# X3 SD Sleep Shutdown Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** End the X3 SD/SPI session immediately before deep sleep so timer wakes do not leave the card and SPI host active.

**Architecture:** Add an idempotent shutdown primitive at the SDK storage boundary, wrap it with the existing `HalStorage` mutex, and invoke it at the final point in `enterDeepSleepInternal()` after dashboard trace writes. Keep scheduling unchanged so the archived battery trace remains a valid baseline.

**Tech Stack:** C++20/Arduino ESP32-C3, SdFat, PlatformIO, FreeInk SDK

**Spec:** `docs/superpowers/specs/2026-09-21-x3-sd-sleep-shutdown.md`

## Global Constraints

- Preserve all existing unrelated modified files.
- Do not change refresh timing or standby refresh.
- No claim of battery improvement without a 48-72 hour hardware measurement.

---

### Task 1: Storage shutdown before deep sleep

**Files:**
- Modify: `freeink-sdk/libs/hardware/SDCardManager/include/SDCardManager.h`
- Modify: `freeink-sdk/libs/hardware/SDCardManager/src/SDCardManager.cpp`
- Modify: `lib/hal/HalStorage.h`
- Modify: `lib/hal/HalStorage.cpp`
- Modify: `src/main.cpp`

**Interfaces:**
- Produces: `SDCardManager::shutdown()` and `HalStorage::shutdown()`, both `void` and idempotent.
- Consumes: `Storage.shutdown()` immediately before `powerManager.startDeepSleep(...)`.

- [x] **Step 1: Add only the final `Storage.shutdown()` call and run the target build**

Run: `uvx --from platformio platformio run -e dashboard-x3`

Expected: FAIL because `HalStorage` has no `shutdown` member. This proves the target path compiles the sleep call.

- [x] **Step 2: Add the minimal storage API**

Implement `HalStorage::shutdown()` under `StorageLock`. Implement `SDCardManager::shutdown()` so the SPI backend calls `sd.end()`, the SDMMC backend ends its volume/device and floats its bus pins, and both clear initialized/cached state. The method must return safely if storage is already stopped.

- [x] **Step 3: Run focused host tests**

Run the existing `agenda_wake_policy` CMake/CTest target to ensure the scheduling policy remains unchanged.

- [x] **Step 4: Run a clean target build**

Run: `uvx --from platformio platformio run -e dashboard-x3 --target clean`, followed by `uvx --from platformio platformio run -e dashboard-x3`.

Expected: both commands exit 0; the final binary links the new SDK and application calls.

- [x] **Step 5: Review the complete diff**

Confirm only the five implementation files plus these approved plan/spec files changed, excluding pre-existing user edits. Confirm `Storage.shutdown()` is after every possible SD write and immediately before the power manager sleep call.
