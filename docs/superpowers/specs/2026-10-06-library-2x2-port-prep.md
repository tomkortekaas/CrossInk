# Port prep — 2×2 Library onto the installed v1.6 line

Date: 2026-10-06. Status: preparation, no code changed.

## Source identity (checked)

- `tools/version-management/scripts/check_state.py` against `X3-actueel/deployment-state.json`: **PASS**.
- Repo `CrossInk-library-x3-current`, branch `feat/x3-current-library-v1.6.1`, HEAD `49ae68aa`
  (installed source `c4146465` + docs only). SDK `8480abf9`.
- Installed artifact `fcdc51bb…` in **app0**; device restored to it on 2026-10-06
  (`/Volumes/2TB/x3-flash-backups/2026-10-06-restore/verification.json`).
- The earlier 2×2 build (origin/feat/agenda-8a, `ac0e8118`) is on the stale v1.5 line and is
  kept only as a reference implementation. Do not flash it.

## What v1.6.1 already has (keep)

- Full SD library index (`lib/LibraryIndex/`): scan, metadata cache, bounded sort with SD spill,
  search, sort picker, list/grid views, Library settings. This replaces my index/sort/scan code
  entirely — none of `LibraryIndexCodec/Store/Sort` is ported.
- Grid view in `LibraryActivity::buildGrid` (`src/activities/library/LibraryActivity.cpp:1266`),
  `GRID_COLUMNS = 3`, `GRID_PAGE_SIZE = 9` (`:49-50`); cover size is computed from the body area.
- X3 buttons (no touch): side Up/Down = previous/next book, Left = sort picker, Right = menu,
  Confirm = open, long-press = book menu, Back = home. Hints read `Home · Open · Sort · Menu`.
  **Keep these** — the port changes the view, not the controls.
- Already always FAST refresh (`render()` → `renderer.displayBuffer()`), so no crackle today.

## Port scope (what changes)

1. **2×2 grid**: `GRID_COLUMNS 2`, `GRID_PAGE_SIZE 4`; cover sizing stays area-driven.
2. **Per-cover progress row** under each cover: thin bar + `%`, ✓ (drawn) when finished,
   empty when never opened. Today only the selected book's progress is loaded
   (`loadGridProgress`, `:375`); load it for the 4 visible books instead.
3. **Footer line for the selected book**: `Title · nog 3h 40m`. Move the existing title band
   (`:1290-1314`) down and add time-left via a shared `ReadingTimeEstimate` (same extraction as
   ref commit `cd1a03e7`; `DashboardTheme.cpp:153-192` is identical here).
4. **Text cover** (title + author) instead of the book icon when no cover is available.
5. **Do not retry undecodable covers** for books outside the recents: v1.6.1 only marks
   `RecentBook::CoverState::Missing` (`loadGridCover`, `:1395-1430`), so a progressive JPEG
   (JPEGDEC err=4) is re-parsed on every visit. Needs a persistent negative marker per book
   (e.g. a `.nocover` marker next to the thumbnail path) — decide during implementation.
6. **Refresh**: page change uses `ReaderUtils::displayWithRefreshCycle` (HALF only every N
   turns, reader setting) to clear cover ghosting without a crackle on every page; selection
   moves stay FAST. Cover generation currently requests one update per cover — batch per page.

## Open design choice — screen chrome in grid mode (decides cover size)

Current v1.6.1 grid stacks: header 72 · sort band 44 · spacer 16 · title band ~30 · body ·
page dots 24 · file-count footer 28 · button hints 40. Estimated cover size on 528×792:

| Option | Removed in grid mode | Cover ≈ |
|---|---|---|
| Today (3×3) | — | 113×168 |
| A | title band + dots + count footer → one combined footer | 187×280 |
| B | A + sort band (sort name moves into the footer) | 201×302 |
| C | B + "Library" header | ~220×330 (as tested on 2026-10-06) |

## Deployment (when implemented)

Build env `dashboard-x3`. Full `read-flash` backup → `write-flash 0x10000 <artifact>` only →
readback compare → boot log → update `deployment-state.json` + `X3-actueel/firmware` per
`tools/version-management/SKILL.md`. Never `pio run -t upload`.
