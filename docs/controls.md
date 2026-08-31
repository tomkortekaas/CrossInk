---
title: Controls
nav_order: 6
---

# Controls

The Controls menu lets you customize front buttons, side buttons, and reader shortcuts.

## Settings Menu Layout

### Power Button

- Short-press action
- Long-press action

### Front Buttons

- Remap front buttons
- Remap front buttons while reading
- Orientation aware
- Long-press behavior (in-reader only)
- Long-press back action (in-reader only)
- Long-press menu action (in-reader only)

Note: Even though some actions assigned to the front buttons could be used globally, they are restricted to apply within the reader only due to the dynamic nature of the front buttons (they can mean different things based on the screen you're on).

### Side Buttons

- Layout
- Orientation aware
- Long-press action

## Side Button Long-press Action

When set to `Change Font Size`, hold a side button for about 2 seconds:

- Up increases font size
- Down decreases font size

When set to `Orientation Change`, hold a side button for about 2 seconds:

- Up cycles through the orientations in the following order: `Landscape CCW` -> `Inverted` -> `Landscape CW` -> `Portrait`
- Down cycles through the orientations in the following order: `Landscape CW` -> `Inverted` -> `Landscape CCW` -> `Portrait`

## X3 Previous-book Shortcut

On this branch, hold the **physical left side button** for at least **700 ms**, then release to open the previous available book at its saved position. Repeat to return to the other book. No library or selection window is opened.

This only activates on EPUB, TXT/Markdown and XTC document pages on X3 when **Side Buttons → Long-press action = Off** and side buttons are enabled. A configured chapter, font-size or orientation action keeps priority. Short left presses still use the configured page direction, but turn on release; the right button retains its existing timing. Front, power and reader-menu controls are unchanged. Missing recent files are skipped; with no other available book a long hold does nothing.

The physical left hold stays on the left when page directions or screen orientation are changed. See the [test checklist](testing/x3-previous-book-shortcut.md) for verification and the [installation record](deployments/2026-08-31-x3-book-switch.md) for build/flash status.

## Power, Back, and Menu Button Actions

Defaults:

- Short-press Power Button Action: Ignore
- Long-press Power Button Action: Sleep
- Long-press Back Button Action: Browse Files
- Long Press Menu Button Action: Ignore

Available actions include:

- Ignore
- Sleep
- Page Turn
- Refresh Screen
- Change Font
- Guide Dots
- Bionic Reading
- Toggle Bookmark
- Sync Progress
- Mark as Finished
- Reading Stats
- Take Screenshot
- Auto Page Turn Interval
- File Transfer
- Calibre Wireless
- Join a Network
- Create Hotspot
- Tilt Page Turn (X3 only)
- Footnotes
- Dark Mode
- Browse Files
- Create Clipping
- Look Up Word

## Footnote Shortcut

When a shortcut is mapped to Footnotes, the shortcut opens the footnotes submenu while reading. If the current page has only one footnote, CrossInk opens that referenced page directly.

The **Quick-return from Footnotes** setting controls whether the Power button acts like Back after opening a footnote page, making it faster to return to the original reading position.
