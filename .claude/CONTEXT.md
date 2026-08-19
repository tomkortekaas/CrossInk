# CrossPoint Reader — Durable Context

Keep this file focused on repo-specific gotchas that are worth reusing in future sessions.

## FreeInk SDK

Refer to https://freeink.org/llms.txt for guidance.

## Simulator

- Simulator patches belong in the adjacent `crossink-simulator` repo.
- The valid local simulator env in this repo is `simulator`, and `pio run -e simulator` currently builds cleanly.
- The simulator `PNGdec` stub in `crossink-simulator/src/PNGdec.h` needs to mirror the real API shape used by app code, including `hasAlpha()` and `getTransparentColor()`, even though decode still fails intentionally.
- Known simulator limits:
  - No image rendering: `platformio.ini` ignores `hal`, `PNGdec`, and `JPEGDEC`, so image decoders are intentionally absent.
  - JPEGDEC stub always fails; `JPEGDEC fallback: open failed (err=-1)` is expected in simulator.
  - `esp_deep_sleep_start()` is a no-op in simulator.
  - `HalStorage` uses POSIX file access under `./fs_` and allows multiple readers, unlike real hardware.

## Real Hardware / Storage

- SdFat on hardware allows only one open reader per file path at a time. If a fallback needs to reopen the same file, close the first handle before reopening.

## Rendering / Reader Pipeline

- `lib/Epub/Epub/Page.cpp`: images must render only in `GfxRenderer::BW`; grayscale passes are text anti-aliasing passes only.
- Kindle EPUBs may contain paired high-res and old-Kindle fallback images. `ChapterHtmlSlimParser` should skip `<img>` nodes with `data-AmznRemoved-M8` to avoid duplicate stacked images.
- After image/layout pipeline changes that affect cached EPUB output, clear the affected `.crosspoint/epub_<hash>/` cache if behavior looks stale.

## UI Consistency

- Use FreeInkUI SDK components and input routing for list-style screens where possible. Row rendering, touch targets,
  hit testing, and pagination should share the same FreeInkUI list configuration instead of custom touch scaling.

## Heap Baselines (X4 hardware, SD card font)

- A normal resume-into-partial reading session runs at ~85-90KB free / ~49KB maxAlloc by
  the first watermark crossing (Epub metadata + x-locations + resident glyph caches).
  Do not read mid-range heap numbers as session degradation without checking the scenario.
- SD-font section builds cost ~38-50KB at cold start; the 4-style advance-table prewarm
  (~30KB incl. 16KB contiguous scratch) dominates and is skipped below 80KB free.

## Misc Repo Gotchas

- POSIX TZ signs are inverted from ISO 8601 in `TimeStore::applyTimezone()`: `"UTC-1"` means UTC+1.
- `LyraTheme::drawHeader()` does not call `BaseTheme::drawHeader()`, so header changes in the base theme must be duplicated in Lyra if needed.

## Host Test Suite And BLE Receiver Builds

- cmake/ctest ARE available, bundled with PlatformIO — earlier sessions wrongly
  recorded them as missing and fell back to throwaway `clang++` harnesses:
  ```bash
  export PATH="$HOME/.platformio/packages/tool-cmake/bin:$PATH"
  cmake -S test -B /tmp/crossink-test-build -DCMAKE_BUILD_TYPE=Release
  cmake --build /tmp/crossink-test-build --target BleHandoffRecordTest -j8
  /tmp/crossink-test-build/ble_handoff_record/BleHandoffRecordTest
  ```
  `tool-ninja` is there too. Use the real suite, not ad-hoc harnesses.
- The X3 holds two app images: `app0`/`ota_0` at `0x10000` is the reader,
  `app1`/`ota_1` at `0x650000` is the BLE receiver. `DashboardBootSwitch` picks
  between them by rewriting `otadata`; each must be flashed separately.
- **Never flash the receiver with `pio run -e spike-ble-receiver-x3 -t upload`.**
  `-t upload` writes to `0x10000` regardless of that env's
  `board_upload.offset_address`, so it overwrites the *reader* with the receiver
  image. The device then boot-loops on `BLE-RX invalid launch route; returning
  to reader`, because the receiver's launch guard restarts into what is now
  itself. Recover with `pio run -e default -t upload` (~3 min). Flash the
  receiver with esptool at the explicit offset instead:
  ```bash
  ~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
    --chip esp32c3 --port /dev/cu.usbmodem31301 --baud 921600 \
    write_flash 0x650000 .pio/build/spike-ble-receiver-x3/firmware.bin
  ```
- `env:default` does NOT build the BLE receiver at all: `BleReceiverMain.cpp` is
  behind `CROSSINK_BLE_HANDOFF_RECEIVER`, defined only by
  `env:spike-ble-receiver-x3`. Flashing `-e default` leaves `app1` untouched.
- `env:spike-ble-receiver-x3` does not extend `[base]`, so it only links what its
  own `lib_deps` lists. Anything in `src/spikes/ble_handoff/` that includes
  `<Logging.h>` needs `BoardConfig` there, or the build fails with
  `fatal error: BoardConfig.h: No such file or directory`.

## Fonts: do not regenerate `src/fontIds.h`

`lib/EpdFont/scripts/build-font-ids.sh` carries a hardcoded font list, and the
checked-in `src/fontIds.h` is older than the script. Running it as the
`custom-fonts` skill instructs (step 3) **rewrites three existing ids** —
`UI_10`, `UI_12` and `SMALL` — which silently repoints every caller that looks a
font up by id. Add a new font's id by hand with the same SHA-256 algorithm
instead, next to the existing defines.

Measured 2026-08-19 while sizing the dashboard font ladder.

## Fonts: subsetting is what makes large sizes affordable

A Lexend size at the default intervals costs ~243 KB in flash for four styles,
and a single unsubsetted 34 px bold face pushes `-e default` **3,840 bytes past
the OTA partition**. The same face restricted to the characters a dashboard
value actually uses costs ~16 KB — a 4x saving on one face.

Restrict with `--font-include-intervals 0:32,126`. Note that
`--additional-intervals` does **not** work alongside it: `--font-include-intervals`
blocks face 0 outside its interval, so a code point added the other way
(e.g. `°` at 176) is exported but then fails to load and ends up an empty glyph.
Add a second include interval instead: `--font-include-intervals 0:176,176`.

`fontconvert.py` needs `freetype-py` and `fontTools`, which are not in the
system Python.
