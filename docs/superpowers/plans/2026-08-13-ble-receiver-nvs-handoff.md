# BLE Receiver to BLE-Free Reader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove that an isolated X3 BLE receiver can persist a bounded payload in NVS and that a separately built BLE-free CrossInk reader can recover it while retaining normal heap and EPUB stability.

**Architecture:** A dependency-free fixed-record codec is shared by two ESP32 builds. A standalone receiver build compiles only the codec, NVS adapter, and BLE entry point; a debug-equivalent reader build retains CrossInk's BLE exclusion and adds one guarded early-boot NVS probe. The operator switches builds manually without erasing NVS.

**Tech Stack:** C++20, Arduino-ESP32/ESP-IDF NVS, ESP32 BLE, PlatformIO, CMake, GoogleTest, XTEINK X3 hardware

**Spec:** `docs/superpowers/specs/2026-08-13-ble-receiver-nvs-handoff-design.md`

## Global Constraints

- Receiver payload length is 1 through 64 bytes.
- The record contains magic, version, monotonically increasing `uint32_t` sequence, length, payload, and CRC32.
- Record bytes are serialized explicitly; persisted layout must not depend on compiler struct padding or endianness.
- Codec and callbacks use fixed storage and perform no heap allocation.
- The receiver must not compile or initialize CrossInk, display, framebuffer, SD, settings, fonts, activities, or EPUB units.
- The reader comparison build must retain `lib_ignore = BLE` and must never initialize BLE.
- NVS uses a dedicated namespace and key of at most 15 characters each.
- Neither build attempts live BLE teardown.
- Existing dirty spike changes are user work: preserve them and do not fold them into these tasks.
- Automatic dual-image boot switching, production dashboard semantics/rendering, and native iPhone background work remain out of scope.

---

### Task 1: Allocation-Free Payload Record Codec

**Files:**

- Create: `src/spikes/ble_handoff/BleHandoffRecord.h`
- Create: `src/spikes/ble_handoff/BleHandoffRecord.cpp`
- Create: `test/ble_handoff_record/CMakeLists.txt`
- Create: `test/ble_handoff_record/BleHandoffRecordTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**

- Produces: `ble_handoff::buildRecord(const uint8_t*, size_t, uint32_t, RecordBytes&) -> Status`
- Produces: `ble_handoff::validateRecord(const uint8_t*, size_t, DecodedRecord&) -> Status`
- Produces: `ble_handoff::nextSequence(bool, uint32_t, uint32_t&) -> Status`
- Produces: `ble_handoff::crc32(const uint8_t*, size_t) -> uint32_t`
- Produces: `RecordBytes`, a fixed `std::array<uint8_t, RECORD_SIZE>`; `DecodedRecord`, with fixed `std::array<uint8_t, 64>` payload storage
- Consumes: only C++ standard fixed-width integers, `std::array`, and `size_t`

- [ ] **Step 1: Add the codec contract and failing tests**

Create the header with constants and declarations, but no function definitions:

```cpp
namespace ble_handoff {
constexpr size_t MAX_PAYLOAD_SIZE = 64;
constexpr size_t RECORD_SIZE = 4 + 2 + 4 + 1 + MAX_PAYLOAD_SIZE + 4;

enum class Status : uint8_t {
  Ok,
  InvalidArgument,
  InvalidSize,
  InvalidMagic,
  InvalidVersion,
  InvalidLength,
  InvalidCrc,
  SequenceOverflow,
  NotFound,
  OpenFailed,
  ReadFailed,
  WriteFailed,
  CommitFailed,
  VerifyFailed,
};

using RecordBytes = std::array<uint8_t, RECORD_SIZE>;
struct DecodedRecord {
  uint32_t sequence = 0;
  uint8_t length = 0;
  std::array<uint8_t, MAX_PAYLOAD_SIZE> payload{};
};

uint32_t crc32(const uint8_t* data, size_t length);
Status buildRecord(const uint8_t* payload, size_t length, uint32_t sequence, RecordBytes& out);
Status validateRecord(const uint8_t* bytes, size_t size, DecodedRecord& out);
Status nextSequence(bool haveCurrent, uint32_t current, uint32_t& out);
}  // namespace ble_handoff
```

Add GoogleTests that independently assert:

```cpp
EXPECT_EQ(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9), 0xCBF43926U);

const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o', ' ', 'x', '3'};
RecordBytes bytes{};
ASSERT_EQ(buildRecord(hello, sizeof(hello), 7, bytes), Status::Ok);
DecodedRecord decoded{};
ASSERT_EQ(validateRecord(bytes.data(), bytes.size(), decoded), Status::Ok);
EXPECT_EQ(decoded.sequence, 7U);
EXPECT_EQ(decoded.length, sizeof(hello));
EXPECT_TRUE(std::equal(std::begin(hello), std::end(hello), decoded.payload.begin()));
```

Add separate tests for a changed covered byte, sizes shorter and longer than `RECORD_SIZE`, zero and 65-byte payloads, wrong magic, wrong version, sequence one when no prior record exists, exact increment, and `UINT32_MAX` overflow.

Register `add_subdirectory(ble_handoff_record)` in `test/CMakeLists.txt`. The target must compile the real `BleHandoffRecord.cpp` and link `crosspoint_test_common` plus `GTest::gtest_main`.

- [ ] **Step 2: Run the new target and verify RED**

Run:

```bash
cmake -S test -B build/tests
cmake --build build/tests --target BleHandoffRecordTest -j4
```

Expected: link failure for undefined `ble_handoff::crc32`, `buildRecord`, `validateRecord`, and `nextSequence`. Header or test compilation errors are not the expected RED; correct those first.

- [ ] **Step 3: Implement the minimal explicit-byte codec**

Use fixed offsets, little-endian `writeU16`/`writeU32` and `readU16`/`readU32` helpers, and the standard reflected CRC32 polynomial `0xEDB88320`. Zero the complete output record before writing fields. Cover version, sequence, length, and only the used payload bytes in CRC calculation; exclude magic and the stored CRC field exactly as specified by the design. Reject null pointers when their length is nonzero and validate size before reading offsets.

Do not use `reinterpret_cast` to wider integer pointers, `std::string`, `std::vector`, `new`, or `malloc`.

- [ ] **Step 4: Run the focused test and full host suite**

Run:

```bash
cmake --build build/tests --target BleHandoffRecordTest -j4
ctest --test-dir build/tests -R BleHandoffRecordTest --output-on-failure
cmake --build build/tests -j4
ctest --test-dir build/tests --output-on-failure
```

Expected: all commands exit 0 and every test passes.

- [ ] **Step 5: Format and commit Task 1**

```bash
clang-format -i src/spikes/ble_handoff/BleHandoffRecord.h src/spikes/ble_handoff/BleHandoffRecord.cpp test/ble_handoff_record/BleHandoffRecordTest.cpp
git add src/spikes/ble_handoff/BleHandoffRecord.h src/spikes/ble_handoff/BleHandoffRecord.cpp test/ble_handoff_record/CMakeLists.txt test/ble_handoff_record/BleHandoffRecordTest.cpp test/CMakeLists.txt
git commit -m "test: define BLE handoff record format"
```

### Task 2: ESP32 NVS Persistence Adapter

**Files:**

- Create: `src/spikes/ble_handoff/BleHandoffNvs.h`
- Create: `src/spikes/ble_handoff/BleHandoffNvs.cpp`

**Interfaces:**

- Consumes: `ble_handoff::RecordBytes`, `DecodedRecord`, `Status`, `buildRecord`, `validateRecord`, and `nextSequence`
- Produces: `ble_handoff::readPersisted(DecodedRecord&, RecordBytes* = nullptr) -> Status`
- Produces: `ble_handoff::persistAndVerify(const uint8_t*, size_t, DecodedRecord&) -> Status`
- NVS identifiers: namespace `blehandoff`, key `payload`

- [ ] **Step 1: Add the adapter contract without its implementation**

Declare the two functions above in the header. Do not create the `.cpp` yet: the receiver call added in Task 3 supplies the observable link failure before the adapter implementation exists.

The adapter is an ESP boundary and does not receive a fake host-NVS test. Its observable integration behavior is covered by read-back validation on hardware.

- [ ] **Step 2: Implement read, write, commit, and read-back verification after Task 3's RED build**

`readPersisted` must:

1. open `blehandoff` read-only;
2. query blob size and require exactly `RECORD_SIZE`;
3. read into fixed `RecordBytes`;
4. close the handle on every exit path; and
5. return `validateRecord`'s precise status.

`persistAndVerify` must:

1. call `readPersisted` to determine the prior valid sequence;
2. call `nextSequence`, treating `NotFound` or invalid prior data as no current record;
3. call `buildRecord` into a fixed buffer;
4. open `blehandoff` read-write;
5. call `nvs_set_blob`, then `nvs_commit`, closing the handle on every path;
6. call `readPersisted` and compare sequence, length, and used payload bytes; and
7. return `VerifyFailed` on any mismatch.

No ESP error path logs inside the codec. The NVS adapter returns typed status; receiver and reader callers provide phase-specific logs.

- [ ] **Step 3: Verify the adapter compiles through the new receiver target introduced in Task 3**

Task 2's final compile check is intentionally coupled to Task 3 because no existing production environment should link this experimental NVS adapter. Proceed directly to Task 3 before committing.

### Task 3: Standalone BLE Receiver Build

**Files:**

- Create: `src/spikes/ble_handoff/BleReceiverMain.cpp`
- Modify: `platformio.ini`

**Interfaces:**

- Consumes: `ble_handoff::persistAndVerify`, `DecodedRecord`, and `Status`
- BLE service: `8c9f9d10-7c6d-4c8e-a2cb-49586da45d10`
- Writable characteristic: `8c9f9d11-7c6d-4c8e-a2cb-49586da45d10`
- Produces: Arduino `setup()` and `loop()` for `env:spike-ble-receiver-x3`

- [ ] **Step 1: Add the isolated PlatformIO environment with an empty receiver entry point**

Add an environment extending `base`, using the X3/X4 board flags from `env:debug`, overriding `lib_ignore` to keep only `HalClockSim` ignored, and using:

```ini
build_src_filter =
  -<*>
  +<spikes/ble_handoff/BleHandoffRecord.cpp>
  +<spikes/ble_handoff/BleHandoffNvs.cpp>
  +<spikes/ble_handoff/BleReceiverMain.cpp>
```

Define `CROSSINK_BUILD_ENV=\"spike-ble-receiver-x3\"`, `ENABLE_SERIAL_LOG`, and `LOG_LEVEL=2`. Add a minimal `setup()`/`loop()` that calls the declared `persistAndVerify`; Task 2 intentionally has not defined it yet.

- [ ] **Step 2: Build and verify the isolated target is RED for the intended missing behavior**

Run:

```bash
pio run -e spike-ble-receiver-x3
```

Expected: link failure for undefined `ble_handoff::persistAndVerify`. If CrossInk reader, SD, display, settings, or EPUB compilation appears, fix `build_src_filter` before continuing.

- [ ] **Step 3: Complete the NVS adapter and minimal receiver**

Create `BleHandoffNvs.cpp` with Task 2's implementation. In `BleReceiverMain.cpp`:

- initialize serial logging and NVS before BLE;
- log `ESP.getFreeHeap()` and `ESP.getMaxAllocHeap()` before BLE and after advertising;
- initialize the existing device name, service UUID, and write UUID;
- accept unpaired writes only;
- in the BLE callback, reject lengths outside 1 through 64, copy into a static fixed 64-byte pending buffer, store the length, and set a `volatile` pending flag;
- guard the pending buffer handoff with a critical section or equivalent safe callback-to-loop synchronization available in the live Arduino-ESP32 SDK;
- in `loop()`, copy the pending data to a local fixed buffer, call `persistAndVerify`, log `PERSISTED sequence=<n> length=<n> crc=<n> free=<n> maxAlloc=<n>` on success, delay only long enough to flush serial, and call `ESP.restart()`;
- on any error, log the typed status, clear only the processed pending flag, and keep advertising; and
- never call BLE deinit or teardown.

Do not construct Arduino `String` or `std::string` from the payload. Log bounded content with an explicit length.

- [ ] **Step 4: Build, inspect isolation, and check size**

Run:

```bash
pio run -e spike-ble-receiver-x3 -v 2>&1 | tee /tmp/crossink-spike-ble-receiver-build.log
rg "src/(main|activities|network)|lib/(Epub|hal)|HalDisplay|HalStorage" /tmp/crossink-spike-ble-receiver-build.log
pio run -e spike-ble-receiver-x3 -t size
```

Expected: build and size commands exit 0. The `rg` command produces no compile-unit matches for CrossInk reader, activities, network, EPUB, display, or storage; SDK/toolchain path mentions do not count as application units.

- [ ] **Step 5: Format and commit Tasks 2 and 3**

```bash
clang-format -i src/spikes/ble_handoff/BleHandoffNvs.h src/spikes/ble_handoff/BleHandoffNvs.cpp src/spikes/ble_handoff/BleReceiverMain.cpp
git add src/spikes/ble_handoff/BleHandoffNvs.h src/spikes/ble_handoff/BleHandoffNvs.cpp src/spikes/ble_handoff/BleReceiverMain.cpp platformio.ini
git commit -m "feat: add isolated X3 BLE receiver spike"
```

### Task 4: Genuinely BLE-Free Reader Probe

**Files:**

- Create: `src/spikes/ble_handoff/BleHandoffReaderProbe.h`
- Create: `src/spikes/ble_handoff/BleHandoffReaderProbe.cpp`
- Modify: `src/main.cpp`
- Modify: `platformio.ini`

**Interfaces:**

- Consumes: `ble_handoff::readPersisted`, `DecodedRecord`, and `Status`
- Produces: `BleHandoffReaderProbe::logPersistedPayload() -> void`
- Build guard: `CROSSINK_BLE_HANDOFF_READER`
- Build environment: `env:spike-ble-reader-x3`, extending `env:debug` and retaining `base`'s `lib_ignore = BLE`

- [ ] **Step 1: Add the guarded call before its implementation**

Under `#ifdef CROSSINK_BLE_HANDOFF_READER`, include the probe header and call `BleHandoffReaderProbe::logPersistedPayload()` after `HalSystem::begin()` and reset diagnostics but before `gpio.begin()`, SD mounting, settings loading, or EPUB initialization.

Add `env:spike-ble-reader-x3` extending `env:debug` with only the extra `-DCROSSINK_BLE_HANDOFF_READER` build flag. Do not override `lib_ignore`.

- [ ] **Step 2: Build and verify RED**

Run:

```bash
pio run -e spike-ble-reader-x3
```

Expected: compile or link failure because `BleHandoffReaderProbe::logPersistedPayload()` is not yet defined.

- [ ] **Step 3: Implement the read-only probe**

The probe uses fixed `DecodedRecord` storage, calls `readPersisted`, and:

- logs `PAYLOAD VALID sequence=<n> length=<n> payload=<bounded bytes>` for `Status::Ok`;
- logs a concise non-fatal status for missing or invalid data; and
- never writes, erases, consumes, or retains the record.

Keep all probe declarations and definitions behind `CROSSINK_BLE_HANDOFF_READER`. The plain `debug` build must not execute the probe.

- [ ] **Step 4: Build the probe, plain baseline, and host tests**

Run:

```bash
pio run -e spike-ble-reader-x3
pio run -e debug
ctest --test-dir build/tests --output-on-failure
```

Expected: all commands exit 0. Inspect both reader build summaries and confirm neither resolves or links the BLE library. The `spike-ble-reader-x3` build is the debug-equivalent measurement image; plain `debug` is its baseline.

- [ ] **Step 5: Format and commit Task 4**

```bash
clang-format -i src/spikes/ble_handoff/BleHandoffReaderProbe.h src/spikes/ble_handoff/BleHandoffReaderProbe.cpp src/main.cpp
git add src/spikes/ble_handoff/BleHandoffReaderProbe.h src/spikes/ble_handoff/BleHandoffReaderProbe.cpp src/main.cpp platformio.ini
git commit -m "feat: read BLE handoff payload in BLE-free reader"
```

### Task 5: Hardware Gate and Measurement Record

**Files:**

- Create: `measurements/test-5.md`

**Interfaces:**

- Consumes: receiver logs `PERSISTED`, reader logs `PAYLOAD VALID`, plain-debug and probe-reader heap measurements
- Produces: a reproducible three-cycle pass/fail record with exact firmware hashes and commands

- [ ] **Step 1: Create the measurement template before touching hardware**

Create `measurements/test-5.md` with sections for date, commit, device, payload, commands, receiver heap, plain-debug baseline heap, probe-reader heap, sequence/CRC results, EPUB identity, 20 page-turn result, three-cycle table, deviations, final decision, and final device state. Initialize status as `NOT RUN`; do not predeclare success.

- [ ] **Step 2: Record a plain debug baseline**

Build and upload without erase:

```bash
pio run -e debug -t upload
pio device monitor -b 115200
```

Open the chosen heavy EPUB and record free heap and largest allocatable block at the same cold-boot/first-render point used for the probe build. Record the EPUB path or stable identifier and current page.

- [ ] **Step 3: Run receiver cycle 1**

Upload the app only; do not run `erase`, `erase_flash`, or any full-chip recovery target:

```bash
pio run -e spike-ble-receiver-x3 -t upload
pio device monitor -b 115200
```

Write `hello x3` to characteristic `8c9f9d11-7c6d-4c8e-a2cb-49586da45d10` using the already proven iPhone or CoreBluetooth test client. Record advertising heap and the complete `PERSISTED` line. Confirm the receiver restarts without a heap-poison assertion.

- [ ] **Step 4: Flash the BLE-free reader and run the EPUB gate**

```bash
pio run -e spike-ble-reader-x3 -t upload
pio device monitor -b 115200
```

Record the `PAYLOAD VALID` line and compare cold-boot/first-render free heap and maxAlloc with plain `debug`. Restore the same heavy EPUB and perform at least 20 forward page turns. Record any white page, failed allocation, crash, or reboot as failure; do not average it away.

- [ ] **Step 5: Repeat two more complete cycles**

Repeat Steps 3 and 4 twice without erasing NVS. Require sequences 1, 2, and 3, or three consecutive values if a valid earlier record already existed. Every cycle must retain the exact payload and valid CRC after the manual flash.

- [ ] **Step 6: Decide the gate and restore a safe device state**

Mark PASS only if all three cycles succeed, sequences increase strictly, reader heap matches ordinary plain-debug variation, and all 20 page turns complete without memory or rendering failure. Otherwise mark FAIL or INCONCLUSIVE with the exact failed gate.

Finally restore normal firmware:

```bash
pio run -e debug -t upload
```

Record whether the final upload completed and whether esptool verified the flashed-data hash.

- [ ] **Step 7: Run final verification and commit the measurement**

```bash
cmake --build build/tests -j4
ctest --test-dir build/tests --output-on-failure
pio run -e spike-ble-receiver-x3
pio run -e spike-ble-reader-x3
pio run -e debug
git diff --check
git status --short
```

Expected: all automated commands exit 0. Hardware evidence in `measurements/test-5.md` determines PASS, FAIL, or INCONCLUSIVE.

```bash
git add measurements/test-5.md
git commit -m "docs: record isolated BLE handoff experiment"
```
