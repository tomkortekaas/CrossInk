# Handmatige ontvangst vanaf hoofdscherm — 4 september 2026

## Gedrag

Dashboard-X3 voegt Ontvangen toe als eerste menuactie op het hoofdscherm van Lyra/Lyra 3 Covers. Minimal en Dashboard tonen de actie in hun bestaande Menu-paneel. De carrouselcache heeft een aparte versie voor de extra actie. Niet-BLE-builds tonen de actie niet.

De reader heeft zijn Bluetooth-geheugen al vrijgegeven. Daarom schildert de actie met de bestaande framebuffer een wachtscherm en vraagt via een eenmalig RTC-woord een softwareherstart aan. Vóór de gewone readerinitialisatie opent de nieuwe boot één venster van maximaal 60 seconden. Alleen de ingestelde Terug-knop wordt meegenomen. Het verzoek wordt vóór alle hardwarewerk gewist en wordt bij koude boot/panic/andere resetredenen geweigerd.

Terug, timeout en succesvolle dashboardoverdracht eindigen in een normale herstart naar Home. Een compleet dashboard krijgt zijn bestaande bevestiging; een gedeeltelijke overdracht wordt niet opgeslagen. De ontvangstboot deinitialiseert geen levende BLE-server: de herstart ruimt de stack op. Een navigatieverzoek volgt de bestaande marker/app0-overgang. Agenda-timervensters blijven de bestaande route gebruiken. De niet hardware-gevalideerde power-hold ingang is alleen met CROSSINK_EXPERIMENTAL_WAKE_HOLD beschikbaar; deze build zet dat niet aan.

## Praktijktest na gerichte installatie

1. Open Ontvangen op het X3-hoofdscherm. Controleer zichtbaar Wachten op iPhone, 60 seconden en Terug: sluiten.
2. Druk Terug zonder telefoonoverdracht. Home moet terugkomen; de annuleerknop mag geen boek openen.
3. Open opnieuw en raak niets aan: na circa 60 seconden plus boottijd moet Home terugkomen. Herhaal eenmaal.
4. Zet een dashboard klaar op de iPhone, open Ontvangen en controleer ontvangstbevestiging. Ga daarna naar agendaslaap en controleer de nieuwe kaart.
5. Start alleen de navigator vanaf de iPhone terwijl Ontvangen openstaat; controleer de opgeslagen route en kaart. Terug moet naar reader leiden.
6. Na afloop agendaslaap en één normale geplande dashboardrefresh bevestigen.

Geen claims over bovenstaande hardwarestappen tot ze werkelijk zijn uitgevoerd. De eerdere vastloper na de readerwissel had geen vastgestelde oorzaak; de rollback en de kaartweergave werkten daarna. Back-up van de werkende reader staat onder /Volumes/2TB/x3-hardware-test-20260904/app1-reader-backup.bin. Geen bootloader, partities, NVS of SD-opslag overschrijven bij installatie.

## Samenwerking

DeepSeek onderzocht de menu-inpassing alleen-lezen en signaleerde capaciteit, selectie en carrouselcache. Codex implementeerde en beoordeelde de radio/boot-integratie en menu-aanpassing. Er is geen tweede schrijver in de werkmap gebruikt.
