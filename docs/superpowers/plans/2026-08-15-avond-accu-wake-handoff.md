# X3 wordt niet wakker op accu — Handoff

**Datum:** 2026-08-15 (avond)
**Voor:** wie dit morgen oppakt in een verse chat, zonder geheugen van deze sessie.
**Volgt op:** `2026-08-15-ios-editor-handoff.md` (de twee opdrachten daaruit zijn af).

**Repo's, beide gecommit en gepusht:**
- Firmware: `/Volumes/2TB/Development/Projects/CrossInk`, branch `feat/ble-handoff`.
- iPhone-app: `/Volumes/2TB/Development/Projects/xteink-x3-dashboard-ios`, branch `feat/native-ios-app`.

> **Let op:** er liep deze dag een tweede sessie in de CrossInk-repo die aan een *date-widget*
> werkt (`docs/superpowers/specs/2026-08-15-date-widget-design.md` en de bijbehorende plan-file,
> commits `09fe2058` en `18ec7cf7`). In de iOS-repo staan twee ongetrackte documenten over
> *WhatsApp message widgets* die ook niet uit deze sessie komen. Ga er niet vanuit dat elke
> wijziging in de working tree van jou is — controleer `git log` en `git status` zelf.

## Het openstaande probleem

**De X3 haalt geen dashboards op zolang hij niet aan de USB hangt.** Aan de kabel komt er elke
minuut een pakket binnen; los van de kabel nooit.

Wat al is uitgesloten, met bewijs:

- **De telefoon niet.** De Diagnose-pagina in de app toonde: pakket gebouwd, pakket-id 214, 5 tegels,
  verstuurpoging gestart, geen fout — en tegelijk "Geaccepteerd: nooit". De telefoon doet zijn werk.
- **De accu niet.** Getest met een volle accu (100%). Zelfde gedrag.
- **De slaapscherm-instelling niet.** `renderAgendaSleepScreen()` wordt alleen aangeroepen in de
  `SLEEP_SCREEN_MODE::AGENDA_SLEEP`-tak (`SleepActivity.cpp:514`), en het dashboard verschijnt op het
  slaapscherm. Die instelling staat dus goed en de cyclus wordt gewapend.
- **De battery-latch niet.** `HalPowerManager::startDeepSleep` laat latch-pinnen los bij het slapen,
  maar het X3-profiel laat `latch0`/`latch1` op `PIN_UNASSIGNED` (de X4 zet `{13, PIN_UNASSIGNED}`).
  Op de X3 is GPIO13 de SD-rail, niet de accu-MOSFET. Die lus doet daar niets.

### Waarom dit lastig te meten was

Serial over USB was het enige diagnosekanaal, en **USB is geen neutrale waarnemer**: met
USB-voeding boot het apparaat via `AfterUSBPower` en blijft het wakker in plaats van te slapen
(`main.cpp:914`, een `TEMP`-tak die er al stond). Elke wake die over serial te zien was, gebeurde
dus onder andere omstandigheden dan die ertoe doen. Alle tien geslaagde overdrachten die in het
serial-log geteld zijn, waren per definitie met kabel.

### Wat er nu klaarstaat om dat op te lossen

`src/spikes/ble_handoff/BleHandoffTrace.cpp` schrijft **één regel per reader-boot** naar
`/crossink-ble-trace.txt` op de SD-kaart:

```
2026-08-15 20:41 UTC wake=timer receiver=timedout
```

Een timer-wake gaat naar de receiver-partitie (die geen SD-toegang heeft) en reset terug naar de
reader; die terugkeer-boot schrijft de regel, met het oordeel van de receiver erbij. **Geflasht op
2026-08-15 20:39.**

**De test die morgen als eerste moet gebeuren:** X3 los van de USB, korte druk op de aan-knop, een
uur of een nacht laten liggen, dan de SD-kaart uitlezen.

| Wat er in het bestand staat | Conclusie |
|---|---|
| Geen nieuwe regels | Hij wordt niet wakker op accu — zoek in de slaap/timer, niet in BLE |
| `wake=timer receiver=timedout`, herhaald | Hij wordt wél wakker, receiver draait, maar telefoon en X3 vinden elkaar niet |
| `receiver=accepted` maar niets op het scherm | Pakket landt; probleem zit in opslaan of tekenen |

Controleer wel eerst dát er geschreven wordt (er hoort al een regel in te staan van de boot na de
flash). Een leeg bestand is alleen bewijs als schrijven aantoonbaar werkt.

### De sterkste hypothese die nog niet getoetst is

`main.cpp:898` zet de agenda-cyclus op `None` bij **elke** wake die niet van de timer komt — dus ook
bij een druk op de aan-knop. En `main.cpp:910` gaat bij een *afgewezen* aan-knop-wake terug slapen
met `startDeepSleep(gpio)`, **zonder wektimer**. Samen kan dat het apparaat achterlaten zonder
gewapende cyclus én zonder timer: dan slaapt hij door tot je hem handmatig wekt.

Dat past op het waargenomen gedrag, maar is **niet bewezen** — er is geen enkele
`Power-button wake: verifying`-regel in het serial-log terug te vinden. Een fix is bovendien niet
triviaal: `SETTINGS` is op dat punt in de boot nog niet beschikbaar (de SD is niet gemount, zie de
comment op `main.cpp:880`), dus het interval en de agendaSleep-vlag zouden uit een NVS-mirror moeten
komen.

## Wat deze sessie wél heeft opgeleverd

### Op hardware bevestigd

- **De stijl-editor werkt.** Tik op een tegel in het voorbeeld → inspector-sheet met type, icoon,
  grootte en nadruk. Tap en slepen leven samen via `.simultaneousGesture` naast de bestaande
  `.highPriorityGesture` — bevestigd op het toestel. De globale stijl staat op compositieniveau.
- **Iconen stonden een kwartslag gedraaid; opgelost.** `GfxRenderer::drawIcon` verwacht assets die
  *voorgedraaid* zijn opgeslagen (het leest `imgW` uit de hoogte), maar de gegenereerde
  Lucide-iconen zijn `freeink::Icon`: row-major, natuurlijke oriëntatie. Omdat de iconen vierkant
  zijn sloeg geen enkele controle aan. Nu via `drawPixel`, dat logische coördinaten neemt en de
  portrait-transform zelf toepast.
- **De tegel-layout was structureel fout.** `GfxRenderer::drawText` neemt de *bovenkant* van de
  tekst en telt de ascender er zelf bij op (`GfxRenderer.cpp:1090`); `renderKpiWidget` telde 'm er
  nog een keer bij. Op een tegel van 130px landde de waarde op 122..156 en het label op 138..159:
  overlappend en allebei buiten de tegel. Dat verklaarde in één klap drie klachten die los leken —
  het gat onder het icoon, waarde en label die tegen elkaar plakten, en labels buiten hun eigen
  grijze vlak. `renderListWidget` had dezelfde fout.

### Gebouwd, maar nog niet op het paneel gezien

- **Hoofdscherm opgeschoond.** Titel is "X3 Dashboard", "Gekozen afspraak" weg, "Klassieke kaart"
  vervangen, en alle statussignalen zitten achter een **Diagnose**-pagina.
- **`TEMPLATE_AGENDA` is van de telefoon verdwenen** (beslissing van de gebruiker: firmware-kant
  blijft voorlopig staan). Een telefoon zonder compositie stuurt nu `WidgetGridComposition.starter`
  — het hele raster als één agenda-tegel.
- **De verstrengeling met kalendertoegang is gerepareerd**, op alle drie de plekken: `refresh()`
  wiste `widgets` vóór de compositie überhaupt bekeken werd, en zowel `canSend` als `buildPackage()`
  keken naar `card != nil`. Een dashboard van stappen, batterij en Home Assistant kon niet verstuurd
  worden als kalendertoegang geweigerd was.
- **`TransferProtocol` weigerde pakketten onder 36 bytes** — het minimum van de agenda-template —
  terwijl een widget-grid zonder widgets een geldig pakket van 34 bytes is.
- **Diagnose-tijdstempels.** Laatst ververst, pakket gebouwd, pakket-id, aantal tegels, verstuurpoging
  en geaccepteerd. Dit heeft zich direct terugverdiend: het sloot de telefoon uit als oorzaak.
- **KPI-cache voor stappen.** HealthKit is versleuteld en onleesbaar zolang de telefoon vergrendeld
  is — precies de toestand tijdens een BLE-verzending in de achtergrond. Daardoor las de stappentegel
  één keer een echt getal en daarna steeds "—". Nu blijft de laatst bekende waarde staan, net zoals
  `HomeAssistantValueCache` dat al deed.

## Verificatie

- Firmware host-tests (61 slagen):
  ```bash
  export PATH="$HOME/.platformio/packages/tool-cmake/bin:$PATH"
  cmake --build /tmp/crossink-test-build --target BleHandoffRecordTest -j8
  /tmp/crossink-test-build/ble_handoff_record/BleHandoffRecordTest
  ```
- iOS (193 slagen): `swift test`. **Dekt het app-target niet**, draai er altijd bij:
  ```bash
  xcodebuild -project X3DashboardApp.xcodeproj -scheme X3DashboardApp \
    -destination 'generic/platform=iOS' build CODE_SIGNING_ALLOWED=NO
  ```
- Installeren:
  ```bash
  xcodebuild -project X3DashboardApp.xcodeproj -scheme X3DashboardApp \
    -destination 'id=837B3AD6-71E0-5E45-8CF6-F309BE23C1B5' -derivedDataPath /tmp/x3dd build
  xcrun devicectl device install app --device 837B3AD6-71E0-5E45-8CF6-F309BE23C1B5 \
    /tmp/x3dd/Build/Products/Debug-iphoneos/X3DashboardApp.app
  ```
  iOS herstart een app **niet** bij installeren. Vraag altijd om forceer-sluiten en opnieuw openen,
  anders beoordeel je de vorige build. Dat is deze sessie één keer misgegaan.

## Openstaand, bewust niet gedaan

- **De reader draait nog op env `fasttest`** (wake-interval 1 minuut in plaats van 15, uit het
  gitignored `platformio.local.ini`). Dat kost echt accu. Terugzetten met `-e default` zodra het
  accu-probleem opgelost is — nu nog niet, want 1 minuut geeft zestig meetpunten per uur.
- **`TEMPLATE_AGENDA` in de firmware.** Productie-oppervlak is klein: `encodePackage` heeft nul
  aanroepers buiten de tests, `decodePackage` precies één (`BleHandoffReaderProbe.cpp:29`), en de
  switch daar heeft al een veilige `default`. Weghalen kost wel een flash van beide partities.
- **De `TEMP`-tak op `main.cpp:914`** die het apparaat wakker houdt zodra er een kabel in zit. Stond
  er al; misleidend juist wanneer je aan het ontwikkelen bent.
- **`main.cpp:1031` logt "Accepted Agenda package"** voor wat een widget-grid is. Verouderde tekst.
- **Is Lexend 18/20 nodig?** Die vraag stond in de vorige handover als "de ladder oogt braaf" — maar
  dat oordeel is geveld over de kapotte layout met ~30px valse ruimte. **Opnieuw beoordelen** voordat
  iemand ~175 KB flash aan grotere fonts uitgeeft. Reader zit op ~570 KB vrij.
- **De agenda-widget krijgt een eigen layout-ronde.** Neem daarin mee: alleen tijden maakt afspraken
  van verschillende dagen ongesorteerd (13:00, 13:00, 08:00…), en er is geen "verouderd"-indicatie
  meer nu de oude kaart weg is — de widget-grid stuurt wel `validUntil` mee, maar geen tekst. De
  KPI-cache heeft hetzelfde probleem: een stappengetal van vanochtend ziet er identiek uit als een
  van net.

## Gereedschap en valkuilen

- **Stop de serial-capture vóór je flasht.** Een open poort geeft
  `device reports readiness to read but returned no data (device disconnected or multiple access on
  port?)`. Dit heeft deze sessie twee flashpogingen gekost.
- **Flashen lukt alleen met de kabel erin.** Zonder USB-voeding is de poort maar ~1 seconde per wake
  zichtbaar, veel te kort voor een upload van ~48 seconden. Met voeding boot hij via `AfterUSBPower`
  en blijft hij wakker.
- **Controleer de exit-status van `pio`, niet die van een pipe.** `pio run ... | tail` gaf exit 0 bij
  een mislukte upload, wat als succes gerapporteerd werd. Gebruik `setopt pipefail` of vermijd de
  pipe. Zie `/tmp/x3-flash-when-ready.sh`.
- **Begrens wachtlussen op wandkloktijd, niet op iteraties.** Een lus van 40 pogingen met `sleep 2`
  op een afwezige poort dekt ~80 seconden en doet één echte poging.
- **zsh breekt een heel commando af** als één glob niet matcht: `ls /dev/cu.* | grep usbmodem`.
- **`grep` struikelde over de CR's in het serial-log.** Gebruik `tr -d '\r'` of python.
- **Het scherm hertekent alleen bij een geaccepteerd pakket** (`main.cpp:895` → `1030`). Verloopt het
  receiver-venster, dan gaat hij direct terug slapen zonder te tekenen (`main.cpp:887`) en blijft het
  oude beeld staan — e-ink houdt zijn beeld vast. Een korte druk op de aan-knop hertekent wél, want
  die gaat via `SleepActivity`.

## Werkwijze die de moeite waard was

- **Hardware vóór meer bouwen, en dan de meting wantrouwen.** De editor is als vertical slice
  geïnstalleerd vóór er omheen gebouwd werd; dat leverde meteen twee bevindingen op. Maar de
  belangrijkste les zit een niveau dieper: het meetkanaal (USB-serial) veranderde zelf het gedrag dat
  onderzocht werd, en dat is een uur lang niet opgemerkt. De gebruiker die zelf de stekker eruit trok
  was de nuttigste zet van de avond.
- **Drie hypotheses sneuvelden bij nakijken** (slaapscherm-instelling, battery-latch, en de aanname
  dat elke slaap hertekent). Alle drie waren plausibel en alle drie fout. Controleer in de code
  vóórdat je iemand op een flash stuurt.
- **Instrumenteren slaat redeneren.** De vraag "stuurt de telefoon of weigert de X3" was met drie
  hypotheses niet te beslechten en met twee tijdstempels in één blik.
