# Widget-grid scale-up — Handoff

**Date:** 2026-08-15
**For:** whoever (human or agent) picks this up next, in a fresh chat with no memory of this session.
**Supersedes:** `2026-08-14-overnight-fixes-handoff.md` — that document's diagnosis has been confirmed correct
and its next steps completed. Read this one instead.
**Repos, both clean, everything committed, nothing pushed:**
- Firmware: `/Volumes/2TB/Development/Projects/CrossInk`, branch `feat/ble-handoff`, HEAD `6bcd512c`.
- iPhone app: `/Volumes/2TB/Development/Projects/xteink-x3-dashboard-ios`, branch `feat/native-ios-app`, HEAD `6e54b86`.

## Where this session started

The previous handoff's bug (X3 never accepting a `TEMPLATE_WIDGET_GRID` package) was confirmed fixed on real
hardware: a widget-grid package was accepted first try (`persistIfNewer status=0 detail=0`) and rendered on
the X3. From there the session did three things: (1) raised the wire-format ceiling that previous sessions had
flagged as the next real constraint, (2) built out Home Assistant support end-to-end, and (3) hit and worked
through the first real-world consequence of the flashing fix from the prior session.

## 1. Widget/package limits raised (CrossInk `fded0ae2`, `6bcd512c`; iOS `04012f7`, `3d67073`)

Two changes, in order, both host-verified and hardware-flashed:

- **`fded0ae2`** — moved list content out of every `Widget` and into a small pool on the package
  (`WidgetGridPackage::lists`, indexed by `Widget::listIndex`). A `Widget` used to carry both `KpiContent`
  (34 B) and `ListContent` (382 B) even though only one is ever used. `sizeof(Widget)`: 421 → 40 bytes.
  Wire format unchanged — encoding never wrote the unused variant. New cap: **`MAX_LIST_WIDGETS = 3`** per
  package (three full lists is already ~975 bytes of content, well past the old 256-byte budget, so nothing
  reachable on the wire is lost).
- **`6bcd512c`** — raised `MAX_PACKAGE_SIZE` 256 → 1024 and `MAX_WIDGETS` 8 → 24 (one per grid cell,
  `GRID_COLUMNS * MAX_ROW_SPAN`). Measured: three KPI tiles + a full six-row agenda now fits (~500 bytes);
  previously that combination didn't fit at all. Four buffers that scale with `MAX_PACKAGE_SIZE` moved from
  stack to `static` (they're now a kilobyte-plus each, too large for the C3's few-KB task stacks): the
  receiver's frame buffer, `persistIfNewer`'s `current` package, `decodeWidgetGridPackage`'s `candidate`, and
  the reader probe's decoded `package`. All four are single-threaded paths; decoding stays two-phase so a
  rejected package still can't clobber the caller's.

Measured cost on the receiver: RAM 6.2% → 8.0% of 320 KB, flash unchanged at ~10%. Both `env:default` and
`env:spike-ble-receiver-x3` build and were flashed to the device (see §3).

**iOS mirrors both limits exactly** (`WidgetGridPackageLayout.maxPackageSize/maxWidgets/maxListWidgets`), so
the composer can't build a package the X3 would refuse. One existing test flipped: "every agenda text field at
its maximum overflows the budget" is no longer true (232 bytes of text now fits inside 1024), so that
assertion became `XCTAssertNoThrow`.

**`STORAGE_VERSION` bumped 1 → 2** (`BleHandoffNvs.cpp`) because a `SlotRecord` written by the old format is a
different size. Consequence: **every device that gets this firmware forgets its stored dashboard** and needs
one fresh send from the phone before anything shows again — see §3, this is not a bug, it's documented in
`CHANGELOG.md` under this version.

Current sizes (from a throwaway `/tmp/sizes.cpp` harness, not committed):
```
Widget            =    40 bytes  (was 421)
WidgetGridPackage =  2136 bytes  (was 3400, at the new MAX_WIDGETS=24)
PackageBytes      =  1024 bytes  (was 256)
```

## 2. Home Assistant support, end to end (iOS `97017c4`, `04012f7`, `1f3f700`, `6e54b86`)

Delegated to DeepSeek for the app-layer plumbing (settings screen, entity picker, wiring), reviewed and fixed
by hand before committing. Total cost this session: ~$3.11 (DeepSeek) + earlier $5.05 (the original diagnosis,
see previous handoff) = ~$8.16.

- `HomeAssistantEntity` — decodes both `GET /api/states/<id>` (`decode`) and `GET /api/states` (`decodeList`),
  renders to a `KpiValue` inside the 16-byte wire budget (rounds float noise to one decimal,
  `unavailable`/`unknown` → "—", truncates on character boundaries so `dashboard::validateUtf8` can't reject
  the package).
- `HomeAssistantClient` — read-only REST client, injectable transport (no network in tests). `states(for:)`
  never throws and returns partial results: one bad entity must not blank a dashboard that only refreshes
  every 15 minutes.
- `HomeAssistantConfigurationStore` — base URL (UserDefaults) + token (**Keychain**,
  `kSecAttrAccessibleAfterFirstUnlock` — Core Bluetooth wakes this app in the background, possibly while the
  phone is locked; the default `WhenUnlocked` would make the token unreadable exactly then). A missing token
  reads as "not configured", not as a configuration with an empty token (which would surface as a 401 instead
  of the truth). **Both of these were bugs in DeepSeek's first draft, fixed before committing** — worth
  reviewing Keychain code from any delegated session carefully.
- Settings screen (`HomeAssistantSettingsView`) with a "Verbinding testen" action, entity picker
  (`HomeAssistantEntityPickerView`) with search over id and friendly name.
- `DashboardViewModel.resolveHomeAssistantValues` resolves every HA entity the current composition references
  on each refresh and feeds them to `WidgetGridComposer`.
- `HomeAssistantValueCache` (`6e54b86`, this session's last piece) — remembers each entity's last resolved
  value in UserDefaults (not Keychain — a sensor reading isn't a secret, and it must survive background
  relaunches). Deliberately without an age indicator: 16 bytes per field doesn't have room to spend on a
  timestamp. This exists because **the user's Home Assistant is only reachable from the home network** (no
  Nabu Casa, no Tailscale/WireGuard/DuckDNS/Cloudflare integration found via the `home-assistant` MCP), so
  every away-from-home refresh would otherwise blank every HA tile to "—". User is evaluating Tailscale as a
  fix for that; not set up yet.

**Verified against the user's real instance**: 1649 entities returned from `allStates()` via the app's "Verbinding testen" button. Not yet verified: rendering an HA entity on the actual e-ink screen (see §3 — no fresh send has landed on the X3 since the reflash that wiped its storage).

## 3. Today's real-hardware findings

### The `-t upload` flashing bug from the previous session is real and was hit again

The previous handoff's CONTEXT.md note (`pio run -e spike-ble-receiver-x3 -t upload` writes to `0x10000`
regardless of that env's `board_upload.offset_address`, silently overwriting the reader) was **confirmed by
directly causing the failure once** at the start of this session, before the note was trusted. Recovered with
`pio run -e default -t upload`. The correct receiver-flash command is documented in
`.claude/CONTEXT.md` and was used successfully twice more this session:
```bash
~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32c3 --port /dev/cu.usbmodem31301 --baud 921600 \
  write_flash 0x650000 .pio/build/spike-ble-receiver-x3/firmware.bin
```
Reader: `~/.platformio/penv/bin/pio run -e default -t upload --upload-port /dev/cu.usbmodem31301`.

### The X3 currently shows its "no appointments" fallback, not a stale grid

After flashing `6bcd512c` (both `app0` reader and `app1` receiver), the X3's NVS was wiped by the
`STORAGE_VERSION` bump. `SleepActivity::renderAgendaSleepScreen()` calls
`BleHandoffReaderProbe::renderDashboardCard()`, which returns `false` when `readLastKnownGood()` finds nothing
(`SleepActivity.cpp:812`), and the caller falls back to a static "AGENDA / No appointments" placeholder
(`SleepActivity.cpp:817-818`). **This is expected, not a regression** — every device updating past this
firmware version loses its stored dashboard, per the `CHANGELOG.md` entry under this version.

**Next step to actually see anything (widget grid or HA tiles) on the screen again:**
1. Ask the user to manually sleep the X3 with a short power-button press — it will **not** enter the agenda
   BLE-wake cycle on its own after a fresh flash; it boots into the normal awake reader and only starts
   sleeping/waking once its own idle timeout elapses (default 10 min) or is told to sleep now. Always ask for
   the manual press; waiting out the idle timeout wastes time for no benefit.
2. Within the next ~15-minute wake window, the phone must be in BLE range with the app foregrounded or
   backgrounded (auto-watch active).
3. That first successful send is unverified end-to-end for this session's changes — it's the one thing left
   to actually observe: a widget grid up to 24 cells, and/or an HA tile showing a live value, rendering
   correctly on real e-ink.

### Home network only, no remote access yet

`mcp__home-assistant__get_system_info` and `manage_config_entry` (checked for `cloud`, `tailscale`,
`duckdns`, `wireguard`, `cloudflare` domains) confirm: HA 2026.7.1, no cloud/VPN integration configured. The
app's configured base URL is a bare LAN IP (`http://192.168.1.178:8123`), which is why it works without an ATS
exception in `Info.plist` (ATS doesn't apply to bare IPs, only hostnames) — but also why it **only works on
the home network**. User has Tailscale already but is weighing battery cost before turning it on
permanently on iOS; decided to stay LAN-only for now. If that changes: **use the tailnet IP, not the
MagicDNS hostname** — the hostname is a domain name and would need an ATS exception to work over bare HTTP,
which nobody wants to add. No code changes needed on either side once the address changes; it's purely what
the user types into Settings.

## Environment gotchas from the previous session, still true

- `~/.platformio/packages/tool-cmake/bin/{cmake,ctest}` (4.0.3) and `tool-ninja` ship with PlatformIO. Use the
  real suite:
  ```bash
  export PATH="$HOME/.platformio/packages/tool-cmake/bin:$PATH"
  cmake -S test -B /tmp/crossink-test-build -DCMAKE_BUILD_TYPE=Release   # only needed once per fresh build dir
  cmake --build /tmp/crossink-test-build --target BleHandoffRecordTest -j8
  /tmp/crossink-test-build/ble_handoff_record/BleHandoffRecordTest
  ```
  53 tests pass at HEAD.
- iOS: `swift test` (195 tests pass at HEAD) and
  `xcodebuild -project X3DashboardApp.xcodeproj -scheme X3DashboardApp -destination 'generic/platform=iOS' build CODE_SIGNING_ALLOWED=NO`
  both need no device. Installing on the real phone:
  `xcrun devicectl device install app --device 837B3AD6-71E0-5E45-8CF6-F309BE23C1B5 <path-to-.app>` after an
  `-destination 'id=837B3AD6-71E0-5E45-8CF6-F309BE23C1B5'` build.
- X3 USB serial (`/dev/cu.usbmodem31301`) disappears on deep sleep; use a reconnect-tolerant capture loop
  (pattern in the previous handoff) if a fresh serial capture is ever needed again. Given the diagnostic detail
  byte added last session, a serial capture likely isn't needed anymore — the phone shows the rejection reason
  on screen now.

## Working style established, worth continuing

- TDD throughout: every new file this session was RED-GREEN, verified with real command output before moving
  on, including for the size-limit changes (wrote a test asserting the *old* limit failed before raising it).
- Delegating to DeepSeek works well for well-scoped, self-contained app-layer work (Keychain/settings/picker
  UI) but its Keychain code needed two real fixes before it was safe to ship — always review security-adjacent
  delegated code by hand, don't just check that tests pass.
- Verify claims from memory (this session's own CONTEXT.md note) by hitting the failure once before trusting
  it, when the cost of being wrong is a boot-loop — this session did, and the note was accurate, but it's worth
  the ten seconds of caution generally.
