# Library op de daadwerkelijk geïnstalleerde X3-basis

De eerste port in `CrossInk-library-x3` gebruikte een oudere checkout (`65636b43`, v1.5.0 plus eigen commits). Die binary is gemarkeerd als **DO NOT FLASH** en is niet op de X3 geschreven.

## Gecontroleerde actuele basis

Op 5 oktober 2026 is vóór het flashen de volledige 16 MB flash uitgelezen. Actief app0 op `0x10000` bevatte 6.150.448 bytes met SHA-256 `e80e89abc3f7be4d459f9c069bb20328e180ce65ffa932a2044314b2de78f30f`. Dit matcht exact de eerder geïnstalleerde **CrossInk v1.6.0-candidate van 27 september** uit broncommit `af65f5ddcd6fd2d40c4c32e67965ecba0ad75a9f`. De oude versie-string in de firmware verwees nog naar `e2563607`; die string alleen was dus onvoldoende bewijs.

De gecorrigeerde port gebruikt `/Volumes/2TB/Development/Projects/CrossInk-library-x3-current`, branch `feat/x3-current-library-v1.6.1`, vanaf `0e42b1af8ce6deb666f77ab35b92fc73c7eeec71` (dezelfde productiebron plus documentatie). Backupbranch: `backup-before-library-merge`. De SDK-basis is de actuele aangepaste `a9ab1ccd61b52984413c05543873f37651dd8835`, met alleen FreeInkUI bijgewerkt voor de Library.

De bestaande volledige v1.6.0-upgrade blijft behouden. Daarop zijn uitsluitend de officiële Library/Cover Grid en benodigde afhankelijkheden uit v1.6.1 geport. Dit is geen volledige v1.6.1-merge. Versie: `1.6.0-library-v1.6.1`.

## Behoud en oplossingen

`src/main.cpp`, `src/spikes/`, bestaande sleepactiviteiten, GPIO/power-HAL en alle SDK-code buiten FreeInkUI zijn byte-identiek aan de actuele basis. Daardoor blijven de nieuwste Agenda-leesbaarheid, dag/nacht-wakebeleid, batterijverbeteringen, navigator, BLE-teardown en SD-shutdown behouden. Ook de v1.6.0-reader-, Dark Mode-, Quick Actions- en instellingenfuncties blijven behouden.

De oudere port is met een gerichte three-way patch overgezet. Overlap zat hoofdzakelijk in vertalingen en bestaande v1.6.0-API’s. Bestaande vertalingen zijn behouden, alleen ontbrekende Library-keys toegevoegd. Bestaande renderer-, filesystem- en gedeelde-themafixes uit v1.6.0 zijn behouden. Home behoudt Quick Actions en de nieuwere selectie van het laatst gelezen boek; Cover Grid is een optionele extra branch. OptionPopup behoudt de volledige v1.6.0-bevestigings-, swipe-, uitschakel- en touchfunctionaliteit en krijgt alleen een Library-menuverdeler. De instellingenreserve is van 102 naar 104 vergroot voor twee nieuwe instellingen, zodat de vector niet onnodig verdubbelt in RAM.

De volledige Library-index kan covers in dezelfde officiële grid tonen, ook buiten Recent geopend. Maximaal negen covers per pagina worden een voor een gegenereerd en als BMP op SD hergebruikt. De RAM HomeCoverCache is uitgeschakeld op de X3 zonder PSRAM. Indexsortering gebruikt begrensde buffers, inclusief spill naar SD; een grote index groeit niet als een volledige boekenlijst in RAM. De leesvoortgang/cacheformaten blijven behouden en herstel na mislukte cache-restore is getest.

Geen CrumBLE, factory reset of SD-wisactie. Hardwareflash schrijft alleen app0; de bestaande app1-rollback, bootloader, partitietabel, OTA-data, NVS en filesystem blijven behouden.

## Gebruik

Behoud Dashboard. De primaire browseknop heet nu Library; File Browser staat in het Home-menu. De instelling **Bibliotheek/bestandsbrowser wisselen** stuurt dit voor Dashboard/Minimal.

Library → Menu → Bibliotheekinstellingen → **Boekenkastweergave: Grid**. Een opgeslagen lijstvoorkeur blijft behouden, dus zet dit zo nodig eenmalig om. Kies **Titel** voor alle boeken; **Recent geopend** toont alleen recente boeken. Up/Down selecteert, Select opent en Back keert terug (een actieve zoekopdracht wordt eerst gewist).

Optioneel: UI-thema **Cover Grid**. Dat homescreen toont recente covers; het boeken-icoon opent de volledige Library. Dashboard blijft beschikbaar.

## Controle

- Dashboard-X3 bouwt; 6.240.928 bytes, 312.672 bytes vrij in apppartitie van 6.553.600 bytes.
- Statisch RAM: 91.032/327.680 bytes; dit is geen meting van de vrije runtimeheap.
- Alle hosttestprogramma’s bouwen. Gerichte selectie: 633 geslaagd, 2 overgeslagen.
- Volledige suite: 1.352 geslaagd, 3 overgeslagen en de 8 bekende niet-Engelse hyphenation-evaluatiefouten uit de v1.6.0-basis. Geen nieuwe fouten in de geteste port.
- X3-simulator: Library, volledige index-grid, ontbrekende/beschadigde covers, boek buiten Recent geopend, fysieke Select/Back, Dashboard, lege bibliotheek en brede bestaande reader/settings/sleep-smoketest geslaagd.
- Simulator gebruikt uitsluitend afzonderlijke synthetische SD-fixtures. Native adaptaties zijn alleen simulatorcode.
- Normale X3-build en fysieke installatie worden aanvullend in `installation.json` vastgelegd.

De fysieke framebuffer is 792×528, met bestaande portraitoriëntatie 528×792. Echte e-ink-weergave, BLE, batterijgedrag en langdurige heapstabiliteit blijven na installatie praktisch te controleren; compilatie en simulator zijn geen hardware-soaktest.

## Bewijs en herstel

Firmware en logs: `/Volumes/2TB/X3/builds/library-v1.6.1-current-2026-10-05/`.

Aanbevolen binary: `x3-dashboard-library-v1.6.1.bin`. `source-proof.json` legt de fysieke bronidentificatie en candidatehash vast. `installation.json` legt write/readback/boot vast. De lijst [changed-files.txt](changed-files.txt) bevat de gewijzigde bronbestanden, inclusief SDK.

Volledige preflashbackup: `/Volumes/2TB/x3-flash-backups/2026-10-05-library/preflash-full.bin`, SHA-256 `b033a6816f8f9f1c5bdffc3922e2e6e7de742308c26effe580f56941fbb9a4c9`. De backup is gemaakt vóór enige firmwarewrite. De bestaande broncheckout en zijn lokale documentatiewijziging zijn ongemoeid gelaten. Firmware en SDK worden gepubliceerd op respectievelijk `feat/x3-current-library-v1.6.1` en `feat/x3-current-library-ui` in de tomkortekaas-forks. Zie [het bronselectieonderzoek](build-selection-investigation.md).
