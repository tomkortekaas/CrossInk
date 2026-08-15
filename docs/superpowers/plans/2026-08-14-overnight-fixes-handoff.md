# Overnight fixes — Handoff

**Date:** 2026-08-14 (late evening, fifth session on this feature)
**Supersedes:** `2026-08-14-widget-grid-rejection-debug-handoff.md` — read this one instead.
**Constraint this session:** no X3 and no iPhone available. Everything below was
verified on the host; nothing has been flashed or run on hardware.

## The headline: the previous session's diagnosis was wrong, twice over

### 1. The wire format was never broken

The leading hypothesis in the previous handoff — that the free-form-placement
change (`ba99962` Swift / `7c8121e1` CrossInk) broke the widget-grid wire format
— is **disproved**. A byte-level audit (delegated to DeepSeek, cross-checked by
hand) showed:

- Swift `WidgetGridPackage.encode()` and C++ `encodeWidgetGridPackage()` produce
  **byte-identical output** for the same widgets (verified on a 185-byte package,
  hexdumps compared).
- The two CRC-32 implementations are bit-for-bit equivalent (same reflected
  polynomial, init, final XOR, and byte range). Check value `crc32("123456789")
  == 0xCBF43926` on both sides.
- `SlotRecord` has no padding on either host arm64 or the real RISC-V toolchain
  (`sizeof == 268`, `crc` at offset 264), so the CRC-over-struct is sound.
- The full firmware chain `encode → peekPackageHeader → TransferAssembler →
  persistIfNewer` accepts every realistic widget-grid package in a mock-NVS
  harness.

### 2. The debug line that "never printed" was never on the device

This is the important one. `#include <Logging.h>`, added to `BleHandoffNvs.cpp`
by the previous session to enable its `LOG_ERR` debugging, **breaks the
`spike-ble-receiver-x3` build**: `lib/Logging/Logging.h` includes
`BoardConfig.h`, and that environment's `lib_deps` did not carry the
`BoardConfig` symlink that `[base]` has. Proved by building the same environment
at unmodified `HEAD` in a throwaway worktree (SUCCESS) versus the working tree
(FAILED on `fatal error: BoardConfig.h: No such file or directory`).

Consequences:

- The `LOG_ERR` calls never printed because the code was never on the X3, not
  because of any USB-CDC / `logSerial` readiness problem. **The
  "`HWCDC operator bool` reads false" theory was a red herring** — drop it.
- The raw `Serial.printf("BLE-RX persistIfNewer status=...")` line was in the
  same non-building file set, so it was never flashed either. The previous
  session's repeated failures to "catch" that line were not a timing problem.
- `env:default` cannot flash the receiver at all: `BleReceiverMain.cpp` is
  behind `#ifdef CROSSINK_BLE_HANDOFF_RECEIVER`, which only
  `env:spike-ble-receiver-x3` defines, and that image lives at a separate
  partition offset (`0x650000`). The previous handoff's flashing instructions
  (`pio run -e default -t upload`) update the *reader*, not the receiver.

**Fixed** (uncommitted): `platformio.ini` now gives `env:spike-ble-receiver-x3`
a `lib_deps` entry for `BoardConfig`. Both firmware environments build:
`spike-ble-receiver-x3` SUCCESS (RAM 6.2%, Flash 10.0%) and `default` SUCCESS.

### 3. cmake was available all along

Four sessions recorded "cmake/nix are not available in this sandbox". PlatformIO
ships both: `~/.platformio/packages/tool-cmake/bin/{cmake,ctest}` (cmake 4.0.3)
and `~/.platformio/packages/tool-ninja/ninja`. The firmware test suite
configures and runs:

```bash
export PATH="$HOME/.platformio/packages/tool-cmake/bin:$PATH"
cmake -S test -B /tmp/crossink-test-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/crossink-test-build --target BleHandoffRecordTest -j8
/tmp/crossink-test-build/ble_handoff_record/BleHandoffRecordTest
```

**49 tests pass**, including the previous session's `AgendaWakePolicy` changes,
which had never been run against the real suite until now. Stop using throwaway
`clang++` harnesses for this repo.

## The actual cause of the widget-grid rejection

Not in the firmware. In the iPhone app, at
`X3DashboardApp/BleDashboardSender.swift`:

```swift
private func rebuiltPackage(withPackageId newId: UInt32) -> Data? {
    guard let decoded = try? DashboardPackage.decode(package) else { return nil }
```

`DashboardPackage.decode` is the **TEMPLATE_AGENDA** decoder; it throws
`unsupportedTemplate` on anything with `templateId != 1`. For a widget-grid
package it always returned `nil`, so `fulfillRebase` bailed out early:
`pendingRebase` was never cleared and `currentPackageId` never advanced. The
sender then retried the same, already-rejected package id until the 16-minute
session timeout — exactly the observed "package=55/58/59 each retried 8-12
times".

The chain: the X3's NVS holds a package id higher than the phone's counter (an
id-store regression, e.g. after an app reinstall wipes `UserDefaults`) →
`persistIfNewer` returns `Stale` → `notify(0x10)` → the phone tries to rebase →
the wrong decoder → nothing changes → infinite retry. `TEMPLATE_AGENDA` was
unaffected because there the decoder succeeds and the rebase self-heals
invisibly.

**Still unconfirmed on hardware.** The falsifiable prediction: a serial capture
during a widget-grid send should now print `BLE-RX persistIfNewer status=2
detail=0` (`PersistStatus::Stale`). Anything else means this story is wrong.

**Fixed** (uncommitted): rebasing now patches the package id straight into the
shared envelope instead of decoding and re-encoding — see below.

## What changed, and where

### iPhone app (`xteink-x3-dashboard-ios`, branch `feat/native-ios-app`)

New, all test-driven (**171 tests pass**, app builds for `generic/platform=iOS`):

- `Sources/DashboardCore/PackageEnvelope.swift` — template-agnostic operations on
  the 28-byte shared prefix + CRC trailer. `rebased(_:to:)` overwrites the
  package id at offset 8..11 and recomputes the CRC, validating magic, declared
  length, schema and CRC first (it recomputes the CRC, so a corrupt input would
  otherwise be silently "repaired"). `packageId(of:)` reads the id without
  decoding content. 9 tests.
- `Sources/DashboardCore/ReceiverStatus.swift` — wraps `TransferStatus` with the
  receiver's new detail byte and turns it into Dutch prose (`reason`). Accepts
  both 7-byte (old firmware) and 8-byte payloads. 9 tests.
- `Sources/DashboardCore/HomeAssistantEntity.swift` — decodes
  `GET /api/states/<id>`, renders an entity as a `KpiValue` inside the 16-byte
  wire budget. Rounds float noise to one decimal, maps
  `unavailable`/`unknown` to "—", derives a label from the entity id when
  `friendly_name` is missing, and truncates on **character** boundaries so
  `dashboard::validateUtf8` cannot reject the package. 12 tests.
- `Sources/DashboardCore/HomeAssistantClient.swift` — read-only REST client with
  an injected transport (so it is testable without a network). `states(for:)`
  is deliberately non-throwing and returns partial results: one renamed entity
  must not blank a dashboard that only refreshes every 15 minutes. 7 tests.

Changed:

- `BleDashboardSender.swift` — `fulfillRebase` uses `PackageEnvelope.rebased`;
  `rebuiltPackage` deleted. New `@Published lastRejectionReason` fed from
  `ReceiverStatus.reason`.
- `WidgetGridComposition.swift` — new `WidgetSlotKind.homeAssistant(entityId:)`
  case; `WidgetGridComposer.widgets(...)` takes `homeAssistantValues:` (defaulted,
  so existing call sites are untouched). Unresolved entities keep their cell and
  render "—" rather than reflowing the layout. 5 tests, including a Codable
  round-trip and backward compatibility for layouts saved before this existed.
- `ContentView.swift` — shows "Laatste weigering".
- `WidgetCompositionView.swift` — preview tile for the new slot kind.

### Firmware (`CrossInk`, branch `feat/ble-handoff`)

- `BleHandoffRecord.h/.cpp` — new `SHARED_PREFIX_SIZE` / `MIN_ENVELOPE_SIZE`
  (28+4). `peekPackageHeader` now uses the envelope minimum instead of
  `MIN_PACKAGE_SIZE`, which is TEMPLATE_AGENDA's 32-byte header and wrongly
  rejected a valid 34-byte zero-widget grid package that
  `decodeWidgetGridPackage` accepted. Regression test added in
  `DashboardWidgetGridTest.cpp`.
- `BleHandoffNvs.h/.cpp` — `persistIfNewer` gained an optional `Status* detailOut`
  carrying the validation status behind an `InvalidPackage` result.
- `BleReceiverMain.cpp` — `notify()` payload is now 8 bytes; the extra byte names
  the reason (`dashboard::Status` behind `0x12`, `PersistStatus` behind `0x13`,
  `TransferStatus` behind `0x11`). Older phone builds read the first 7 bytes and
  are unaffected.
- `platformio.ini` — `BoardConfig` in the receiver env's `lib_deps` (see above).
- `CHANGELOG.md` — Unreleased section.

## Next steps, in order

1. **Flash the receiver with the right environment** — this is the step that was
   silently wrong before:
   ```bash
   ~/.platformio/penv/bin/pio run -e spike-ble-receiver-x3 -t upload --upload-port /dev/cu.usbmodem31301
   ```
   The X3 must be awake. `env:default` flashes the *reader*, not the receiver.
2. Ask the user to sleep the X3 with a short power-button press (do not wait out
   the 10-minute idle timeout), then capture serial through one wake cycle while
   the phone sends a widget-grid package. Use the reconnect-tolerant capture loop
   from the previous handoff — `/dev/cu.usbmodem31301` disappears on deep sleep.
3. Read `BLE-RX persistIfNewer status=N detail=M`. Prediction: `status=2` (Stale).
   With the app rebuilt, the rebase should now succeed and the next attempt
   should be accepted. The phone will also show the reason on screen, so step 2
   may not even be necessary.
4. If it is *not* Stale, the elimination argument in this document is wrong —
   decode `N` against `PersistStatus` (`Ok=0, NotFound=1, Stale=2,
   InvalidPackage=3, OpenFailed=4, ReadFailed=5, WriteFailed=6, CommitFailed=7,
   VerifyFailed=8`) and `M` against `Status` (`Ok=0, InvalidArgument=1,
   InvalidSize=2, InvalidMagic=3, InvalidSchema=4, UnsupportedSchema=5,
   UnsupportedTemplate=6, InvalidLength=7, InvalidTimestamp=8, InvalidText=9,
   InvalidUtf8=10, InvalidCrc=11, StalePackage=12`).

## The design decision waiting for the user

`MAX_PACKAGE_SIZE` is 256 bytes, of which 34 go to the envelope. Measured
ceilings for a realistic dashboard:

| Layout | Bytes |
|---|---|
| 3 KPI tiles only | 87 |
| 3 KPI + agenda list, 4 rows | 185 |
| 3 KPI + agenda list, 6 realistic rows | 226 |
| 3 KPI + agenda list, 3 max-length rows | rejected (`InvalidLength`) |

`MAX_WIDGETS` is 8. For the stated goal — a dashboard of Home Assistant,
third-party and iPhone entities — this is the binding constraint, and raising it
is not free: `TransferAssembler`, `SlotRecord` (×2, static), `pendingFrame`, and
the `frame` local in `loop()` all scale with it, and that last one is a stack
allocation. Worth deciding before more widget types are added. Not attempted
this session: it cannot be verified without hardware.

## Notes for whoever picks this up

- Both repos still have the previous session's uncommitted work (Bug 1's
  `resendNow`, the 07:00–22:00 `AgendaWakePolicy` change, `main.cpp`'s
  `resolveAgendaWakeLocalTime`). It has been preserved, and the
  `AgendaWakePolicy` tests now pass in the real suite. **Nothing is committed** —
  the user has not seen any of this yet.
- The `AgendaWakePolicy` night-branch (sleeping straight through to 07:00) is
  still unverified on hardware; it needs either waiting past 22:00 or faking the
  RTC.
- `iPhone`: `xcrun devicectl` device id `837B3AD6-71E0-5E45-8CF6-F309BE23C1B5`,
  team `F5M939PSPR`. Build without a device with
  `-destination 'generic/platform=iOS' CODE_SIGNING_ALLOWED=NO`.
- Home Assistant support stops at `DashboardCore`. There is no settings UI for
  the base URL and token yet, no keychain storage, and no entity picker in the
  composer — those need design input and a device to test on.
