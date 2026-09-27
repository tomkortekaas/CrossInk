# X3 SD Sleep Shutdown Specification

## Goal

Reduce X3 timer-sleep battery drain by ending the mounted SD/SPI session before deep sleep while keeping the existing 15-minute dashboard schedule unchanged for an A/B measurement.

## Requirements

- Every normal deep-sleep path through `enterDeepSleepInternal()` must stop storage only after all possible SD-backed logging and state writes have finished.
- `HalStorage` must expose one thread-safe shutdown operation.
- The current SPI/SdFat backend must flush/end the volume and SPI card session, then mark its cached state uninitialized.
- The SDMMC backend must also remain safe and idempotent: end the volume and host, float bus pads, and clear cached state.
- Do not change the dashboard refresh interval, standby-refresh behavior, dashboard UI, or existing user edits.
- Prove the new API wiring first with a target-build failure, then a successful clean `dashboard-x3` target build.
- Hardware battery improvement remains unverified until a 48-72 hour unplugged measurement is collected.

## Evidence

- Baseline log: `/Volumes/2TB/X3/Logs/2026-09-21/crossink-ble-trace-2026-09-21.txt`
- The X3 timer-sleep path keeps GPIO13 asserted because it is both the battery latch and the declared SD power enable.
- The existing upstream SDMMC shutdown fix is not sufficient for this custom X3 timer-sleep mode because upstream treats SPI/X3 shutdown as a no-op under the assumption that sleep cuts the full rail.
