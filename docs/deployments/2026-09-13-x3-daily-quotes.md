# X3 dashboard — 365 dagelijkse quotes — 2026-09-13

Het persoonlijke X3-dashboard is bijgewerkt met 365 unieke Nederlandse en
Engelse quotes. De dagelijkse quote-ID gebruikt payloadformaat 3 met een
little-endian `uint16_t`; de firmware blijft bestaande formaat-2-pakketten
accepteren.

## Installatiebewijs

- Apparaat: ESP32-C3, MAC `d4:05:92:90:13:74`, 16 MB flash.
- Seriële poort: `/dev/cu.usbmodem31401`.
- De fysieke partitietabel is opnieuw uitgelezen: app0 op `0x10000` en app1 op
  `0x650000`, beide 6.553.600 bytes. De nieuwste geldige OTA-record had
  volgnummer 2 en selecteerde app1.
- Alleen de actieve dashboard-applicatie app1 op `0x650000` is geschreven.
  Bootloader, partitietabel, NVS, OTA-metadata, app0 en SPIFFS zijn niet
  geschreven.
- Geïnstalleerde image: 6.289.424 bytes; SHA-256
  `08dc03be35d7102d99ac22db3e7b9f13b96fad4af383da73a623c210142512a1`.
- De exact geschreven byte-range is teruggelezen en vergeleken. `cmp` en beide
  SHA-256-hashes bevestigden bytegelijkheid.
- De 32 KiB metadatazone `0x8000..0xffff` was voor en na de installatie
  byte-identiek; SHA-256
  `e81de87add3a7c2b9658247c6dcbfdb5fa70b98cb18c2adf070431c061013a2f`.
- De volledige pre-flash app1-partitie staat in
  `/Volumes/2TB/x3-flash-backups/2026-09-13-daily-quotes/preflash-app1-full.bin`;
  SHA-256
  `daef397411f2cd0c0052f9f257498d356d1f0a796d9980d2ddd3ede00d66dcb6`.

## Verificatie vóór installatie

- De Swift- en firmwaretabellen bevatten beide 365 unieke en entry-voor-entry
  identieke quotes: 185 Engels en 180 Nederlands.
- Alle 862 Swift-tests slaagden.
- De volledige BLE/dashboard-hostsuite draaide 223 tests: 222 slaagden en één
  artifacttest werd omgevingsafhankelijk overgeslagen; die test slaagde daarna
  afzonderlijk met zijn outputdirectory ingesteld.
- De productie-fontmeting rapporteerde `0 of 365 quotes overflow their box`.
- De schone `dashboard-x3`-build slaagde. De app-partitie houdt 264.176 bytes
  vrije ruimte over.

## Fysieke acceptatie

De seriële schrijf- en terugleescontrole bewijst de correcte installatie, maar
niet de e-inkweergave of een echte BLE-overdracht. Open de bijgewerkte iPhone-
app, verstuur het dashboard en controleer visueel dat de quote en auteur in de
footer verschijnen. Een pakket met een quote-ID boven 255 vormt de volledige
end-to-endcontrole van payloadformaat 3.
