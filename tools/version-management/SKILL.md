---
name: version-management
description: Controleer bron-, build- en installatie-identiteit bij ontwikkelen, committen, pushen en afronden van Toms apps en firmware, inclusief actieve bronverwijzingen.
---

# Versiebeheer voor apps en firmware

Gebruik deze controle naast bestaande commit- en afrondskills. Een groene build bewijst niet dat de juiste bronbasis is gekozen.

## Bij starten

Zoek de projectregistratie `deployment-state.json` en de laatste deployment. Leg repositorypad, branch, broncommit, dependency/submodulecommits, buildprofiel, artifacthash en installatiestatus naast de gekozen checkout. Gebruik mapnamen, Git describe of schermversies nooit als enig bewijs.

Voer `python3 <skillpad>/scripts/check_state.py <registratie>` uit wanneer de registratie dit schema gebruikt. Een mislukte controle blokkeert verder bouwen of installeren vanuit die bron, niet onafhankelijk onderzoek. Onderzoek verschillen; wijzig de registratie nooit alleen om de controle groen te maken. Een ontbrekende registratie betekent onbekend, niet actueel. Reconstrueer eerst uit bewijs. Nieuw werk mag afstammen van de bewezen basis; bedoelde oudere branches blijven toegestaan wanneer de gebruiker dat expliciet vraagt en de afwijking benoemd is.

## Bouwen en committen

Houd bron, gebouwd, getest, gepusht en geïnstalleerd apart. Leg bij elke candidate exacte bron- en dependencycommits, buildprofiel, dirty-status en artifact-SHA256 vast. Controleer dat werkmap en builddependencies naar de bedoelde bron wijzen. Neem versie plus commit automatisch op in installatiebuilds waar het project dat ondersteunt; `dev` of een oude tekst is onvoldoende identificatie.

Controleer vóór push dat submodulecommits bereikbaar zijn via de geconfigureerde remote. Publiceer dependencies vóór de hoofdrepo. Commit/push/deploy alleen binnen bestaande gebruikersautorisatie. Maak geen nieuwe release of installatie om alleen de registratie bij te werken.

## Bij afronden

Na build of push alleen de bijbehorende status bijwerken: dit is geen installatie. Na een daadwerkelijk geverifieerde installatie: registreer broncommit, dependencies, artifacthash, target, datum en verificatiebewijs; werk daarna de bedoelde actieve bronverwijzing bij. Controleer het opgeloste pad en voer de identiteitscontrole opnieuw uit. Bewaar het vorige doel als herstelinformatie; overschrijf uitsluitend een bewezen symlink, nooit een directory of onbekende verwijzing. Bij mislukte installatie blijft de laatste bewezen installatie actief.

Firmware: controleer actief OTA-slot vóór schrijven en readback na schrijven; behoud overige partities. Apps: controleer geïnstalleerde build-identiteit via het platform. Vraag alleen ontbrekende autorisatie voor echte apparaat-/deployacties, niet opnieuw wanneer die al gegeven is.

## X3

Registratie: `/Volumes/2TB/Development/Projects/X3-actueel/deployment-state.json`. Actieve bron: dezelfde map, symlink `firmware`. De historische verwijzing wees naar dashboard-v3-firmware terwijl de X3 al de v1.6.0-upgrade draaide. Controleer de registratie vóór nieuwe X3-werkzaamheden. Oude bronnen blijven bewaard; alleen de actieve verwijzing verandert.

De scriptcontrole bewijst pad, bronafstamming, dependency-identiteit en artifacthash. Zij vervangt geen build-, test-, readback- of hardwarebewijs. Test nieuwe controles met zowel een verouderde als een juiste verwijzing voordat je ze afgerond noemt.
