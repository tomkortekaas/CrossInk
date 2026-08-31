# X3: previous-book shortcut

Hold the physical left side button for at least 700 ms, then release to reopen the previous available book. Repeat to return. Short presses retain their configured page direction, but now turn on release; the right button retains press timing.

The shortcut only activates on X3 document pages (EPUB, TXT/Markdown and XTC), with **Controls → Side Buttons → Long Press = Off** and side buttons enabled. Chapter skipping, font-size and orientation holds take precedence and are never overwritten. Reader menus, front buttons and power shortcuts are unchanged. No settings migration or write is performed.

Physical mapping follows this firmware's `BaseTheme::drawSideButtonHints` and `LyraTheme::drawSideButtonHints`: BTN_UP is the left X3 edge, BTN_DOWN the right. The shortcut bypasses orientation remapping but short page turns keep the configured direction. This mapping still needs confirmation on the actual device.

## Local verification

From the firmware root:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Isrc test/previous_book_shortcut/PreviousBookShortcutTest.cpp -o /tmp/x3-previous-book-test
/tmp/x3-previous-book-test
pio run -e dashboard-x3
```

The host test covers short/long/boundary timing, single release, held-on-entry, interrupted gestures, chords, timer wraparound, A/B selection, duplicate current entries and missing targets. It does not prove physical input mapping, display behaviour or SD progress persistence.

## Hardware acceptance (not yet performed)

1. Check the current long-press setting. If it is not Off, do not replace it without the owner's agreement.
2. Open book A and advance several pages. Open B and advance several pages.
3. Hold/release the left side button: A must open at its saved position; repeat: B must open at its saved position. No extra page turn or repeated switch while held.
4. Short left and right presses must still turn exactly one page in their configured direction. Repeat with swapped page directions and screen rotation: the physical left hold stays the switch.
5. Check front/menu/back/power shortcuts and navigation inside reader menus. They must retain their previous behaviour.
6. With only one available book, hold does nothing. A deleted recent book is skipped. With a configured chapter/font/orientation hold, that action remains in control.
7. Sleep/wake and repeat A/B switching; both positions must remain saved. No cache reset is required.

Do not open a USB serial monitor to inspect settings: doing so has rebooted this user's X3. Installation was completed on 2026-08-31 with a verified flash write and a requested reset; see the [deployment record](../deployments/2026-08-31-x3-book-switch.md). The functional hardware checklist above is still pending.

Deployment note: this personal X3 requires `dashboard-x3` (in-process BLE receiver and standby refresh). The `default` build compiles the shortcut but must not replace this dashboard firmware. Read the installed partition/OTA selection before writing: on 2026-08-31 the device booted app1 at 0x650000. Preserve the bootloader, partition table, NVS, OTA metadata and filesystem.
