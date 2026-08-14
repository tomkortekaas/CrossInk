# Widget Dashboard — Handoff

**Date:** 2026-08-14
**For:** whoever (human or agent) picks this up next, in a fresh chat with no memory of this session.
**Repos:**
- Firmware: `/Volumes/2TB/Development/Projects/CrossInk`, branch `feat/ble-handoff`, HEAD `107fdf49`
- iPhone app: `/Volumes/2TB/Development/Projects/xteink-x3-dashboard-ios`, branch `feat/native-ios-app`, HEAD `c67f64c`

Both repos are clean and fully pushed as of this handoff. Nothing uncommitted, nothing unpushed.

## Where this came from

Original scope was the calendar-dashboard vertical slice (`docs/superpowers/specs/2026-08-13-calendar-dashboard-vertical-slice-design.md` and its plan sibling): one iPhone app sends the next calendar event over BLE to an isolated receiver, which persists it and the reader shows it on the sleep screen. That slice **physically works** — confirmed multiple times on real hardware, including a fully unattended background cycle (phone backgrounded, screen off, X3 woke on its own timer and got updated automatically). See CHANGELOG.md and the commit history on `feat/ble-handoff` before `5ea965e7` for that arc.

Two things then happened in this session that pushed the design further:

1. **The user wants more than one card.** Not "pick template A or B" but a genuine widget system — like iOS home-screen widgets: KPI tiles, an agenda list, arranged by the phone into a layout, with the arrangement itself changeable without a firmware update. This produced the `TEMPLATE_WIDGET_GRID` design below. An earlier, narrower attempt (`TEMPLATE_DAY_LIST`, a single hardcoded day-list shape) was built, then deliberately superseded once the fuller vision came out — see commit `5ea965e7` for exactly what replaced what and why.
2. **Flash was nearly full** (95.4% on env:default) before any of this. Profiling with `nm --size-sort -S -C` found 342KB spent on hyphenation dictionaries for 10 languages, 201KB of that on German alone. Trimmed to English-only (commit `03ad9638`), freeing 316KB — now 90.7% used, 611KB free. This is why there's room to keep building.

## Current state, precisely

### Shipped to the user's real firmware (not spike-only)
- `env:default` now includes `CROSSINK_BLE_HANDOFF_READER` (commit `13997c01`) — was previously only in the `spike-ble-reader-x3` test env. The agenda dashboard feature is now part of the daily-driver firmware, gated at runtime behind `SETTINGS.sleepScreen == AGENDA_SLEEP`.
- Only English hyphenation ships (commit `03ad9638`). The other 9 languages' generated `.trie.h` files are still on disk under `lib/Epub/Epub/hyphenation/generated/` (regeneratable via `scripts/update_hyphenation.sh`), just not `#include`d from `LanguageRegistry.cpp`.
- The user's physical X3 has this firmware flashed to app0 as of this session (flashed via `pio run -e default -t upload --upload-port /dev/cu.usbmodem31301`).
- The user's phone has the iOS app installed and running via Xcode/devicectl, with the "resilient foreground send session" + background-watch behavior from `xteink-x3-dashboard-ios` HEAD.

### Widget-grid architecture (built, tested where possible, NOT yet used by anything real)
New package template `TEMPLATE_WIDGET_GRID = 3`, alongside the still-active `TEMPLATE_AGENDA = 1` (the iPhone app has not been changed to send anything else — **nothing changes for daily use yet**). Files, all under `src/spikes/ble_handoff/`:

- `DashboardWidgetGrid.h/.cpp` (commit `5ea965e7`) — encode/decode for a package containing up to `MAX_WIDGETS` (8) widgets. Each widget has `columnSpan` (1-4, `GRID_COLUMNS=4`) and `rowSpan` (1-6, `MAX_ROW_SPAN=6`), and is either `WidgetType::Kpi` (label+value, ≤16 bytes each) or `WidgetType::List` (heading + up to 6 time+label rows — this is what replaces both the old single-event agenda card and the abandoned day-list idea). Shares the same 28-byte common prefix (magic/schema/template/length/packageId/generatedAt/validUntil) and CRC trailer as `TEMPLATE_AGENDA`, still capped at 256 bytes total (`MAX_PACKAGE_SIZE`, unchanged, reused from `BleHandoffRecord.h`).
- `DashboardGridLayout.h/.cpp` (commit `11e6e004`) — pure geometry: `computeGridLayout()` flows widgets left-to-right/top-to-bottom in shelves, wrapping when a widget's columnSpan doesn't fit the remaining row, with each shelf's height set by the tallest `rowSpan` placed in it. No rendering dependency; this is the one piece with real host-test coverage.
- `DashboardGridRenderer.h/.cpp` (commit `11e6e004`) — the actual `GfxRenderer` draw calls: bordered tile for Kpi, heading+rows for List, using `computeGridLayout()` for placement and `GfxRenderer::truncatedText()`/`getTextWidth()` for per-widget (not whole-screen) UTF-8-safe bounding. Only compiles under `CROSSINK_BLE_HANDOFF_READER`.
- `BleHandoffRecord.h/.cpp` (commit `107fdf49`) — added `PackageHeader` + `peekPackageHeader()`: reads the common prefix (packageId, timestamps, templateId, crc) from **any** template's bytes without knowing its content shape, and — important — does **not** reject an unrecognized `templateId`. That's what makes persistence generic.
- `BleHandoffNvs.h/.cpp` (commit `107fdf49`) — `PersistedPackage` now carries a `PackageHeader header` instead of an agenda-only `Package package`. `readSlot`/`load`/`persistIfNewer` validate and compare package ids via `peekPackageHeader` instead of the agenda-only `decodePackage`. **This file could not be host-tested** — it includes `<nvs.h>`, ESP-IDF only, doesn't compile on macOS. Verified by building `env:default` and `env:spike-ble-receiver-x3` clean, nothing more. If anything about persistence looks wrong, this is the first place to double check, ideally on real hardware with the existing last-known-good package still in NVS (don't erase NVS when testing).
- `BleHandoffReaderProbe.h/.cpp` (commit `107fdf49`) — `renderAgendaCard` renamed to `renderDashboardCard`; it now reads the persisted header and dispatches: `TEMPLATE_AGENDA` renders exactly as before (unchanged fonts/positions), `TEMPLATE_WIDGET_GRID` decodes and calls `renderWidgetGrid()`, anything else returns `false` so `SleepActivity` falls back to the pre-existing dashboard sleep screen. One call site updated in `src/activities/boot_sleep/SleepActivity.cpp:812`.

None of this has been visually verified — no simulator or hardware run has actually shown a widget-grid package rendered on a screen. Only the pure layout math (`DashboardGridLayout`) has real test coverage.

## What's actually left (the real next step)

**The iPhone app still only builds and sends `TEMPLATE_AGENDA` packages.** `xteink-x3-dashboard-ios` was not touched after commit `c67f64c` (the background-watcher milestone). Nothing in `DashboardCore` (the Swift package) knows about the widget-grid format at all yet.

To make any of the firmware work in part 2 actually visible, in order:

1. **Swift port of the widget-grid codec** — mirror `DashboardWidgetGrid.h/.cpp`'s byte layout in `Sources/DashboardCore/` (a new file, e.g. `WidgetGridPackage.swift`), with unit tests exercising the exact same byte layout as the C++ side (see how `DashboardPackageTests.swift` verifies exact bytes against the C++ agenda codec — same discipline needed here, cross-checking against `DashboardWidgetGrid.cpp`'s encoder).
2. **A composition UI** — this is probably the biggest remaining chunk. The user's stated goal (see conversation, not reproduced in full here) is an iOS-widget-like experience: pick which widgets appear (KPI tiles, agenda list) and where. Needs product decisions this session did not make:
   - What real data sources back a KPI tile? (steps, weather, battery, something else — nothing was decided beyond the illustrative "Stappen / 8.421" example used in tests and the mockup shown to the user.)
   - Does the user pick a layout once (persisted in the app) and it just refreshes, or re-pick per send? Given the background-watcher architecture (`BleDashboardSender.startAutoWatch`), the natural fit is: compose once, then `packageProvider` closure rebuilds fresh *content* for the same *arrangement* every cycle.
3. **Physical verification** — build both apps, flash, and watch the same kind of serial-log + visual check this session did for the agenda card, this time for a widget-grid package. Confirm no BLE symbols in the reader, confirm flash headroom is still healthy (was 611KB free after this session; recheck after the Swift/UI work in case anything unexpectedly grows the firmware side — it shouldn't, this milestone touched only already-shipped-size renderer/persistence code, but verify).
4. Only after that: consider whether `TEMPLATE_AGENDA` should be retired from the iOS app (the user was fine dropping it eventually — this is their own fork, no other consumers — but do this last, once `TEMPLATE_WIDGET_GRID` is proven end-to-end, not before).

Deferred, not blocking, still open from the original design's own backlog (`docs/superpowers/specs/2026-08-13-calendar-dashboard-vertical-slice-design.md`, "Deferred Decisions"):
- Authenticated/paired BLE (explicitly accepted as out of scope for now, including for the background-watch feature — the user chose "accept the risk for now" when asked).
- Seamless EPUB quick-resume without the CrossInk splash logo after a button wake from Agenda sleep (logged as a backlog item, never built).
- The original vertical slice's own hardware acceptance checklist items that were explicitly skipped this session at the user's request (stale-package-id physical retest, 20-page-turn heap-regression retest) — these cover the *old* single-slot persistence path, now generalized; if anyone wants that specific reassurance again it would need re-running, but nothing since has touched the parts it was checking.

## Environment notes for whoever continues this

- **`cmake` and `nix` are not available** in whatever sandboxed shell this session ran in, so the checked-in GoogleTest suites (`test/ble_handoff_record/*.cpp`) could not actually be run via `ctest`/`cmake --build`. Every new host-testable piece this session was instead verified with a throwaway `clang++ -std=c++20` harness (compiled and run directly against the relevant `.cpp` files, then deleted — never committed). **Before trusting any of this further, run the real suite** (`cmake --build build/test --target BleHandoffRecordTest && ctest` or however this project normally invokes it) in an environment that has the toolchain.
- `pio`, `xcodebuild`, `clang++`, `nm`/`riscv32-esp-elf-nm`, and `esptool` (via `~/.platformio/penv/bin/python -m esptool`) all work fine in that same sandbox and were used throughout, including flashing real hardware and installing the iOS app via `xcrun devicectl`.
- Physical devices, if still connected the same way: X3 at `/dev/cu.usbmodem31301` (confirmed ESP32-C3, XTEINK X3, partition table matches `partitions.csv`). iPhone via `xcrun devicectl` device id `837B3AD6-71E0-5E45-8CF6-F309BE23C1B5` ("iPhone 13"), auto-signed with team `F5M939PSPR`.
- There's a `~/.codex/skills/delegate-to-deepseek/` mechanism on this machine for delegating bounded, reversible implementation work to a DeepSeek-backed worker while an orchestrator keeps architecture/integration/final acceptance. It exists and was explained to the user once, but wasn't actually used this session — every task turned out small enough to do directly. Worth knowing about if a future chunk of work (e.g. the Swift codec port, which is fairly mechanical and cross-checkable against the C++ side) is a good fit for it.

## Working style established this session (worth continuing)

- No big spec/plan documents for incremental work — small milestones, each: write a failing test first where host-testable, implement, verify green, build the real firmware/app, commit, push. State explicitly in the commit message what could and couldn't be verified.
- Ask before architecture pivots that would redo prior work (this session did that twice: once before committing to the widget-grid redesign, once before accepting the reduced security posture for background BLE) — don't guess on decisions that are really the user's to make.
- Never leave the user's actual daily-use path (currently: `TEMPLATE_AGENDA`, working, physically verified) broken or ambiguous mid-refactor. Every persistence/render change this session kept the old template's behavior byte-for-byte intact and gated the new behavior behind template-id dispatch.
