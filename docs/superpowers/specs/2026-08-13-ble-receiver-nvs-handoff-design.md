# Minimal BLE Receiver to BLE-Free Reader Design

**Date:** 2026-08-13

## Purpose

Validate that an XTEINK X3 can receive a bounded dashboard payload over unpaired BLE, preserve it across a firmware replacement, and then run the normal BLE-free CrossInk reader with its normal heap profile and stable heavy-EPUB rendering.

This is a manual two-flash feasibility experiment. It answers whether strict binary isolation removes the BLE/EPUB heap conflict observed in Tests 3 and 4. It is not the final delivery architecture.

## Scope

The experiment adds:

- a standalone X3 BLE receiver build that excludes the CrossInk application, display, SD, settings, and EPUB initialization;
- a fixed, versioned NVS record shared by the receiver and reader builds;
- an early, read-only reader hook that validates and logs the retained record; and
- host, build, and hardware gates for persistence, heap isolation, and EPUB stability.

The experiment retains the existing BLE service UUID `8c9f9d10-7c6d-4c8e-a2cb-49586da45d10` and writable characteristic UUID `8c9f9d11-7c6d-4c8e-a2cb-49586da45d10`.

Automatic dual-image boot switching, partition redesign, production payload semantics, dashboard rendering from the payload, and native iPhone background behavior are explicitly deferred until this gate passes.

## Architecture

### Standalone receiver binary

A dedicated PlatformIO environment uses `build_src_filter` to compile a minimal receiver entry point and the shared payload-record code only. It must not link or initialize the normal CrossInk reader path. The receiver initializes serial logging, NVS, and BLE, then advertises the existing service and accepts unpaired writes of 1 through 64 bytes.

The receiver logs free heap and largest allocatable block before BLE initialization and after advertising begins. It does not initialize the display, framebuffer, SD card, settings, fonts, activities, or EPUB reader.

### Shared payload record

The receiver and reader share a fixed-size, allocation-free record definition with these fields:

- 32-bit magic value;
- 16-bit format version;
- 32-bit monotonically increasing sequence number;
- 8-bit payload length;
- 64-byte payload storage; and
- 32-bit CRC32 covering the version, sequence, length, and used payload bytes.

Validation rejects an incorrect magic or version, a zero or greater-than-64 length, or a CRC mismatch. Encoding, CRC calculation, and validation use fixed-size stack or static storage and do not allocate heap memory.

The record is stored as one blob in a dedicated NVS namespace and key that do not overlap existing CrossInk settings.

### BLE-free reader hook

The normal `debug` environment remains unchanged in its exclusion of the BLE library. A small early-boot hook opens the shared NVS namespace read-only, validates any stored record, and logs `PAYLOAD VALID` with its sequence, length, and bounded payload text. It does not erase or mutate the record, so repeated reader boots remain diagnosable.

## Data Flow

1. Flash the standalone receiver without erasing the NVS partition.
2. Boot it and record the pre-BLE and advertising heap measurements.
3. Write a 1-to-64-byte payload, initially `hello x3`, to the writable characteristic.
4. Load the last valid record, use sequence one when none exists, or increment the valid sequence otherwise.
5. Build the new fixed record and CRC32 in bounded storage.
6. Persist the blob to NVS and commit it.
7. Read the blob back and validate every field.
8. On success, log `PERSISTED` with sequence, length, CRC, free heap, and largest allocatable block, then call `ESP.restart()` without live BLE teardown.
9. Flash the normal `debug` firmware without an erase operation.
10. Boot CrossInk, validate the retained record, and log `PAYLOAD VALID` before the heavy reader path.
11. Open the selected heavy EPUB, render it, and perform the page-turn stability gate.

The normal PlatformIO app upload starts at the application offset and must not erase the separate NVS partition. The hardware procedure must use an upload command that preserves NVS; any full-chip erase invalidates that cycle.

## Failure Handling

- A zero-length or greater-than-64-byte BLE write is rejected without changing NVS.
- If the existing record is missing or invalid, the next accepted record starts at sequence one.
- Failure to open NVS, write the blob, commit, read it back, or validate the read-back is logged. The receiver remains running and does not restart.
- The previous valid record must remain usable when a new write fails before commit. A read-back failure after commit is reported as a failed cycle and must not be presented as persisted success.
- The reader treats missing, truncated, version-mismatched, or CRC-invalid data as non-fatal. It logs the reason and continues its normal boot.
- Sequence wrap from `UINT32_MAX` to zero is rejected rather than silently producing a non-monotonic record.
- No path attempts live BLE deinitialization.

## Memory and Scope Constraints

The standalone receiver exists to isolate BLE memory from EPUB memory, not to optimize simultaneous operation. It uses a fixed 64-byte payload and fixed record, introduces no growing strings or containers in callbacks, and performs no full-screen allocation. BLE callback work copies only the bounded payload and signals processing outside the callback where practical.

The reader hook performs a one-shot fixed-size NVS read during cold boot and retains no payload buffer afterward. The regular reader binary must continue to exclude BLE so its heap comparison is meaningful.

This experiment supports the focused-reading mission by testing a low-residency transfer boundary without adding an always-on connectivity feature to the reader runtime.

## Verification Gate

### Host verification

Host tests must demonstrate:

- a record for `hello x3` validates;
- any changed covered byte fails CRC validation;
- lengths zero and greater than 64 are rejected;
- wrong magic and version are rejected;
- a valid prior sequence increments by exactly one; and
- sequence overflow is rejected.

### Build verification

- Build the standalone receiver environment successfully.
- Build the normal `debug` environment successfully.
- Inspect the receiver build inputs or map output to confirm reader, display, SD, settings, and EPUB application units are excluded.
- Confirm the normal `debug` build still excludes BLE.

### Hardware verification

The gate passes only when all of the following are recorded:

- the receiver accepts an unpaired write and logs a successful validated NVS read-back;
- the normal `debug` image reads the same payload and sequence after a non-erasing manual flash;
- the cold-boot reader free heap and largest allocatable block match the normal `debug` baseline within ordinary run-to-run variation, with no BLE tasks or initialization present;
- the chosen heavy EPUB renders normally and completes at least 20 page turns without white pages, allocation failures, crash, or reboot; and
- at least three complete receive, manual-flash, and read cycles succeed with strictly increasing sequences and valid CRCs.

Any full-chip erase, BLE-linked reader binary, invalid CRC, non-increasing sequence, unexplained heap regression, allocation failure, white page, crash, or reboot during the EPUB gate makes the result inconclusive or failed rather than passed.

## Decision After the Gate

If the gate passes, the next design may evaluate automatic receiver-to-reader boot switching and only then a native Swift/CoreBluetooth background test. If it fails, retain the independent successes of the sleep-screen and timer-wake experiments, capture the failing measurement, and do not proceed to iPhone background work.
