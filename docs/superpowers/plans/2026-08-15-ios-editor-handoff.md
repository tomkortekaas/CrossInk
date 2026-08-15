# iPhone-app: stijl-editor en opschoning hoofdscherm — Handoff

**Datum:** 2026-08-15
**Voor:** wie dit oppakt in een verse chat, zonder geheugen van deze sessie.
**Volgt op:** `2026-08-15-widget-grid-scale-up-handoff.md` (afgerond) en
`docs/superpowers/specs/2026-08-15-widget-grid-editor-design.md` (het goedgekeurde ontwerp — lees die
eerst, met name "Wire-formaat").

**Repo's, beide schoon, alles gecommit, niets gepusht:**
- Firmware: `/Volumes/2TB/Development/Projects/CrossInk`, branch `feat/ble-handoff`, HEAD `55b2c2c2`.
- iPhone-app: `/Volumes/2TB/Development/Projects/xteink-x3-dashboard-ios`, branch `feat/native-ios-app`,
  HEAD `df39b64`.

## Wat werkt, op echte hardware geverifieerd

De hele keten draait op het nieuwe formaat: telefoon → receiver (app1) → reader (app0) → e-ink. Een
dashboard met gestileerde tegels is met eigen ogen op het scherm gezien.

- **Wire-formaat schema 2**: één globale stijlbyte (randniveau, dichtheid, scheidingslijnen) plus twee
  bytes per widget (icoon-id, groottesport, nadruk). 49 bytes bij het maximum van 24 widgets.
- **Vier vulniveaus werken echt.** Wit, 25% dither, 50% dither, geïnverteerd — op het paneel duidelijk
  onderscheiden, en het lichte raster leest als grijs, niet als vuil. Alle vier zijn bruikbaar.
- **Iconen**: 64 Lucide-iconen, gegenereerd op 32 en 48 px uit één manifest dat zowel de firmware-header
  als de iOS asset-catalog voedt. 24 px is geprobeerd en onbruikbaar gebleken: te dunne lijnen om te
  overleven bij het rasteren.
- **Groottesladder**: Lexend Deca 10/12/14/16 (ascender 21/25/30/34 px). Werkt, maar oogt braaf — zie
  "Open vragen".
- **Bezelmarge**: het raster was tot deze sessie vanaf `0,0` getekend; de buitenste pixels van de X3
  zitten onder de bezel.

## Wat er nu op het apparaat staat

- Reader (app0): env **`fasttest`**, niet `default`. Dat is een lokale env uit `platformio.local.ini`
  (gitignored) die het BLE-wakker-interval van 15 naar **1 minuut** zet, zodat testen niet per poging een
  kwartier kost. **Dit kost echt batterij** — zet `-e default` terug zodra het stylen klaar is, of gooi
  `platformio.local.ini` weg. De gecommitte standaard blijft 15.
- Receiver (app1): env `spike-ble-receiver-x3`, geflasht met de schema-2-acceptatie in
  `peekPackageHeader`. **Beide partities moeten mee** bij een formaatwijziging; alleen de reader flashen
  levert een receiver op die elk pakket weigert vóór het ooit wordt opgeslagen, met een foutmelding die
  naar de telefoon wijst in plaats van naar de flash.

Flashen, in deze volgorde (receiver eerst: gaat die mis, dan overschrijft hij `0x10000` waar de reader
staat, en de reader-flash erna repareert dat sowieso):

```bash
~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32c3 --port /dev/cu.usbmodem31301 --baud 921600 \
  write_flash 0x650000 .pio/build/spike-ble-receiver-x3/firmware.bin

~/.platformio/penv/bin/pio run -e fasttest -t upload --upload-port /dev/cu.usbmodem31301
```

Gebruik **nooit** `pio run -e spike-ble-receiver-x3 -t upload`: die schrijft naar `0x10000` ongeacht de
`board_upload.offset_address` van die env.

## De opdracht

### 1. De stijl-editor bouwen

Dit is waar het om begonnen is: de gebruiker kan de stijlen nu niet zelf zetten. Er staat een tijdelijke
**stijl-sampler** in de weg die dat gat overbrugt (zie hieronder) — die moet weg zodra de editor er is.

Goedgekeurde vorm, uit het ontwerp: **tik op een tegel in de preview → inspector-sheet eronder**, met vier
rijen: type · icoon · grootte · nadruk. Live terugkoppeling in de preview terwijl je schuift. Plus de
globale stijl (randniveau, dichtheid, scheidingslijnen) ergens op compositieniveau.

Alles eronder ligt klaar: `WidgetSlot` heeft `iconId`/`sizeRung`/`emphasis`, `WidgetGridComposition` heeft
`borderLevel`/`density`/`listDividers`, `DashboardIcon` mapt icoon-id op asset-naam en leesbare naam, en
de preview rekent al met dezelfde ingesnoerde canvas als de firmware.

**Het enige echt riskante stukje** is tap-naast-slepen. Lees de comment op
`WidgetCompositionView.swift` regel ~126 vóór je begint: SwiftUI's eigen `.draggable`/`.dropDestination`
én een gewone `.gesture` verloren daar allebei van de scroll-recognizer van de omsluitende `List`; het
werkt nu met `.highPriorityGesture`. Tap moet daarnaast passen zonder het slepen te breken —
`.simultaneousGesture` is het vertrekpunt, niet het bewezen antwoord.

### 2. Het hoofdscherm opschonen

`ContentView.swift` is meegegroeid met drie generaties functionaliteit en die staan nu door elkaar. De
gebruiker heeft hier expliciet om gevraagd. Concreet aangetroffen:

- **De titel is `"X3 Agenda"`.** Het is allang geen agenda-app meer maar een dashboard-app; de agenda is
  één widgettype van meerdere.
- **`"Gekozen afspraak"` staat prominent** in de Kalender-sectie. Dat is een overblijfsel uit de tijd dat
  het apparaat één afspraak toonde (`TEMPLATE_AGENDA`).
- **`"Klassieke kaart"`** verschijnt als waarde bij Widgets zodra er geen compositie is. Dat is
  ontwikkelaarstaal voor een fallback, geen gebruikersinformatie.
- **Diagnostiek en acties lopen door elkaar.** Toegangsstatus, verbindingsstatus, laatste weigeringsreden
  en foutmeldingen staan tussen de knoppen. Overweeg één diagnose-sectie, of een aparte pagina.
- **Verstrengeling met echte gevolgen:** `buildPackage()` begint met `guard card != nil` — de legacy
  agenda-kaart — vóór de widget-grid-tak, en `canSend` doet hetzelfde. `card` is alleen nil zonder
  kalendertoegang, dus het gaat in dagelijks gebruik niet mis. Maar het betekent wel dat een volledig
  samengesteld dashboard van stappen, batterij en Home Assistant **niet verstuurd kan worden als de
  gebruiker kalendertoegang weigert**. De nieuwe functionaliteit hangt aan de oude. Dit is de plek waar
  opruimen ook echt gedrag repareert.

Beslis bewust of `TEMPLATE_AGENDA` blijft. Het is een compleet tweede pad (`DashboardPackage`,
`CalendarCardFormatter`, `card`, `selectedEvent`, `nextEvent`) dat alleen nog dienst doet als er géén
compositie is. Een lege compositie met één agenda-widget doet hetzelfde. Weghalen scheelt een hoop
oppervlak — maar het is wél het pad dat bewezen werkt, dus haal het niet weg zonder de vervanging op
hardware te hebben gezien.

### 3. Weghalen zodra 1 klaar is

- `Sources/DashboardCore/StyleSampler.swift`
- De sectie "Stijlvoorbeeld" en `borderLevels` in `ContentView.swift`

Die sturen een dashboard waarin elke stijloptie naast elkaar staat, puur omdat de stijlen anders niet te
zien zijn. Ze **overschrijven de opgeslagen compositie van de gebruiker**, wat prima is voor een testknop
maar niet voor iets dat blijft staan.

## Open vragen, met wat we al weten

- **Is Lexend 18/20 nodig?** De ladder werkt maar oogt braaf; Lexend 16 bovenaan is "iets groter", geen
  hero-getal dat de tegel draagt. Grotere fonts zouden dat geven. **Meet eerst**: de reader zit op
  **91,1% flash** van 6,55 MB, dus ~580 KB over, en de schatting van ~175 KB voor regular+bold is
  ongetoetst. Let op: de "~10% flash" uit oudere documenten sloeg op de *receiver*, niet op de reader.
- **De agenda-widget krijgt een eigen layout-ronde.** De gebruiker wil die in z'n geheel mooier maken.
  Eén concreet punt om daarin mee te nemen: nu staan er alleen tijden, dus afspraken van verschillende
  dagen ogen ongesorteerd (13:00, 13:00, 08:00…). Een dagkopje of `ma 08:00` lost dat op — controleer wel
  of dat in de 16 bytes van het tijdveld past. Bewust niet nu gedaan.
- **`GRID_MARGIN = 6`** in `DashboardGridRenderer.cpp` is de knop voor hoe ruim het raster staat. De
  Swift-kant spiegelt dat getal hardcoded in `DashboardGridLayout.x3Grid*`; die moet mee als je eraan
  draait.

## Verificatie

- Firmware host-tests (61 slagen op HEAD):
  ```bash
  export PATH="$HOME/.platformio/packages/tool-cmake/bin:$PATH"
  cmake -S test -B /tmp/crossink-test-build -DCMAKE_BUILD_TYPE=Release   # eenmalig
  cmake --build /tmp/crossink-test-build --target BleHandoffRecordTest -j8
  /tmp/crossink-test-build/ble_handoff_record/BleHandoffRecordTest
  ```
- iOS (212 slagen op HEAD): `swift test`.
- **`swift test` dekt het app-target niet.** Draai er altijd bij:
  ```bash
  xcodebuild -project X3DashboardApp.xcodeproj -scheme X3DashboardApp \
    -destination 'generic/platform=iOS' build CODE_SIGNING_ALLOWED=NO
  ```
- Installeren op het toestel:
  ```bash
  xcodebuild -project X3DashboardApp.xcodeproj -scheme X3DashboardApp \
    -destination 'id=837B3AD6-71E0-5E45-8CF6-F309BE23C1B5' -derivedDataPath /tmp/x3dd build
  xcrun devicectl device install app --device 837B3AD6-71E0-5E45-8CF6-F309BE23C1B5 \
    /tmp/x3dd/Build/Products/Debug-iphoneos/X3DashboardApp.app
  ```
- Op hardware zien: stijlvoorbeeld-knop indrukken, dan **korte druk op de aan-knop** van de X3 om 'm
  handmatig te laten slapen. Hij gaat er na een verse flash niet vanzelf in; hij boot in de gewone reader
  en slaapt pas na zijn eigen idle-timeout. Vraag altijd om die druk, wachten is verspilde tijd.

## Gereedschap en valkuilen uit deze sessie

- **`WebSearch` en `WebFetch` zijn stuk** in deze omgeving: `There's an issue with the selected model
  (deepseek-v4-flash)`. Het netwerk werkt prima. Gebruik `gh api "/search/repositories?q=..."` en `curl`
  in plaats daarvan. `gh search repos` gaf soms lege resultaten waar de API-vorm wél werkte.
- **zsh breekt een heel commando af** als één glob niet matcht. `ls /dev/cu.usbmodem* /dev/cu.wchusb*`
  meldde "geen poort" terwijl het apparaat er gewoon hing. Gebruik `ls /dev/cu.* | grep usbmodem`.
- **De USB-poort verdwijnt in deep sleep.** Een wachtlus die elke paar seconden kijkt en uploadt zodra de
  poort verschijnt, scheelt heen-en-weer.
- **Delegeren aan DeepSeek werkt goed** voor scherp afgebakend, mechanisch werk (wire-formaat aan twee
  kanten, generatiescripts, modelvelden) — zie `~/.claude/skills/delegate-to-deepseek/`. Maar **review is
  niet optioneel**: deze sessie maakte het de composer `throws`, wat de refresh-lus in propageerde en één
  onbekende stijlwaarde het hele dashboard zou laten bevriezen. Dat botst met een principe dat elders in
  dit project expliciet staat (`HomeAssistantClient.states` geeft juist partiële resultaten terug). Opgelost
  met drie lagen: `validate()` streng bij opslaan, `sanitized()` degraderend bij verversen, wire-encoder als
  achtervang.

## Werkwijze die de moeite waard is om aan te houden

- **Hardware vóór meer bouwen.** Deze sessie is bewust eerst een wegwerp-sampler gestuurd in plaats van
  de editor te bouwen. Dat leverde binnen één verzending drie bevindingen op — 24px-iconen onleesbaar, de
  tegel las als drie losse delen, de agenda oogde ongesorteerd — waarvan er twee tegen de verwachting in
  gingen. Zonder die stap was de editor om verkeerde aannames heen gebouwd.
- **Voorspellingen uit rekenwerk zijn niet gratis goed.** Er is deze sessie voorspeld dat icoon en
  waardecijfer elkaar exact zouden raken. Op het paneel was het tegenovergestelde waar: er stond juist een
  gat, omdat een `max()` het cijfer altijd op de middenlijn hield. De rekensom klopte alleen voor de tak
  die niet werd genomen.
- **Controleer de staat van een repo zelf** in plaats van een handoff te geloven. Deze sessie stond er
  onvastgelegd werk in de iOS-repo dat een vorige handoff "schoon" noemde; dat had bijna tot verkeerde
  conclusies over testaantallen geleid.
