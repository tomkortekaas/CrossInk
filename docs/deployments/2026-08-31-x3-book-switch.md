# X3 dashboard + previous-book shortcut — 2026-08-31

The personal X3 was flashed with `dashboard-x3`, keeping the in-process BLE dashboard receiver and standby refresh. Hold the physical left side button for at least 700 ms and release to reopen the previous available book. Only active when **Controls → Side Buttons → Long Press = Off** and side buttons are enabled. Existing long-press actions take precedence. See the [controls](../controls.md#x3-previous-book-shortcut) and [test checklist](../testing/x3-previous-book-shortcut.md).

## Evidence and limits

- Host gesture/previous-book tests passed; both `default` and `dashboard-x3` builds passed. Only `dashboard-x3` was installed.
- Installed image: 6,269,056 bytes, SHA-256 `83da656a5bc341207d3e206f72648b28c519b681c05ed2a08a59fa83053ea5f2`.
- Full pre-flash backup and actual partition/OTA inspection completed. At that time app1 (`0x650000`) was active. Only the application image was written, ending at `0x00c4afff` after sector alignment; bootloader, partition table, NVS, OTA metadata and filesystem were not written.
- Esptool 5.1.2 reported successful data-hash verification and requested a hard reset. This proves writing, not a successful interactive boot or book-switch test. On-device A/B position preservation, physical button mapping and dashboard operation after this update remain to be confirmed by the owner.
- The previously built standard image was not installed. Do not use `default` to replace this dashboard variant.

## Rebuilding

Use this source revision with FreeInk SDK commit `1ff020263cd2202ea79ce3eb811f5ac8489b8cde` plus the exact [battery-latch patch](x3-battery-latch.patch). The SDK working tree deliberately remains modified. In a fresh checkout at that SDK commit, first check and then apply the patch from inside `freeink-sdk`:

```sh
git apply --check ../docs/deployments/x3-battery-latch.patch
git apply ../docs/deployments/x3-battery-latch.patch
```

Do not apply it again to an already patched SDK, reset the SDK or update its revision to make the working tree clean. Build from the firmware root with `pio run -e dashboard-x3`. The installed binary was built before committing these source changes, so its embedded version has the base commit and `dirty` suffix; a later rebuild can have a different binary hash even with equivalent code.

Before any future serial installation, make a fresh backup and inspect that device's current partition/OTA selection. The recorded offset is not a universal instruction. Opening USB serial can reboot the X3; do not use repeated serial polling to inspect interactive settings.

The public release contains only the build-produced application image, checksum and metadata. Device-memory backups and private logs stay local. See the [machine-readable record](2026-08-31-x3-book-switch.json).
