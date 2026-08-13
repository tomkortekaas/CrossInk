# Calendar Dashboard Vertical Slice Design

**Date:** 2026-08-13  
**Status:** Approved in conversation; awaiting written-spec review  
**Milestone:** One real calendar card from a minimal iPhone app to the XTEINK X3 dashboard sleep screen

## Goal

Prove one product-shaped vertical slice in which an iPhone reads the next calendar event, sends a compact versioned dashboard package over an explicitly activated BLE receiver, and the BLE-free CrossInk reader renders the persisted card on its dashboard sleep screen without regressing EPUB stability.

The slice retains the architecture established by the isolated BLE handoff spike: the iPhone owns data access and presentation strings, while the X3 validates, persists, and renders bounded data. BLE and the reader must never be resident in the same firmware image.

## Scope

The slice includes:

- a minimal native iPhone app using Swift, EventKit, and CoreBluetooth;
- the next calendar event beginning within 24 hours, or an explicit no-events card;
- a versioned render-ready dashboard package;
- chunked BLE transfer with receiver status notifications;
- power-loss-safe last-known-good persistence;
- boot switching between a BLE-free reader in `app0` and an isolated receiver in `app1`;
- `Back + Power` activation of the receiver;
- one fixed portrait agenda-card layout in the existing dashboard sleep mode;
- host/simulator tests, firmware and iOS builds, and one hardware milestone run.

The slice excludes pairing, BLE encryption, background synchronization, multiple cards, user-selectable widgets, layout editing, calendar selection, external data sources, app distribution, and a production OTA/receiver packaging strategy.

## Product Constraints

- The iPhone is the brain. It selects calendar data and produces all visible localized text.
- The X3 is deliberately simple. It validates a bounded package, retains last-known-good, and renders fixed fields.
- The reader image must contain no BLE symbols, tasks, initialization, or retained BLE buffers.
- The receiver image must exclude reader, display, framebuffer, SD, settings, fonts, activities, and EPUB units.
- The receiver temporarily occupies the second existing OTA slot. Normal dual-slot OTA rollback is not available in this prototype configuration.
- The BLE service remains unpaired for this milestone. The receiver advertises only after an explicit physical gesture and exits after one accepted package.
- Existing dirty changes in the main worktree must not be overwritten. Work continues in the clean `feat/ble-handoff` worktree.

## Architecture

### Firmware images and activation

The normal BLE-free reader is installed in the `app0` OTA partition. The isolated BLE receiver is installed in `app1`. Both use the existing partition table and shared NVS partition.

During the reader's existing early power-button wake handling, holding `Back + Power` selects `app1` and restarts. The gesture is checked before the expensive reader, SD, settings, and EPUB initialization path. A normal power wake remains unchanged. The existing `Back`-held behavior that routes a normal boot to Home remains unchanged when Power is not part of the receiver gesture.

The receiver verifies that it is running from the expected receiver slot before advertising. After accepting and verifying one complete package, it selects `app0`, logs the result, flushes serial briefly, and restarts without attempting live BLE teardown. On a transfer, validation, persistence, or boot-switch failure, it logs and reports the error and remains in receiver mode so the user can retry. A receiver timeout is not part of v1; the user can power-cycle or retry from the app.

Boot switching reuses the repository's existing OTA-data switching mechanism rather than introducing a new partition format. The implementation must validate both target partitions and refuse to switch if the expected app image is absent.

### iPhone app

The iPhone app is a small native SwiftUI application in the existing empty `iphone-test-app` directory. It uses:

- EventKit for calendar authorization and event lookup;
- CoreBluetooth for discovery, connection, transfer, and status notifications;
- `UserDefaults` for a monotonically increasing local package identifier.

The single screen shows calendar permission state, the selected next event, a `Ververs agenda` action, a `Stuur naar X3` action, and connection/transfer status. The app searches all readable calendars for the earliest event whose start is between now and 24 hours from now. All-day events are eligible. If no event exists, the app produces a valid card whose title states that there are no appointments in the next 24 hours.

Visible strings are formatted on the iPhone using the user's current locale and time zone. The app sends a title, time line, normal footer, and stale footer. It does not send calendar identifiers, notes, attendees, locations, account data, or credentials.

The service UUID remains `8c9f9d10-7c6d-4c8e-a2cb-49586da45d10`. The write characteristic remains `8c9f9d11-7c6d-4c8e-a2cb-49586da45d10`. A new notify/read status characteristic uses the next UUID in the family, `8c9f9d12-7c6d-4c8e-a2cb-49586da45d10`.

### Dashboard package v1

The package is a fixed-header, variable-content binary structure with a maximum encoded size of 256 bytes. All multi-byte integers are unsigned little-endian. The byte-level layout is:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | ASCII magic `X3DP` |
| 4 | 1 | schema version, value `1` |
| 5 | 1 | template id, value `1` for agenda card |
| 6 | 2 | total package length including CRC |
| 8 | 4 | monotonically increasing package id |
| 12 | 8 | generated-at Unix seconds |
| 20 | 8 | valid-until Unix seconds |
| 28 | 1 | title byte length |
| 29 | 1 | time-line byte length |
| 30 | 1 | footer byte length |
| 31 | 1 | stale-line byte length |
| 32 | variable | UTF-8 title, time line, footer, stale line in that order |
| final 4 | 4 | CRC-32 over every preceding package byte |

The package must be at least 36 bytes and no larger than 256 bytes. Text fields may be empty only where the following rules permit it:

- title: required, 1–96 bytes;
- time line: required, 1–48 bytes;
- footer: optional, 0–40 bytes;
- stale line: required, 1–48 bytes.

The combined fields must fit the maximum package size. Each field must be valid UTF-8, contain no embedded NUL byte or ASCII control character other than no controls at all, and end on a code-point boundary. `generatedAt` must be nonzero. `validUntil` must be greater than or equal to `generatedAt`. The receiver does not interpret locale or calendar semantics.

A package id must be greater than the highest valid persisted package id. Equal or lower ids are rejected as stale. The iPhone increments its `UserDefaults` counter before constructing a send attempt; gaps caused by failed sends are valid.

### BLE transfer protocol v1

The negotiated ATT payload may be smaller than a package, so the app sends framed writes. Every frame begins with a one-byte frame type and four-byte little-endian package id.

- `START` (`0x01`): package id, two-byte total package length, and four-byte expected package CRC.
- `CHUNK` (`0x02`): package id, two-byte package offset, followed by one or more data bytes.
- `COMMIT` (`0x03`): package id only.

The app subscribes to the status characteristic before sending. It uses write-with-response and sizes each chunk to the peripheral's reported maximum write value length minus the seven-byte chunk header. Chunks are contiguous, ordered, non-overlapping, and cover the package exactly once.

The receiver owns one static 256-byte assembly buffer and fixed scalar transfer state. `START` resets any incomplete transfer after validating id and length. `CHUNK` is accepted only at the next expected offset. `COMMIT` is accepted only after the declared number of bytes has arrived. The receiver then verifies that the package id and CRC in the frame agree with the decoded package and performs full package validation before persistence.

Status notifications use a compact binary response containing status code, package id, and received-byte count. V1 codes are:

- `ready` (`0x01`)
- `progress` (`0x02`)
- `accepted` (`0x03`)
- `stale` (`0x10`)
- `invalid-frame` (`0x11`)
- `invalid-package` (`0x12`)
- `storage-failed` (`0x13`)
- `boot-switch-failed` (`0x14`)

The app reports success only after `accepted`. Disconnect is not proof of success. The receiver sends `accepted` only after write, commit, readback, and validation of the persisted slot have succeeded. It delays only long enough for the notification and serial log to leave before switching to the reader.

### Persistence and last-known-good

NVS contains two fixed-size package slots and a small selection record. Each package slot stores its own record magic, storage-format version, encoded package length, package bytes padded to the maximum, and record CRC. The storage wrapper and package CRC protect different boundaries and are both validated.

On update, the receiver:

1. reads and validates both slots;
2. identifies the valid slot with the highest package id;
3. rejects a package whose id is not greater;
4. writes the other slot;
5. commits NVS;
6. reads and fully validates the new slot;
7. updates and commits the small selection record;
8. reads the selected slot again before reporting `accepted`.

The old slot is never erased as part of accepting a new package. If power fails before selection is committed, the prior selected valid slot remains last-known-good. If the selection record is absent or invalid, reader and receiver recover by choosing the valid slot with the highest package id. If only one slot validates, it is used. If neither validates, the system behaves as if no dashboard package exists.

The implementation uses fixed arrays and stack objects below the repository's 256-byte stack guidance. The 256-byte assembly buffer and package slot buffers are static or single-instance storage, not repeatedly allocated. There are no growing containers or `String`/`std::string` instances in BLE callbacks, validation, persistence, or dashboard rendering.

### Reader and sleep-screen rendering

The reader performs no BLE work. It reads and validates the selected package only when rendering `SLEEP_SCREEN_MODE::DASHBOARD_SLEEP`. Other sleep modes are unchanged.

When there is a valid supported agenda package, it replaces the existing book-statistics dashboard sleep screen with a fixed portrait agenda layout:

- small heading `AGENDA`;
- a prominent appointment title, limited to two rendered lines;
- a prominent time line;
- a bottom footer.

If the X3 RTC time is later than `validUntil`, the same card remains visible but the stale line replaces the normal footer. If RTC time is unavailable, the package is rendered with its normal footer and a diagnostic is logged; absence of a trustworthy clock must not discard last-known-good. Text that does not fit is truncated without splitting a UTF-8 sequence. Rendering uses existing fonts and glyph fallback, runtime renderer dimensions, and the same bezel-safe layout mechanisms as existing sleep screens.

If no valid supported package exists, the existing dashboard sleep implementation is used unchanged. An unknown schema or template is unsupported, not corrupt: it is skipped and the existing dashboard is rendered. A corrupt slot never replaces a valid older slot.

Package data is copied into a bounded local/render object only for the render operation and is not retained by the activity afterward. No second framebuffer is introduced.

## Error and Recovery Behavior

- Calendar permission denied: the app explains that calendar access is required and does not fabricate event data.
- No upcoming event: the app sends a valid no-events card.
- X3 not found: the app remains retryable and preserves the constructed card.
- Disconnect or timeout before `accepted`: the app reports failure; the X3 retains the prior selected slot.
- Invalid frame, offset, length, id, CRC, UTF-8, timestamp, schema, or template: the receiver reports the specific protocol class and does not persist or reboot.
- Older or duplicate package id: the receiver reports `stale` and remains available for a newer attempt.
- NVS failure: the receiver reports `storage-failed`, keeps the old slot, and remains available.
- Reader-slot switch failure: the accepted package remains persisted, the receiver reports `boot-switch-failed`, and does not restart into an unknown target.
- Missing receiver image: the reader refuses the `Back + Power` switch and continues a normal boot with an error log.
- Missing, corrupt, or unsupported dashboard data in the reader: existing dashboard sleep rendering remains the fallback.

## Security and Privacy

This milestone deliberately uses an unpaired writable BLE service. Risk is limited operationally by requiring a physical `Back + Power` activation, advertising only in the isolated receiver image, accepting one package, and returning to the reader immediately after acceptance. This is not a production security boundary.

The app transmits only the four rendered card strings and package timestamps. It does not transmit raw EventKit records, locations, attendees, notes, calendar names, account identifiers, credentials, or historical events. Pairing, authenticated writes, replay protection across app reinstalls, and encrypted application payloads are deferred to the packaging/security milestone.

## Testing Strategy

### Host and simulator tests

Dependency-free C++ tests cover:

- exact v1 byte encoding and decoding;
- CRC verification and every length boundary;
- valid and invalid UTF-8, embedded NUL, and control characters;
- timestamp validation;
- supported and unknown schema/template behavior;
- frame parsing, contiguous chunk assembly, offsets, duplicates, overflow, reset, and commit-before-complete;
- monotonically increasing package ids;
- two-slot selection, corrupt-slot fallback, invalid selection-record recovery, and interrupted-update cases;
- UTF-8-safe render truncation.

Simulator coverage verifies valid-package agenda rendering, expired-package stale rendering, missing-package fallback, and unsupported-package fallback at X3 dimensions.

Swift unit tests cover event selection, all-day formatting, no-event packages, exact binary encoding, CRC vectors, package-id incrementing, and BLE frame construction independent of CoreBluetooth.

### Build and static gates

- Build the simulator environment and relevant host tests.
- Build the normal X3 reader image pinned for `app0`.
- Build the isolated receiver image pinned for `app1`.
- Inspect receiver build inputs or symbols to confirm reader/display/SD/settings/EPUB units are absent.
- Inspect reader symbols to confirm BLE APIs and BLE tasks are absent.
- Build and test the iPhone app for an available iOS Simulator destination; EventKit and BLE hardware behavior remain device tests.
- Run formatting and targeted static analysis on touched firmware files.

### Hardware milestone gate

One real XTEINK X3 cycle is sufficient at this milestone boundary:

1. Install and verify the reader in `app0` and receiver in `app1` without erasing NVS.
2. Confirm a normal wake boots the reader.
3. Hold `Back + Power` and confirm the receiver advertises.
4. On a physical iPhone, grant calendar access, select a real next event, and send it.
5. Record the receiver's validated/persisted package id and CRC and the app's `accepted` status.
6. Confirm automatic return to `app0` with no heap-poison assertion or boot loop.
7. Enter dashboard sleep mode and confirm the rendered title, time line, and footer match the transmitted bytes.
8. Send or inject one corrupt or lower-id attempt and confirm it cannot replace last-known-good.
9. Open the Test 5 reference EPUB and complete at least 20 rendered forward page turns without a white page, allocation failure, crash, or reboot.
10. Measure cold-boot and post-render free heap and largest allocatable block at the same points as Test 5.
11. Confirm the reader's largest allocatable block does not regress from the Test 5 reader baseline beyond ordinary run-to-run variation; any repeatable regression requires investigation and fails the gate.
12. Leave the device running the BLE-free reader image.

The result record must include commits, build sizes, package bytes or a privacy-safe redacted representation, CRC, app acknowledgement, receiver/reader logs, heap measurements, page-turn result, deviations, final decision, and final device state.

## Milestone Acceptance Criteria

The milestone passes only if:

- the native iPhone app reads a real upcoming calendar event and builds the documented package;
- the explicit `Back + Power` route enters the isolated receiver;
- the package is transferred, acknowledged, validated, persisted, and recovered by the reader;
- the event card renders on the dashboard sleep screen exactly as encoded;
- missing, corrupt, incomplete, duplicate, older, expired, and unsupported inputs follow the documented fallback behavior;
- interrupted writes cannot destroy the previous valid package;
- the reader image remains BLE-free and the receiver remains reader-free;
- all targeted automated tests and builds pass;
- the hardware transfer/render cycle passes;
- at least 20 EPUB page turns remain stable and the largest allocatable block shows no repeatable regression; and
- the device ends in the BLE-free reader image.

## Deferred Decisions

After this slice passes, a separate design will decide:

- production installation and signing of both firmware images;
- coexistence with normal OTA update and rollback behavior;
- App Store, TestFlight, or personal-signing distribution;
- authenticated BLE and replay protection;
- background refresh and automatic receiver scheduling;
- multiple data sources, card schemas, widgets, and layout selection.
