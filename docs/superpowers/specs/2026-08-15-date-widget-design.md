# Datumwidget — ontwerp

**Datum:** 2026-08-15
**Status:** goedgekeurd, nog niet geïmplementeerd
**Repo's:** firmware `/Volumes/2TB/Development/Projects/CrossInk` (branch `feat/ble-handoff`),
iPhone-app `/Volumes/2TB/Development/Projects/xteink-x3-dashboard-ios` (branch `feat/native-ios-app`).
**Voorgeschiedenis:** `docs/superpowers/specs/2026-08-15-widget-grid-editor-design.md`.

## Probleem

Het dashboard kent twee widgettypes, `Kpi` en `List` (`DashboardWidgetGrid.h:108`). Er is geen manier
om de datum te tonen. Dat kan vandaag alleen door een KPI-tegel met tekst te vullen vanaf de iPhone,
en dat heeft twee problemen:

- **De tekst veroudert.** Het pakket wordt één keer verstuurd en daarna bewaard; de X3 tekent elke
  kwartierwake opnieuw uit diezelfde bytes (`BleHandoffReaderProbe.cpp:46`,
  `AgendaWakePolicy.h:14`). Een ingebakken "vrijdag 15 augustus" staat er de dag erna nog steeds,
  ook al is het scherm sindsdien zestig keer opnieuw getekend.
- **Het kost ruimte die er niet is.** Een KPI-tegel met label en waarde kost ongeveer 25 van de 256
  pakketbytes, voor informatie die het apparaat zelf al heeft: er is een RTC
  (`halClock.getDateTime(...)`, `main.cpp:303`).

## Wat we bouwen

Een derde widgettype dat de datum uit de RTC leest en zichzelf tekent, met een keuze welk datumveld
de tegel toont en een indeling die zich richt naar de tegelmaat.

### Beslissingen en hun redenen

| Beslissing | Reden |
|---|---|
| Nieuw `WidgetType::Date = 3`, geen tekst op de draad | 8 bytes in plaats van ~25, en de tegel blijft kloppen als de iPhone dagenlang weg is. |
| Namen en weeknummer in de firmware, niet in het pakket | De 7 weekdag- en 12 maandnamen passen niet in 256 bytes en veranderen nooit. |
| Veldkeuze in de payload, niet in de reservebits van het stijlwoord | *Welk* datumveld je toont is inhoud, geen stijl. De vijf vrije bits (`widgetStyleReservedMask = 0xF800`) blijven zo beschikbaar voor stijl van álle widgettypes. |
| Indeling op beschikbare pixels, niet op `columnSpan`/`rowSpan` | Dan vallen ook 4×1 en 3×2 vanzelf goed, zonder tabel van spancombinaties. De renderer kiest al op tegelhoogte welke icoonrasterisatie hij pakt; dit is dezelfde denkwijze. |
| Geen provider in de app | Er valt niets op te halen. Dit is het eerste widgettype zonder databron. |
| Alleen Nederlands | Eén gebruiker, Nederlandse app. Een taalbyte is later één byte en één tabel. |

### Bewust niet gedaan

- **De datum door de iPhone laten invullen**, al dan niet als fallback op een onbetrouwbare RTC. Kost
  een tweede codepad en een tweede bron van waarheid voor iets wat het apparaat zelf weet. De
  placeholder bij een kapotte klok is eerlijker dan een verzonnen datum.
- **Hergebruik van `Kpi` met een "vul uit de klok"-vlag.** Bespaart protocolwerk, maar de KPI-tegel
  rendert een vaste stapel icoon/waarde/label — het kalenderblad met kop past daar niet in, en de
  maat-afhankelijke indeling zou in de KPI-render belanden waar hij niet thuishoort.
- **Tijd tonen.** De X3 wordt elk kwartier wakker; een klok die kwartieren verspringt is erger dan
  geen klok.
- **Een taalbyte** voor NL/EN. YAGNI.

## Wire-formaat

De widgetkop is ongewijzigd: 7 bytes (type, kolom, rij, kolomspan, rijspan, stijlwoord u16), zie
`readRawWidget` in `WidgetGridPackage.swift` en `readWidget()` in `DashboardWidgetGrid.cpp`. Een
`Date`-widget voegt daar één byte aan toe:

| Offset | Veld | Waarden |
|---|---|---|
| +7 | `field` | `0` auto, `1` dag, `2` weekdag, `3` maand, `4` jaar, `5` weeknummer |

Totaal 8 bytes per datumtegel. Waarden boven `5` worden geweigerd met `invalidArgument`, zoals de
bestaande begrensde velden. Er is geen tekst, dus geen lengtevelden en geen `PackageText.validate`.

`MAX_WIDGETS` en de lijstpool (`MAX_LIST_WIDGETS`) blijven ongemoeid: een `DateContent` is één byte.

Het schema verandert niet. Firmware die `type == 3` niet kent, weigert het pakket via de bestaande
`default`-tak, en de app toont die weigeringsreden al.

## Firmware

Een `renderDateWidget(renderer, rect, field, style)` naast de bestaande tegelrenders in
`DashboardGridRenderer.cpp`.

**Klok.** `halClock.getDateTime(year, month, day, hour, minute)`. Levert die `false`, of een jaar
vóór 2025, dan toont de tegel een streepje in plaats van een datum. Die jaargrens spiegelt
`MIN_TRUSTED_MIGRATED_RTC_YEAR` in `CrossPointSettings.cpp:43`, zodat het dashboard dezelfde klok
wantrouwt als de rest van de firmware.

**Afgeleide waarden.** De HAL geeft geen weekdag; die rekent de renderer uit de datum. Het
weeknummer is ISO-8601 (week 1 is de week met de eerste donderdag). Beide zijn pure functies van
`(jaar, maand, dag)` en horen in een apart bestandje met eigen tests, los van de renderer.

**Namen.** Vaste NL-tabellen in flash: 7 weekdagnamen en 12 maandnamen, plus hun afkortingen
(weekdagen twee letters — `ma`, `di`, `wo`, `do`, `vr`, `za`, `zo`; maanden drie — `jan` t/m `dec`).

**Indeling.** Bij `field != 0` toont de tegel dat ene veld, zo groot als past, met waar zinnig een
klein label erboven (`week` boven `33`). Bij `field == 0` kiest de renderer op de binnenmaat van de
tegel ná padding uit vier indelingen:

| Binnenmaat | Indeling |
|---|---|
| smal en laag | weekdag klein, dagnummer groot |
| breed en laag | icoon + `za 15 aug` op één regel, weeknummer erbij als het past |
| smal en hoog | weekdag, dagnummer groot, maand afgekort, gestapeld |
| breed en hoog | kop met maand + jaar in omgekeerde emphasis, dagnummer groot, weekdag voluit, weeknummer |

De drempelwaarden worden bij het bouwen afgesteld tegen `getLineHeight(fontId)` en de gemeten
tekstbreedtes, niet vooraf op papier vastgelegd. De regel die ze moet halen: een gekozen tekst mag
nooit buiten zijn tegel vallen. Past een naam niet op de gekozen grootte, dan valt de renderer terug
op de drieletterafkorting; past die ook niet, dan op een kleinere fontgrootte.

`sizeRung` in het stijlwoord blijft een bovengrens: de indeling mag naar beneden afwijken als het
anders niet past, maar nooit naar boven. Een `iconId` wordt alleen getekend in de brede lage
indeling, waar breedte over is; in de andere indelingen concurreert hij met het dagnummer en wordt
hij genegeerd.

## iPhone-app

- `DashboardWidgetContent.date(field:)` naast `.kpi` en `.list`, met een `DateField`-enum die de zes
  waarden hierboven spiegelt.
- Encoder en decoder in `WidgetGridPackage.swift`, byte-identiek aan de C++-kant.
- Composer: tegeltype "Datum" met een veldkiezer (auto, dag, weekdag, maand, jaar, weeknummer).
- Preview: een SwiftUI-benadering van dezelfde indelingsladder, op de manier waarop de bestaande
  previews de firmware benaderen — een benadering, geen tweede implementatie van de waarheid.
- Geen `...Provider`: er is geen databron.

## Tests

**Swift.** Roundtrip van elk `field`; een geweigerde waarde `6`; de 8-byte lengte; een handgeschreven
bytevector voor een `Date`-widget in een pakket met een KPI-tegel ernaast.

**C++.** Dezelfde handgeschreven bytevector aan de decodeerkant. Plus tabeltests voor weekdag en
ISO-week op de plekken waar ze breken: schrikkeljaren, 29 februari, 1 januari in een jaar dat op
donderdag begint, 31 december in week 1 van het volgende jaar.

**Pariteit.** Een draaiende clang++/swiftc-harness die aantoont dat de Swift-encoder en de
C++-decoder dezelfde bytes over hetzelfde pakket eens zijn. Dit is het soort bewijs dat in dit
project eerder een bug vond die vier sessies gemist hadden; een analyse zonder draaiende harness
telt niet.

**Hardware.** Niet geautomatiseerd: één keer versturen naar de X3 en met eigen ogen zien dat de
tegel klopt en de dag erna is omgeslagen.

## Werkverdeling

- **DeepSeek** (Pro, per repo met `--dir`): het wire-formaat aan beide kanten — de Swift-encoder en
  -decoder, de C++-decoder, de bijbehorende unit tests, en de pariteitsharness met echte
  commando-output als bewijs.
- **Claude:** dit ontwerp, de firmware-renderer en de indelingsladder, de composer-UI en de preview,
  en de review van DeepSeeks diff.

## Aandachtspunt bij aanvang

Beide repo's hebben op dit moment ongecommit werk, waaronder 48 nieuwe regels in
`DashboardGridRenderer.cpp` — precies het bestand dat dit ontwerp uitbreidt. Dat eerst afronden of
apart zetten voordat het werk begint.
