# Onderzoek verkeerde bronbasis — 5 oktober 2026

## Oorzaak

De eerste Library-port gebruikte de checkout met HEAD `65636b43`, via de bestaande verwijzing `X3-actueel/firmware`. Die verwijzing werd ten onrechte gelijkgesteld aan de firmware op het apparaat. Het was een fout in mijn bronselectie en verificatie: de geïnstalleerde binary had vóór het kiezen van de mergebasis geïdentificeerd moeten worden. Een succesvolle build of test bewijst alleen de gekozen bron, niet dat die bron actueel is voor het apparaat.

## Direct gecontroleerde aanwijzingen

- Op 5 oktober toont `ls -ld /Volumes/2TB/Development/Projects/X3-actueel/firmware` nog de symlink naar `/Users/tomkortekaas/Development/worktrees/CrossInk/dashboard-v3-firmware`; de linkdatum is 31 augustus. De centrale X3 README noemt de verwijzingen een actief startpunt, maar dat garandeert geen actuele installatie-identiteit.
- Het verslag van de eerste port, `/Volumes/2TB/Development/Projects/CrossInk-library-x3/docs/library-v1.6.1/README.md:7`, identificeert `65636b43`; regel 8 noemt de officiële v1.5.0-basis. Dat verslag beschrijft daadwerkelijk de oude checkout.
- [source-proof.json](source-proof.json) legt de fysieke installatiehash vast: `e80e89abc3f7be4d459f9c069bb20328e180ce65ffa932a2044314b2de78f30f`. Deze matcht exact de v1.6.0-candidate uit [het eerdere buildverslag](../deployments/2026-09-27-x3-crossink-v1.6.0-build.md), regels 65–67.
- [README.md](README.md), regel 7, documenteert dat de bestaande firmware nog een oude versie-string met `e2563607` bevatte. De tekstuele versienaam was daarom geen betrouwbaar bewijs van de productiebron. De nieuwe bootlog meldt bovendien `dev`; voor de huidige installatie geldt eveneens de binaryhash als identiteit.
- [Het installatieverslag](../deployments/2026-10-05-x3-library-v1.6.1.md) documenteert de gecorrigeerde build, succesvolle boot en exacte readback. De oude candidate was al vóór het schrijven vervangen en is niet geflasht.

## Waarom de fout niet meteen opviel

Er waren meerdere checkouts en historische verwijzingen met namen als actueel. Ik controleerde de Git-basis van de gevonden checkout, maar had nog geen bewijs dat die overeenkwam met de nieuwste deployment. Mijn eerste antwoord over v1.5 was daarom te stellig: het beschreef de checkout, niet jouw apparaat. Jouw vraag over een oude build leidde tot de fysieke controle die de fout aantoonde.

## Correctie en vaste werkwijze

De Library is opnieuw geïntegreerd op de bewezen volledige v1.6.0-bron en de bijbehorende aangepaste SDK. De verouderde candidate heeft een DO-NOT-FLASH-markering. De huidige firmware en SDK krijgen eigen gepubliceerde branches; de submodule-URL verwijst naar de fork waar de gepinde SDK-commit beschikbaar is.

Voor vervolgwerk eerst de laatste deployment, broncommit, SDK-commit en artifacthash vergelijken. Bij onduidelijkheid pas na autorisatie een fysieke flashbackup uitlezen en de actieve apphash vergelijken. Een mapnaam, Git describe of runtime-versiestring alleen is onvoldoende. Voor elke flash de candidatehash en actieve OTA-slot controleren, alleen de afgesproken app schrijven en teruglezen. Geen historische symlinks verplaatsen: die kunnen nog door andere hulpmiddelen worden gebruikt.

Dit onderzoek wijzigt geen firmwaregedrag en vergroot de covers niet. De vraag om grotere covers was nog niet goedgekeurd als wijziging.
