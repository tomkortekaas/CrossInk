# Dashboard ververste niet — opgelost; accu-wake — opgelost

**Datum:** 2026-08-16 (ochtend geschreven, 's middags afgerond)
**Voor:** wie dit oppakt in een verse chat.
**Volgt op:** `2026-08-15-avond-accu-wake-handoff.md`. Lees die eerst, maar weet
dat de aanname bovenaan dat document niet klopte — zie "Wat er van gisteren
overeind blijft".

**Repo's:** sinds 2026-08-16 staat alles onder
`/Volumes/2TB/Development/Projects/X3/`: firmware in `X3/firmware` (branch
`feat/ble-handoff`), iPhone-app in `X3/ios` (branch `feat/native-ios-app`),
werkmap-notities en `sdk-patches/` in `X3/` zelf. De oude losse mappen
`CrossInk` en `xteink-x3-dashboard-ios` bestaan niet meer.

> **Uitkomst, 2026-08-16 14:05–14:49 lokaal.** Beide fouten zijn weg. De
> accu-wake is op hardware bevestigd: vierenveertig minuten op accu, 34
> timerwakes achter elkaar, 35 traceregels `reset=DEEPSLEEP`, nul
> `reset=POWERON`. Vóór de fix stonden drie tests op nul boots van welke soort
> dan ook. De oorzaak was GPIO13, die de accu-MOSFET stuurt en door het slaappad
> werd losgelaten.
>
> Wat een wake *kost* is nog niet gemeten. Zie "Nog open" onderaan.

## Wat er speelde

Het dashboard op de X3 ververste al veertien uur niet. Het bleek **twee losse
fouten die op elkaar leken**, en de ene verborg de andere.

## Fout 1 — opgelost: één onbekend widgettype keurt het hele pakket af

`validateWidget` in `src/spikes/ble_handoff/DashboardWidgetGrid.cpp` eindigt op
een kale `return Status::InvalidArgument` voor elk type dat de firmware niet
kent. Dat verwerpt **het complete raster**, niet alleen die tegel.

De gebruiker had een datumtegel in de compositie op de telefoon gezet. De
firmware op het toestel was de build van 2026-08-15 20:39 en kende
`WidgetType::Date` nog niet — die tak is pas om 22:53 gecommit. Dus:

1. Telefoon bouwt en verstuurt netjes, Diagnose-pagina meldt geen fout
2. X3 wordt wakker, ontvangt, en weigert élk pakket
3. E-ink houdt zijn beeld vast, dus het laatste geaccepteerde raster blijft staan

Symptoom: "hij ververst niet", niet te onderscheiden van "hij wordt niet wakker".
Een geweigerd pakket liet in die build geen enkel spoor na.

**Opgelost door te flashen.** De datumwidget-firmware stond wél gebouwd
(`firmware.bin` van 23:01) maar was nooit geüpload. Sinds de flash van vanochtend
09:36 wordt elk pakket geaccepteerd — vijftien op vijftien in de eerste
kwartier — en de datumtegel staat voor het eerst op het paneel, in vier
indelingen tegelijk.

**Les:** loopt de firmware achter op de app, verdenk dan eerst een nieuw
widgettype voordat je in slaap, timers of accu gaat zoeken.

## Fout 2 — opgelost: op accu ging hij uit in plaats van slapen

Drie onafhankelijke tests op 2026-08-16 (20, 5 en 10 minuten op accu, met een
wake-interval van 1 minuut uit env `fasttest`): **nul boots.** Van welke soort
dan ook. Aan USB draait dezelfde cyclus in diezelfde periode foutloos, elke
minuut, met `wake=timer` in de trace.

### Wat is uitgesloten, met bewijs

| Verdachte | Waarom afgevallen |
|---|---|
| De accu | 100%, en de aan-knop wekt hem op accu wél |
| ~~De accu-latch~~ | **Ten onrechte uitgesloten — dit bleek juist de oorzaak.** Zie hieronder |
| Het slaapscherm | `receiver=awaiting` in de trace bewijst dat de cyclus gewapend wordt |
| De telefoon | Vijftien foutloze pakketten op USB |
| Het pakketformaat | Fout 1, opgelost |
| De RTC-klokbron | `CONFIG_RTC_CLK_SRC_INT_RC=y` — interne RC, geen kristal dat kan wegblijven |
| Brownout | Zou geboot hebben en een trace-regel geschreven |
| Het meetinstrument | `SDCardManager::begin()` doet `gpio_hold_dis` en zet de SD-rail zelf aan, dus de trace werkt óók op accu |

### De oorzaak, gevonden in het schema

**Op accu slaapt hij niet, hij gáát uit — en USB verbergt dat.**

Uit het X4-schema (sunwoods/Xteink-X4, `readme-img/sch.jpg`; X3 en X4 delen het
bord): `IO13` voedt via schottky D2 een R6/R4-deler die Q7 opent, die op zijn
beurt de gate van Q1 (AO3401A P-FET) omlaag trekt en zo de cel op VIN zet.
USB-VBUS voedt datzelfde knooppunt parallel via D1.

**GPIO13 is dus de accu-latch, niet de SD-rail.** Het X3-profiel had hem als
`sd.powerEnable`, waardoor `powerDownRailsForSleep()` hem elke slaapbeurt laag
trok en `esp_sleep_config_gpio_isolate()` hem daarna liet zweven. R4 trekt het
knooppunt leeg, Q1 sluit, toestel uit. Aan de kabel dekt D1 dat volledig af — de
chip blijft onder spanning en de RTC-timer vuurt gewoon.

De commentaarregel in het X3-profiel die GPIO13 als SD-rail beschreef, klopt dus
niet. Die is in dit onderzoek een tijdlang voor waar aangenomen en heeft de
accu-latch ten onrechte als verdachte laten afvallen.

Onafhankelijke steun: de X4-voorbeeldfirmware van CidVonHighwind slaapt zónder
GPIO13 aan te raken en overleeft op accu, en upstream issue #2951 beschrijft
"X3 in slaap, 's ochtends niet te starten, pas na even aan de lader".

### De fix

Zit in `X3/firmware` op `feat/ble-handoff`. **Op hardware bevestigd.**

- X3-profiel krijgt `power.latch0 = 13` naast `sd.powerEnable = 13`, met drie
  `static_assert`s die bewaken dat die twee rollen niet uit elkaar lopen
- `powerDownRailsForSleep()` slaat latch-pinnen over — display, touch en mic
  worden nog gewoon afgeknepen, alleen de systeemrail blijft met rust
- `startDeepSleep()` laat de latch alleen los als er **géén** wektimer staat; met
  een timer wordt hij juist hoog vastgehouden ná de `isolate`. De release-lus
  doet nu eerst `gpio_hold_dis()`, anders blokkeert een vorige timer-slaap de
  daaropvolgende power-off stilletjes

Let op: drie van die bestanden zitten in de **submodule** `freeink-sdk`
(`BoardConfig.h`, `PowerManager.h`, `PowerManager.cpp`). Daar bestaat geen fork
van, dus ze kunnen nergens gecommit worden en staan alleen als patch in
`X3/sdk-patches/crossink-freeink-sdk-battery-latch.patch`. Een `git checkout` of
`git submodule update` in `firmware/freeink-sdk` wist ze zonder waarschuwing.

Let ook op: voor de X4 is loslaten juist bedoeld gedrag — daar is deep sleep de
software-uitschakeling. De semantiek verschilt per bord; gooi de gedeelde
slaaproutine niet om.

### De meting die het bevestigd heeft

Env `fasttest` (wake-interval 1 minuut), kaart in het toestel, kabel eruit,
44 minuten laten liggen. Daarna terug naar Home en de trace over USB uitlezen.

| In de trace | Conclusie |
|---|---|
| 34× `wake=timer` | De timer vuurde, minuut na minuut |
| 35× `reset=DEEPSLEEP` | Elke boot kwam echt uit deep sleep |
| 0× `reset=POWERON` | De rail is geen enkele keer weggevallen |

Een enkele geslaagde verversing bewijst dit niet: die had ook van een toestel
kunnen komen dat gewoon wakker bleef. Het is `reset=DEEPSLEEP` dat die twee
scheidt, want die resetreden krijg je alleen als de powerdomeinen uit zijn
geweest.

**Klokval:** het toestel klokt op UTC, lokaal is UTC+2. Een traceregel van
`12:05` hoort bij 14:05 op de muur. Dat verschil heeft tijdens dit onderzoek een
keer tot de verkeerde conclusie geleid ("die run schreef niets weg"). Het
wekvenster is wél lokaal — `resolveAgendaWakeLocalTime` past `clockUtcOffsetQ`
toe — dus 22:00–07:00 bij de gebruiker verschijnt in de trace als 20:00–05:00.

## Ook bewezen: de afgekeurde knopdruk-wake bestaat

De hypothese onderaan de handoff van gisteren is geen hypothese meer. In de
seriële log:

```
--- port opened 10:13:25 ---
[286] [INF] [BOOT] Wake route: PowerButton
--- port lost 10:13:27 (sleep?) ---
```

Twee seconden: booten, en via `main.cpp:910` terug de slaap in met
`startDeepSleep(gpio)` — **zonder wektimer**, en met de agenda-cyclus een paar
regels eerder al op `None` gezet. Daarna ligt het toestel stil tot iemand het met
de hand wekt.

Dat dit gebeurt is nu aangetoond. Dat het *spontaan* gebeurt nog niet: de twee
waargenomen gevallen waren echte drukken tijdens een test. Sinds vanochtend is
het herkenbaar in de trace als `stage=rejected`.

## Het meetinstrument

`appendBootTrace` stond op `main.cpp:961`, ná de SD-mount. De drie boots die er
het meest toe doen springen daar ruim vóór uit en lieten dus geen spoor na:

- `main.cpp:933` — timerwake schakelt naar de receiver en doet `ESP.restart()`
- `main.cpp:888` — de terugkeer met `TimedOut` gaat direct de slaap in
- `main.cpp:910` — knopdruk-wake die zijn hold-check niet haalt

Gevolg: een leeg bestand na een nacht op accu was niet te onderscheiden van
"wordt elk kwartier wakker en vindt niemand thuis". Precies de misleiding waar de
handoff van gisteren voor waarschuwde, ingebakken in het instrument zelf.

**Nu:** `appendEarlyBootTrace` mount de kaart zelf en wordt op alle drie die
plekken aangeroepen. De regel draagt twee extra kolommen:

```
2026-08-16 08:31 UTC wake=timer receiver=awaiting stage=handoff reset=DEEPSLEEP
```

- `stage=` — `full`, `handoff`, `timedout`, `rejected`
- `reset=` — de resetreden, genoemd door `resetReasonName()` in `main.cpp`

Kosten: een SD-mount van een paar honderd milliseconde op een wake waarvan het
radiovenster twintig seconden is. Verwaarloosbaar.

## Wat er van gisteren overeind blijft

De aanname bovenaan de vorige handoff — *"De X3 haalt geen dashboards op zolang
hij niet aan de USB hangt"* — was verkeerd geformuleerd. Dat de tien geslaagde
overdrachten allemaal met kabel waren, bewees niet dat het aan de kabel lág: ze
waren allemaal van vóór de datumtegel.

Maar de conclusie zelf klopte toevallig wél, en is nu met beter gereedschap
bevestigd. Er waren twee fouten, en beide gaven hetzelfde beeld.

## Gereedschap en valkuilen

- **Een buildtijd is geen flashtijd.** `firmware.bin` van 23:01 zei niets over
  wat er op het toestel stond; de laatste geslaagde upload was van 20:39.
- **`pio run ... | tail` geeft exit 0 bij een mislukte upload.** Controleer de
  exit-status van `pio` zelf. Stop een seriële capture vóór je flasht.
- **USB-CDC mist de eerste ~1,1 seconde van elke boot.** Daarin staan
  `Wake route:`, `BLEPAY` en de receiver-uitslag. Wat je overhoudt is de staart
  van de versieregel (`dev`). Gebruik de SD-trace, niet serial, voor boot-vragen.
- **`pio device monitor` wil een tty.** Gebruik een herverbindende capture; de
  poort verdwijnt bij elke deep sleep. Script: `/tmp/x3-serial-log-2.py`,
  pyserial zit in `~/.platformio/penv`.
- **zsh breekt een heel commando af** als één glob niet matcht.
- **De SD-kaart in een computer steken liegt.** De Mac liet 71 traceregels zien
  terwijl er 197 op het toestel stonden, en die 71 waren exact het begin van de
  197. SdFat werkt bij het aanhangen de datacluster bij maar niet de
  directory-entry, dus een host leest de grootte van de laatste sync. Een
  "onveranderde" tracefile op de Mac bewijst dus niets.
- **Lees de trace over USB-serieel.** `UsbSerialFileTransfer.cpp` kent een
  binair protocol: `b"CMND" + b"T" + uint16le(len(pad)) + pad`, terug komt
  `b"READY\n" + uint32le(grootte) + data + uint32le(crc32)`. Script:
  `/tmp/x3-read-file.py`. Twee voorwaarden: de seriële capture moet uit (één
  open poort tegelijk) en de X3 moet op het **Home-scherm** staan — `main.cpp`
  geeft `isHomeActivity()` mee als toestemming, anders komt `ERR:not_on_home`.
  Er is geen commando dat van scherm wisselt. Geen wifi nodig, kaart hoeft er
  niet uit.
- **Zonder SD-kaart in het toestel schrijft een accutest niets weg.** Eén keer
  gebeurd: de kaart lag in de Mac terwijl de test liep.
- **Er zijn twee X3-firmwarelijnen geweest.** Er is een keer uit de
  probe-repo geflasht terwijl de agenda-firmware draaide; het paneel toonde
  daarna een boekomslag en dat leek op een oude build, maar was precies
  andersom. De probe-lijn is nu gearchiveerd.

## Staat van de working tree

Alles gecommit op `feat/ble-handoff`, behalve de submodule — zie "De fix".

De reader draait op env `default` (wake-interval 15 minuten). `fasttest` uit het
gitignored `platformio.local.ini` zet dat op 1 minuut; dat is prima voor een
wake-test maar kost echt accu, want hij is dan grofweg de helft van de tijd
wakker. Altijd terugzetten na een meting.

## Verificatie

```bash
export PATH="$HOME/.platformio/packages/tool-cmake/bin:$PATH"
cmake --build /tmp/crossink-test-build --target BleHandoffRecordTest -j8
/tmp/crossink-test-build/ble_handoff_record/BleHandoffRecordTest   # 73 slagen

~/.platformio/penv/bin/pio run -e default
~/.platformio/penv/bin/pio run -e default -t upload --upload-port /dev/cu.usbmodem31401
echo $?   # de exit-status van pio zelf, niet die van een pipe
```

Trace uitlezen zonder de kaart eruit te halen (X3 op Home, capture gestopt):

```bash
~/.platformio/penv/bin/python /tmp/x3-read-file.py /crossink-ble-trace.txt /tmp/trace.txt
```

## Nog open: wat een wake kost

De wake wérkt nu, maar niemand weet wat hij kost. Drie dingen die dat bepalen.

**De ruststroom is nooit gemeten.** De laadtoestand die de gauge geeft is in hele
procenten, en één procent is hier ruwweg 15 mAh — grover dan een hele nacht
standby, dus voor en na lezen hetzelfde getal. Daarom draagt de traceregel nu
`mv=` en `mah=`, rechtstreeks van de BQ27220 (registers `0x08` en `0x10`). Dat
laatste is een coulombteller die ook tijdens deep sleep doortelt, dus het
verschil tussen twee wakes is wat dat interval echt gekost heeft. Het wekvenster
sluit om 22:00 lokaal en gaat om 07:00 weer open; in die negen uur is er per
definitie geen enkele wake, dus het verschil tussen de laatste avondregel en de
eerste ochtendregel is **zuivere ruststroom**. Die meting staat nog uit.

**De accu is kleiner dan gedacht.** Eerste uitlezing aan de lader: `mah=635`.
Het pakket is dus eerder ~650–700 mAh dan de 1500 waar eerdere schattingen van
uitgingen; alle "hoeveel dagen"-getallen van vóór deze regel zijn ruwweg twee
keer te optimistisch. `mv=4353` ligt boven de 4,2 V die een cel normaal doet —
gemeten aan de lader, dus mogelijk de laadspanning, maar het kan ook betekenen
dat register `0x08` verkeerd geschaald wordt. De eerste meting op accu wijst dat
uit.

**Een knopdruk in de broekzak is de duurste post die we gezien hebben.** Elke
knopdrukwake parkeert hem op Home, en `sleepTimeoutMinutes` staat standaard op
10. In de trace zichtbaar: na de knopdrukwake van 11:55 komt de eerstvolgende
timerwake pas om 12:05, terwijl het interval toen één minuut was. Tien minuten
wakker is meer dan een hele dag kwartierwakes bij elkaar. Twee knoppen die dat
temmen, allebei instellingen op het toestel: de slaaptimeout omlaag, en de korte
druk op de aan-knop op iets anders dan *Slaap* zetten — dan is 200 ms nodig om
te wekken in plaats van 10 ms (`getPowerButtonWakeDuration()` in
`CrossPointSettings.h`). Nog niet gedaan.

Wat wél al vaststaat over de kosten: per cyclus is hij ruwweg 25 à 30 seconden
wakker. Dat zijn twee echte boots — de timerwake doet de handoff en roept
`ESP.restart()` — plus een luistervenster van `RECEIVER_WINDOW_MS = 20000`.
Bij een interval van 15 minuten is dat ongeveer 3% van de tijd, bij `fasttest`
de helft.

## Nog open: interval instelbaar vanuit de app

Nu de wake werkt is de vraag of de telefoon het interval en het wekvenster mag
zetten in plaats van de compile-time `CROSSINK_AGENDA_WAKE_INTERVAL_MINUTES`
(standaard 15) en de hardgecodeerde `AGENDA_WAKE_WINDOW_START/END_MINUTE`
(07:00–22:00) in `AgendaWakePolicy.cpp`.

Kan, en het meeste ligt er al. Drie dingen om te weten:

- **Het moet persistent zijn.** Een timerwake is een chipreset, dus RAM is leeg.
  `BleHandoffNvs` bewaart het laatst geaccepteerde pakket al in NVS; daar hoort
  dit bij.
- **Het is een schemawijziging, en dat is de gevaarlijke helft.**
  `PackageHeader` draagt een `schema`-byte die bij binnenkomst gevalideerd wordt.
  Stuurt de app V2 naar firmware die alleen V1 kent, dan wordt het pakket
  geweigerd — precies de val van fout 1 hierboven. Firmware eerst flashen, of de
  firmware een onbekend veld laten negeren in plaats van af te keuren.
- **Het venster is de grotere accuknop dan de frequentie.** Negen uur per nacht
  niet wakker worden scheelt meer dan de keuze tussen een kwartier en een half
  uur overdag.

## Nog open, los van de accu

- **De dagovergang van de datumtegel.** Na 07:00 moet hij vanzelf omslaan zonder
  dat er iets verstuurd wordt. Nooit waargenomen.
- **Iconen lijken één positie verschoven** op het paneel: stappen droeg een
  zonnetje, batterij droeg voetstappen. Kan handmatige configuratie zijn.
- **De agenda-widget sorteert niet** over dagen heen: 13:00, 14:30, 08:00, 08:00.
  Stond al in de vorige handoff.
- **`main.cpp:1031` logt "Accepted Agenda package"** voor wat een widget-grid is.
