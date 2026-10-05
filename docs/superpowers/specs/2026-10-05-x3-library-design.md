# X3 Library — design

Date: 2026-10-05. Status: approved by Tom in conversation; not implemented.

## Goal

A library screen that shows **every book on the SD card** as large covers, so choosing
a book feels like looking at a bookcase rather than a file browser. Continuing the
current book stays on Home (recent books); the library is for choosing.

## Layout (X3 portrait, 528 × 792)

- 2 × 2 covers, 2:3 aspect, **220 × 330** each (3.3× the area of the current
  RecentBooksGrid cover of 123 × 180). Four covers ≈ 69 % of the screen.
- No header. Top margin 8 px, side margins 36 px, column gap 16 px, row gap 12 px.
- Under every cover a 22 px row: a thin progress bar with a percentage. Finished books
  show a ✓ instead of a percentage; never-opened books show an empty bar and no number.
- A 28 px footer line for the **selected** book: `Title · <time left>` (time left uses
  the existing reading-stats estimate; omitted when unknown).
- The standard firmware button-hint bar stays at the bottom (40 px):
  `Home · Open · ‹ · ›`, drawn with `GUI.drawButtonHints` like every other screen.
- Books without a cover image: a framed box with the title (and author) wrapped
  in large text instead of a small icon.
- Geometry is computed from the renderer size, not hard-coded, so the X4
  (480 × 800) gets 216 × 324 covers from the same code.

## Content

- All supported books (`.epub`, `.xtc`/`.xtch`, `.txt`, `.md`) anywhere on the card,
  **flattened** (folders ignored). Hidden entries (leading `.`, so `/.crosspoint` too)
  and OS metadata entries are skipped. Expected size: 50–200 books; hard cap 300.
- Persistent index at `/.crosspoint/library_index.bin` (path, title, author,
  "first seen" sequence number, status). Rescanned on each library open and merged:
  new paths are appended, vanished paths dropped, known metadata kept.
- Title/author for new books are read once (EPUB/XTC metadata; filename stem for
  TXT/MD or when metadata fails) during the merge, behind a progress popup.
- Status per book: **In progress** (in the recent-books list and not finished),
  **Finished** (reading stats `isCompleted`), **New** (otherwise).

## Sorting (user-selectable, persisted as a setting)

1. **In progress first** (default): in-progress books in recent order, then new
   books by title, then finished books by title.
2. **Recently added**: newest "first seen" first. `HalFile` exposes no file dates,
   so "added" means "first seen by the library index"; the very first scan gives
   every book the same generation and falls back to title order.
3. **Title**.
4. **Author** (books without an author last).

Changeable from Settings → Display and from the library's long-press menu.

## Controls

- Left / Right: previous / next book (wraps).
- Side Up / Down: previous / next page (lands on the page's first book, the existing
  `ButtonNavigator::nextPageIndex` behaviour used elsewhere in the firmware).
- Confirm: open the book. Long-press Confirm: a book menu with the actions that make
  sense while choosing — mark finished/unfinished, delete cache, delete book — plus
  "Sort library". (Reader-specific actions stay in Recent Books.)
- Back: Home.
- Entry point: a new "Library" item in the Home menu. Recent Books stays unchanged.

## E-ink behaviour

- A selection move redraws with FAST refresh; a page change uses HALF refresh to
  clear ghosting.
- Selection is the existing double rounded frame around the cover (no black fills).
- Covers are 1-bit thumbnails generated per page on first visit (existing
  `generateThumbBmp` pipeline, cached per size on SD), with the existing loading popup.

## Out of scope

Folder shelves, search, collections, grayscale covers, changes to Home's recent-books
carousel or to RecentBooksGridActivity.
