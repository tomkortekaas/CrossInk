# Praktijktest GPX-routebasis — nog niet uitgevoerd

Dit checkpoint test GPX-overdracht en lokale routegeometrie. Het is **geen complete wandel-navigatie**: live GPS, achtergrondkaart met wandelpaden, automatische manoeuvres en zuinige slaap/partial-refresh zijn nog aparte bouwblokken. Een routelijn zonder omliggende wegen is op dit moment dus te verwachten. Gebruik dit nog niet als enige navigatiemiddel.

## Voorbereiding, pas wanneer de gebruiker kan testen

- Bewaar de bestaande SD-inhoud en de huidige werkende firmware. Verander geen partitie-indeling.
- Installeer uitsluitend de bij dit checkpoint gecontroleerde navigator-app in **app0, offset 0x10000**. Geen merged image, bootloader, partitietabel, SPIFFS of erase-flash. Reader/dashboard blijft in app1.
- Gebruik de passende iPhone-build met GPX-import en route-overdracht, niet alleen de oudere knop die het voorbeeldscherm startte.
- Zorg voor een leesbare SD-kaart, een opgeladen X3 en een kleine bekende GPX-wandeling. Later herhalen met normale 10–15 km en uitzonderlijke 40 km routes.
- Dit testprotocol is geen toestemming voor automatisch flashen; in dit checkpoint is niets geflasht.
- `fixtures/x3-route-smoke.gpx` is een synthetisch bestand met twee segmenten voor de eerste bureautest. Het is nadrukkelijk geen buiten te volgen wandeling; de onderbreking tussen de twee segmenten moet zichtbaar blijven.

## Functionele controle

1. Kies een GPX-bestand op de iPhone. Controleer naam, afstand en geschatte tijd. De GPX-lijn blijft leidend; er mag geen aansluiting tussen afzonderlijke tracksegmenten worden verzonnen.
2. Verstuur naar X3. Als de reader/dashboard actief is, wacht de iPhone op het bestaande ontvangstvenster; de navigatiecode wijzigt de kwartierplanning niet. Gebruik eventueel de al werkende handmatige ontvangstactie van jullie firmware.
3. Na de overgang naar navigator moet de iPhone met **dezelfde X3** verbinden. Een dashboardstatus of een apparaatnaam alleen geldt niet als bewijs dat navigator klaarstaat.
4. Tijdens de pakketoverdracht mag het X3-scherm niet bij ieder blok knipperen. Na volledige validatie/opslag verschijnt het routeoverzicht in portretstand.
5. Het scherm meldt dat GPS nog niet actief is. Er verschijnt geen gesimuleerde positie of verzonnen volgende afslag. De iPhone mag pas succes melden na de ontvangstbevestiging van COMMIT.
6. Terug op X3 gaat naar reader/dashboard. Start navigator later opnieuw: dezelfde route moet zonder nieuwe download vanaf SD verschijnen.
7. Na een gesloten ontvangstvenster opent OK een nieuw venster. Controleer opnieuw verzenden wanneer navigator al actief is; als dit nog niet automatisch kan, noteer het als fout, niet als succesvolle herhaaltest.

## Betrouwbaarheid

- Verbreek de verbinding halverwege een vervangende route: de vorige geldige route moet blijven bestaan. Opnieuw verbinden mag geen halve nieuwe route tonen.
- Herhaal overdracht met dezelfde route en daarna met een andere route.
- Herstart nadat een route volledig is geaccepteerd: controleer route-id/naam/geometrie, niet alleen de succesmelding.
- Start zonder SD: heldere foutmelding, geen voorbeeldkaart als echte route. Plaats de kaart terug en probeer opnieuw; de SDK kan fysieke kaartverwijdering niet betrouwbaar uit alleen zijn ready-vlag afleiden.
- Test kaartverwijdering en stroomonderbreking alleen met een aparte testkaart na backup. FAT biedt geen bewezen power-loss-atomiciteit; verlies van zowel bin als backup blijft een hardware-/bestandssysteemrisico.
- Geen wijzigingen aan boeken, readerinstellingen, dashboarddata of andere SD-mappen. Routebestanden horen alleen onder `/Navigation/Routes/active/`.

## Metingen om vast te leggen

- Tijd van navigator-start tot BLE-connectie, en van START tot geaccepteerde route.
- Aantal zichtbare volledige schermverversingen tijdens een overdracht.
- Portretoriëntatie, leesbaarheid routelijn/cijfers, en segmentonderbrekingen.
- Boot- en COMMIT-logregels: vrije heap, kleinste vrije heap, grootste vrije blok en task-stack high-water mark. Linker-RAM is geen runtime-meting.
- Gedrag na drie minuten zonder verkeer en na geaccepteerde route. Advertenties/link worden beëindigd; de BLE-stack en hoofdloop blijven voorlopig aanwezig. **Dit is nog geen energiezuinige pocketmodus.**
- Ga na de proef met Terug naar de reader/dashboard; laat deze ontwikkelbuild niet onnodig uren in navigator staan.
- BLE is in deze ontwikkelbasis niet geauthenticeerd/gepaard. Een beperkte ontvangsttijd en apparaatselectie zijn geen beveiligingsgarantie; test in een vertrouwde omgeving. Pairing/bezitscontrole vereist een aparte beslissing vóór regulier gebruik.

## Wat softwaretests niet bewijzen

Hosttests controleren formaat, transacties, fouten, schermbuffer en protocolvolgorde. De simulator kan de interface starten, maar bewijst geen CoreBluetooth-herverbinding op een echte iPhone. Werkelijke SD-fouten, BLE-timing rond reboot, e-ink-ghosting, batterijtijd en slaap/wake moeten op de apparaten worden gemeten.
