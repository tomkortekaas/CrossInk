# Widget-grid editor — ontwerp

**Datum:** 2026-08-15
**Status:** goedgekeurd, nog niet geïmplementeerd
**Repo's:** firmware `/Volumes/2TB/Development/Projects/CrossInk` (branch `feat/ble-handoff`),
iPhone-app `/Volumes/2TB/Development/Projects/xteink-x3-dashboard-ios` (branch `feat/native-ios-app`).
**Voorgeschiedenis:** `docs/superpowers/plans/2026-08-15-widget-grid-scale-up-handoff.md`.

## Probleem

De widget-grid rendert vlak en inconsistent. Concreet:

- Elke KPI-tegel krijgt een volle zwarte omlijning (`DashboardGridRenderer.cpp:29`), lijst-widgets
  krijgen er geen. Het raster oogt daardoor half omkaderd.
- Er zijn maar twee lettergroottes in gebruik (`UI_10` en `SMALL`, ascender 21 en 17 px) op tegels van
  132 px hoog. Waarde en label liggen te dicht bij elkaar om hiërarchie te maken.
- Geen iconen, terwijl de gebruiker tegels op betekenis wil kunnen herkennen zoals in een
  kleurendashboard (weer, auto, beurs, agenda).
- Geen manier om één tegel te laten opvallen. Op een kleurenscherm doe je dat met rood/groen; op 1-bit
  e-ink bestaat dat gereedschap wel (vier vulniveaus) maar wordt het niet gebruikt.

## Wat we bouwen

Een globale stijl voor het hele dashboard plus een klein accent per tegel, bewerkbaar in de iPhone-app
via een inspector-sheet, meegestuurd in het pakket.

### Beslissingen en hun redenen

| Beslissing | Reden |
|---|---|
| Globale stijl + accent per tegel, niet volledige controle per tegel | Houdt de editor en het wire-formaat klein en het dashboard visueel samenhangend. |
| Eén letterfamilie (Lexend Deca), alleen groottes instelbaar | Lexend 10/12/14/16 zijn al geregistreerd in de reader (`main.cpp:780-787`); nul extra flash. |
| Iconen altijd handmatig kiezen | Schrapt de `mdi:`→Lucide-vertaling en een klasse van verrassende mismatches. |
| Gecureerde set van 64 iconen | ~13 KB flash (24 en 32 px, ~200 byte per icoon). De firmware kan alleen tekenen wat ingebakken zit. |
| Stijl reist mee in het pakket | Uiterlijk veranderen = één keer versturen. Het alternatief (thema's in firmware) zou een reflash per iteratie vragen, en flashen is in dit project het gevaarlijke deel. |
| Editor: tik tegel → inspector-sheet | De enige vorm die niet instort als er opties bijkomen. |

### Bewust niet gedaan

- **Telefoon rendert een bitmap, X3 toont 'm** (het Tesserae-model). Lost de dubbele implementatie in
  één klap op, maar 528×792 op 1 bit is 52 KB per update tegen ~500 bytes nu, over BLE, op een apparaat
  dat elke 15 minuten kort wakker wordt. Niet haalbaar op deze hardware en dit transport.
- **Conditionele stijlbytes** om ~40 bytes te sparen. Kost een tak in decoder, encoder en beide
  testsuites voor 5% van een budget dat op ~50% zit.
- **Lexend 18/20 toevoegen** voor een echt hero-getal (ascender 38/42 px). Pas overwegen als de
  vier-sporten-ladder op het echte scherm te vlak blijkt; dan eerst de flash-kosten méten, niet gokken.

## Wire-formaat

`schema` gaat van 1 naar 2. Dat is nu gratis: volgens de handoff staat er op de X3 op dit moment geen
pakket opgeslagen, dus niemand verliest iets. Er is **geen `STORAGE_VERSION`-bump nodig** — `SlotRecord`
(`BleHandoffNvs.cpp:23`) bewaart rauwe pakketbytes in een vast 1024-byte array en valideert alleen via
`peekPackageHeader`, dus `sizeof(SlotRecord)` verandert niet.

### Globale stijlbyte

Nieuw op offset 30. `contentOffset` gaat van 30 naar 31.

| bits | veld | waarden |
|---|---|---|
| 0–2 | `borderLevel` | 0 geen · 1 haarlijn tussen tegels · 2 lichte dither-rand · 3 volle zwarte rand (huidig gedrag) |
| 3–4 | `density` | 0 compact (4 px padding) · 1 normaal (8) · 2 ruim (14) |
| 5 | `listDividers` | 0 uit · 1 aan |
| 6–7 | reserve, moet 0 zijn | |

Decoders **moeten** een pakket weigeren waarvan een reserve-veld niet 0 is, zodat een toekomstige
uitbreiding niet stil verkeerd gerenderd wordt.

### Twee stijlbytes per widget

Little-endian `uint16_t`, direct achter de bestaande `type, column, row, columnSpan, rowSpan` en vóór
de type-specifieke lengtebytes.

| bits | veld | waarden |
|---|---|---|
| 0–6 | `iconId` | 0 = geen icoon, 1–64 = index in de gecureerde set |
| 7–8 | `sizeRung` | 0 Lexend 10 (asc 21) · 1 Lexend 12 (25) · 2 Lexend 14 (30) · 3 Lexend 16 (34) |
| 9–10 | `emphasis` | 0 geen · 1 licht (25% dither) · 2 donker (50% dither) · 3 geïnverteerd |
| 11–15 | reserve, moet 0 zijn | |

`iconId > 64` is ongeldig. Kosten: 1 byte globaal + 2 per widget = 49 bytes bij het maximum van 24
widgets, binnen `MAX_PACKAGE_SIZE` van 1024.

## Firmware

`renderKpiWidget` in `src/spikes/ble_handoff/DashboardGridRenderer.cpp`:

- Vulling volgens `emphasis` via `GfxRenderer::fillRectDither()` — puur 1-bit ordered dithering
  (`drawPixelDither`: LightGray = 25%, DarkGray = 50%), dus géén grijswaarde-refresh en geen extra
  kosten.
- Icoon via `drawIcon()`, of `drawIconInverted()` wanneer `emphasis == 3`.
- Tekst in de `sizeRung`-font, met `drawText(..., black=false)` op geïnverteerde tegels.
- Padding uit `density` in plaats van de vaste `TILE_PADDING = 6`.

De omlijning verhuist van per-tegel naar één doorloop over het hele raster in `renderWidgetGrid`, zodat
lijst-widgets dezelfde behandeling krijgen als KPI's. Dat lost de bestaande inconsistentie op.

Iconen: `src/components/icons/dashboardIcons.h`, gegenereerd door
`freeink-sdk/libs/assets/Icons/tools/gen_icons.py` uit een manifest, op 24 en 32 px.

## iOS

`WidgetCompositionView`: tik op een tegel in de preview selecteert 'm; een sheet toont type, icoon,
grootte en nadruk, met live terugkoppeling in de preview. De bestaande `.highPriorityGesture` voor
slepen blijft ongewijzigd — zie de comment op regel 126 over waarom SwiftUI's eigen drag-API het daar
verloor van de omsluitende `List`. Tap komt ernaast via `.simultaneousGesture`.

## Hoe de twee kanten niet uit elkaar lopen

Dit is het grootste risico van het ontwerp: elke stijlknop bestaat twee keer, in C++ en in SwiftUI. Het
open-source project Tesserae vermijdt dit door editor en productie dezelfde renderer te laten gebruiken;
dat kan hier niet.

Twee tegenmaatregelen:

1. **Eén manifest.** `dashboard-icons.txt` (één Lucide-naam per regel, volgorde bepaalt de `iconId`)
   genereert zowel de C-header als het Swift-enum en de asset-catalog. Geen handonderhouden lijst aan
   twee kanten.
2. **Een parity-harness.** Dezelfde stijlspec door de C++- en de Swift-encoder halen en de bytes
   diffen; en dezelfde bytes door beide decoders halen en de velden vergelijken. Draait op de host, geen
   hardware nodig.

## Verificatie

- Firmware host-tests: `cmake --build /tmp/crossink-test-build --target BleHandoffRecordTest -j8` en de
  binary draaien (53 tests slagen op HEAD). `export PATH="$HOME/.platformio/packages/tool-cmake/bin:$PATH"`.
- iOS: `swift test` in de app-repo (195 tests slagen op HEAD).
- Op hardware: pas te zien na een verse send. De X3 moet handmatig in slaap (korte druk op de
  aan-knop) en de telefoon binnen ~15 minuten in BLE-bereik.

## Werkverdeling

Naar DeepSeek: het wire-formaat aan beide kanten plus de parity-harness, de icon-generatiepijplijn, en
het icon-picker-scherm. Bij Claude: de C++ renderer (stackbudget, `CLAUDE.md`-regels), de
tap-naast-drag-interactie, en review van al het gedelegeerde werk — de vorige sessie leverde
DeepSeek-Keychain-code op met twee echte fouten die alleen handmatige review ving.
