# Eén-partitie dashboardfirmware — implementatieplan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** De agenda-BLE-ontvangst in de reader-firmware zelf draaien in plaats van in een aparte partitie, zodat een wakecyclus één keer opstart in plaats van drie en er accu bespaard wordt.

**Architecture:** De ontvanger is nu een losse binary in `app1`; de reader draagt bij een timerwake over met een herstart, en herstart nog een keer terug. Dit plan haalt eerst flashruimte vrij (ongebruikte talen en functies uit de build), zet dan de NimBLE-stack in de reader-build, en verplaatst de ontvangstlogica naar een module die de radio alleen tijdens het luistervenster initialiseert en daarna weer afbreekt. De partitiewissel en de twee herstarts vervallen.

**Tech Stack:** PlatformIO / ESP-IDF (Arduino), ESP32-C3, NimBLE, GoogleTest via CMake voor hosttests.

---

## Uitgangswaarden (gemeten 2026-08-28)

Deze getallen zijn de nulmeting. Zonder deze kan achteraf niet worden aangetoond dat het gewerkt heeft.

Reproduceer ze met `python3 scripts/trace_power_report.py <tracebestand>`.

| Meting | Waarde | Bron |
|---|---|---|
| Rustverbruik (nachtvenster, geen enkele wake) | 2,36 mA | trace 26-08 20:00 → 27-08 04:54 (8,9 u) |
| Verbruik met wakes | 3,18 mA | trace 27-08 04:54 → 28-08 08:13 (27,3 u, 72 cycli) |
| Kosten per wakecyclus | ~313 µAh | verschil van bovenstaande |
| Aandeel wakes in dagverbruik | ~25% (19 van 75 mAh) | afgeleid, 60 cycli/dag |
| Flash reader | 6.321.935 van 6.553.600 bytes (96,5%) | build |
| Flash BLE-stack alleen | ~169 KB | symbolen `spike-ble-receiver-x3` |
| Statisch RAM reader | 76.292 bytes (23%) | build |
| Heap vrij met boek open, laagste punt | 82.932 bytes | serieel, `[MEM] Periodic` |
| Grootste aaneengesloten blok | 61.428 bytes | idem |

**Verwachte winst:** de volledige reader-boot die tekent blijft bestaan; wat vervalt is de korte handoff-boot, de receiver-boot en twee herstarts. Reken op 30–50% van de 19 mAh/dag, dus **6–10 mAh/dag op een totaal van 75**. Dat is een bescheiden maar reële winst; als de meting in Taak 9 daar ver onder blijft, is het werk het niet waard geweest en is dat een geldige uitkomst.

**Belangrijke nuance die het plan bewust níet aanneemt:** een cyclus die eindigt in `stage=timedout` boot vandaag al *niet* volledig door — `appendEarlyBootTrace` schrijft die regel vóór de SD-mount en het toestel gaat direct terug slapen. De winst zit dus niet in "78% van de cycli tekent onnodig", die verspilling bestaat niet.

---

## Bestandsindeling

| Bestand | Verantwoordelijkheid |
|---|---|
| `platformio.ini` | Nieuwe env `dashboard-x3`; BLE niet langer in `lib_ignore`; taalselectie |
| `scripts/gen_i18n.py` | Moet een taalselectie uit een build-flag kunnen honoreren |
| `src/spikes/ble_handoff/InProcessReceiver.h/.cpp` | **Nieuw.** Start NimBLE, draait het luistervenster, breekt af. Wat `BleReceiverMain.cpp` doet, maar als aanroepbare functie in plaats van een `app_main` |
| `src/spikes/ble_handoff/AgendaWakePolicy.h/.cpp` | Krijgt een routekeuze die "in proces" als derde optie kent |
| `src/main.cpp` | Roept de ontvanger aan in plaats van de partitiewissel |
| `test/ble_handoff_record/AgendaWakePolicyTest.cpp` | Tests voor de nieuwe routekeuze |

---

## Taak 0: Hosttests draaibaar maken

Zonder dit is er geen TDD mogelijk; `cmake` ontbreekt op deze machine, waardoor de GoogleTest-suite niet kan draaien.

- [ ] **Stap 1: Installeer cmake**

```bash
brew install cmake
```

- [ ] **Stap 2: Configureer de testbuild**

```bash
cd ~/Development/worktrees/CrossInk/dashboard-v3-firmware
cmake -S test -B build/tests -DCMAKE_BUILD_TYPE=Release
```

Verwacht: eindigt met `-- Generating done` en `-- Build files have been written to: .../build/tests`.

- [ ] **Stap 3: Bouw en draai de bestaande suite**

```bash
cmake --build build/tests --target BleHandoffRecordTest -j8
cd build/tests && ctest --output-on-failure
```

Verwacht: alle tests slagen, inclusief de vier `AnchorsWakesToTheClockGrid*`-tests die op 2026-08-28 zijn toegevoegd maar nog nooit via ctest gedraaid hebben.

- [ ] **Stap 4: Commit indien er iets aan de testconfiguratie moest wijzigen**

```bash
git add -A && git commit -m "chore: make the host test suite runnable"
```

---

## Taak 1: Talen terugbrengen tot Nederlands en Engels

Grootste besparing met het minste risico: 0,30 MB, en er verandert niets aan wat op het scherm staat.

**Files:**
- Modify: `scripts/gen_i18n.py`
- Modify: `platformio.ini`

- [ ] **Stap 1: Stel vast hoe de talen nu gegenereerd worden**

```bash
grep -n "STRINGS_\|LANGUAGES\|langs" scripts/gen_i18n.py | head -20
```

Noteer welke lijst de 28 taalcodes bepaalt. Dat is het aangrijpingspunt.

- [ ] **Stap 2: Voeg een taalfilter toe via een omgevingsvariabele**

In `scripts/gen_i18n.py`, waar de talenlijst wordt opgebouwd:

```python
import os

# Een build kan de talen beperken; leeg of ongezet betekent "alle talen",
# zodat de standaardbuild zich niet anders gedraagt dan voorheen.
_only = os.environ.get("CROSSINK_I18N_LANGUAGES", "").strip()
if _only:
    wanted = {c.strip().upper() for c in _only.split(",") if c.strip()}
    languages = [l for l in languages if l.upper() in wanted]
    if not languages:
        raise SystemExit(f"CROSSINK_I18N_LANGUAGES={_only!r} liet geen enkele taal over")
```

Pas `languages` aan naar de werkelijke variabelenaam uit stap 1.

- [ ] **Stap 3: Controleer dat de standaardbuild onveranderd blijft**

```bash
~/.platformio/penv/bin/pio run -e spike-ble-reader-x3 2>&1 | grep "^Flash:"
```

Verwacht: 6.321.935 bytes, precies gelijk aan de nulmeting. Zo niet, dan raakt het filter iets dat het niet zou moeten raken.

- [ ] **Stap 4: Bouw met alleen NL en EN**

```bash
CROSSINK_I18N_LANGUAGES=NL,EN ~/.platformio/penv/bin/pio run -e spike-ble-reader-x3 2>&1 | grep "^Flash:"
```

Verwacht: ongeveer 6.004.000 bytes, dus ruwweg 318 KB minder.

- [ ] **Stap 5: Commit**

```bash
git add scripts/gen_i18n.py
git commit -m "feat: let a build select which languages are compiled in"
```

---

## Taak 2: Dashboard-build met beperkte functieset

Nog geen BLE — eerst een env die bestaat en bouwt, zodat de volgende taak iets heeft om op voort te bouwen.

**Files:**
- Modify: `platformio.ini`

- [ ] **Stap 1: Voeg de env toe**

Onder `[env:spike-ble-reader-x3]` in `platformio.ini`:

```ini
; De dagelijkse dashboardfirmware. Zelfde reader als spike-ble-reader-x3 (lezen
; blijft werken), maar zonder de functies die niet gebruikt worden en met de
; talen beperkt. De vrijgekomen ruimte is wat de BLE-stack in taak 3 nodig heeft.
[env:dashboard-x3]
extends = env:spike-ble-reader-x3
build_flags =
  ${env:spike-ble-reader-x3.build_flags}
  -DCROSSINK_WITHOUT_DICTIONARY=1
  -DCROSSINK_WITHOUT_OPDS=1
extra_scripts =
  ${env:spike-ble-reader-x3.extra_scripts}
```

- [ ] **Stap 2: Zet de taalbeperking vast voor deze env**

Voeg vóór de `build_flags` van `[env:dashboard-x3]` toe:

```ini
custom_i18n_languages = NL,EN
```

en zorg dat het buildscript die doorgeeft. Controleer eerst hoe andere `custom_*`-sleutels gelezen worden:

```bash
grep -rn "custom_firmware_device_type" scripts/*.py | head -3
```

Volg dat patroon: lees `custom_i18n_languages` uit `env.GetProjectOption(...)` en zet er `CROSSINK_I18N_LANGUAGES` mee in de omgeving vóór `gen_i18n.py` draait.

- [ ] **Stap 3: Bouw en bevestig de besparing**

```bash
~/.platformio/penv/bin/pio run -e dashboard-x3 2>&1 | grep -E "^Flash:|^RAM:"
```

Verwacht: onder de 6.010.000 bytes flash, dus minimaal 310 KB vrij ten opzichte van de nulmeting.

- [ ] **Stap 4: Commit**

```bash
git add platformio.ini scripts/
git commit -m "feat: add a dashboard build without dictionary and OPDS"
```

---

## Taak 3: BLE in de dashboard-build krijgen

Dit is het moment waarop blijkt of het past. Nog geen gedragswijziging — alleen linken.

**Files:**
- Modify: `platformio.ini`

- [ ] **Stap 1: Haal BLE uit `lib_ignore` voor deze env**

In `[env:dashboard-x3]`:

```ini
lib_ignore =
  HalClockSim
```

Dit overschrijft `base.lib_ignore`, waar `BLE` in staat. `HalClockSim` blijft uitgesloten omdat de basis dat ook doet.

- [ ] **Stap 2: Forceer dat de stack ook echt gelinkt wordt**

Een niet-aangeroepen bibliotheek wordt weggeoptimaliseerd, dus zonder aanroep meet je niets. Voeg tijdelijk boven aan `setup()` in `src/main.cpp` toe:

```cpp
#include <BLEDevice.h>
// TIJDELIJK — alleen om te meten of de stack past. Weg in taak 5.
void crossinkLinkProbe() { BLEDevice::getInitialized(); }
```

- [ ] **Stap 3: Bouw en lees de flashbezetting**

```bash
~/.platformio/penv/bin/pio run -e dashboard-x3 2>&1 | grep -E "^Flash:|^RAM:"
```

Verwacht: onder de 6.553.600 bytes. Verwachting op basis van de nulmeting is ongeveer 6.180.000 (6.004.000 na taak 1 en 2, plus ~169 KB BLE), dus zo'n 370 KB marge.

- [ ] **Stap 4: Als het niet past, stop hier en meld het**

Dan is de aanname onder dit plan onjuist en moeten er meer functies uit voordat het zin heeft verder te gaan. Ga niet improviseren met het weghalen van lettertypen: Bitter is het leeslettertype van de gebruiker, Inter is de hele interface, en Lexend Deca recht is het dashboard. Alleen Lexend Deca cursief en vet-cursief (0,19 MB) zijn vrij, en alleen als Lexend Deca geen leesoptie meer hoeft te zijn — dat is een gebruikersbeslissing, geen implementatiedetail.

- [ ] **Stap 5: Commit**

```bash
git add platformio.ini src/main.cpp
git commit -m "chore: link the BLE stack into the dashboard build to measure fit"
```

---

## Taak 4: Heapmeting met de radio erbij

Flash is nu bekend; heap is de tweede en gevaarlijkere beperking. Meten vóór er logica verplaatst wordt.

- [ ] **Stap 1: Laat de probe de stack echt initialiseren**

Vervang de probe uit taak 3 in `src/main.cpp` door:

```cpp
// TIJDELIJK — meet de heapkosten van een geïnitialiseerde stack. Weg in taak 5.
static void crossinkHeapProbe() {
  LOG_INF("MEM", "voor BLE-init: heap free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  BLEDevice::init("x3-probe");
  LOG_INF("MEM", "na BLE-init:   heap free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  BLEDevice::deinit(true);
  LOG_INF("MEM", "na BLE-deinit: heap free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}
```

Roep die aan nadat het scherm en de lettertypen geladen zijn — dat is het punt waarop de heap het laagst staat (82.932 bytes in de nulmeting).

- [ ] **Stap 2: Flash en lees mee**

```bash
~/.platformio/penv/bin/pio run -e dashboard-x3 -t upload --upload-port /dev/cu.usbmodem31401
~/.platformio/penv/bin/python /tmp/x3-mem-capture.py 30
```

- [ ] **Stap 3: Beoordeel de uitkomst**

Noteer de drie regels. Twee dingen moeten kloppen:

1. `free` na init blijft ruim boven nul — reken op 30 tot 50 KB verbruik, dus er zou zo'n 30 KB over moeten blijven van de 83.
2. `free` na deinit keert terug naar ongeveer de waarde vóór init. Doet hij dat niet, dan lekt de stack geheugen en is aan/uit schakelen per venster geen begaanbare weg; dan moet de stack één keer geïnitialiseerd blijven en is de heapmarge structureel kleiner.

Blijft er minder dan 15 KB over, stop dan en meld het. Dat is te weinig marge voor het indexeren van een nieuw boek.

- [ ] **Stap 4: Leg de meting vast**

```bash
git commit --allow-empty -m "docs: record BLE heap measurement on device

voor init:   free=<X> maxAlloc=<Y>
na init:     free=<X> maxAlloc=<Y>
na deinit:   free=<X> maxAlloc=<Y>"
```

---

## Taak 5: Ontvanger als aanroepbare module

**Files:**
- Create: `src/spikes/ble_handoff/InProcessReceiver.h`
- Create: `src/spikes/ble_handoff/InProcessReceiver.cpp`
- Modify: `src/main.cpp` (probe uit taak 3/4 verwijderen)

- [ ] **Stap 1: Lees wat de bestaande ontvanger doet**

```bash
sed -n '100,200p' src/spikes/ble_handoff/BleReceiverMain.cpp
```

Noteer: de service- en characteristic-UUID's, de advertentienaam, hoe een pakket binnenkomt en hoe het venster afloopt. Die logica wordt niet herschreven, alleen anders aangeroepen.

- [ ] **Stap 2: Schrijf de header**

```cpp
#pragma once

#include <stdint.h>

#include "AgendaWakePolicy.h"

namespace dashboard {

// Draait het agenda-ontvangstvenster binnen het lopende readerproces, in
// plaats van in de losse receiver-partitie.
//
// De radio gaat aan bij binnenkomst en onvoorwaardelijk uit bij vertrek, ook
// bij een mislukking: de stack laten staan kost heap die de reader nodig heeft
// zodra er weer een boek geopend wordt. Dat is precies de reden dat de
// ontvanger ooit een aparte partitie kreeg, en het is de aanname waarop deze
// hele opzet rust.
//
// `windowMs` is hoe lang er geluisterd wordt voordat er wordt opgegeven.
// Blokkeert voor de duur van het venster.
ReceiverResult runReceiverWindow(uint32_t windowMs);

}  // namespace dashboard
```

- [ ] **Stap 3: Verplaats de logica in plaats van hem te kopiëren**

De anonieme namespace boven in `BleReceiverMain.cpp` (regels 1–110: `SERVICE_UUID`, `WRITE_UUID`, `STATUS_UUID`, `DEVICE_NAME`, `serverCallbacks`, `writeCallbacks`, `statusCharacteristic`, `framePending`, `pendingFrame`, `pendingLength`, `pendingMux`, `assembler`, `receiverWindow`, `notify`) verhuist ongewijzigd naar `InProcessReceiver.cpp`. Kopieer hem niet: twee exemplaren van deze toestand lopen gegarandeerd uit elkaar.

Voeg daaronder toe:

```cpp
ReceiverResult runReceiverWindow(const uint32_t windowMs) {
  if (!BLEDevice::init(DEVICE_NAME)) return ReceiverResult::TimedOut;

  // Eén uitgang, zodat `deinit` niet overgeslagen kan worden. Blijft de stack
  // staan, dan is de heap weg zodra de gebruiker een boek opent — precies het
  // probleem dat de aparte partitie ooit oploste.
  ReceiverResult result = ReceiverResult::TimedOut;
  BLEServer* server = BLEDevice::createServer();
  if (server != nullptr) {
    server->setCallbacks(&serverCallbacks);
    BLEService* service = server->createService(SERVICE_UUID);
    BLECharacteristic* writable = service->createCharacteristic(WRITE_UUID, BLECharacteristic::PROPERTY_WRITE);
    statusCharacteristic = service->createCharacteristic(
        STATUS_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    writable->setCallbacks(&writeCallbacks);
    service->start();
    BLEAdvertising* advertising = server->getAdvertising();
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->setScanResponse(false);
    advertising->start();

    const uint32_t startedAt = millis();
    while (millis() - startedAt < windowMs) {
      if (packageAccepted) {
        result = ReceiverResult::Accepted;
        break;
      }
      // Dezelfde framebehandeling als `loop()` in BleReceiverMain.cpp: haal
      // een binnengekomen frame onder de critical section vandaan en voer het
      // aan de assembler.
      static std::array<uint8_t, MAX_FRAME_SIZE> frame{};
      size_t length = 0;
      portENTER_CRITICAL(&pendingMux);
      if (framePending) {
        length = pendingLength;
        std::memcpy(frame.data(), pendingFrame.data(), length);
        framePending = false;
      }
      portEXIT_CRITICAL(&pendingMux);
      if (length == 0) {
        delay(5);
        continue;
      }
      const dashboard::TransferResult transfer = assembler.accept(frame.data(), length);
      if (transfer.status == dashboard::TransferStatus::Ready) {
        notify(0x01, transfer.packageId, transfer.received);
      } else if (transfer.status == dashboard::TransferStatus::Progress) {
        notify(0x02, transfer.packageId, transfer.received);
      }
    }
    advertising->stop();
  }

  BLEDevice::deinit(true);
  return result;
}
```

- [ ] **Stap 4: Maak `BleReceiverMain.cpp` dun**

De partitieroute blijft bestaan als terugweg, dus die moet blijven bouwen. Vervang zijn `setup()`-body na de routecontrole door een aanroep van `runReceiverWindow(20000)` en laat `loop()` leeg. Zo draaien beide routes op dezelfde code en kan er geen gedragsverschil tussen de twee ontstaan.

- [ ] **Stap 5: Verwijder de probe uit taak 3 en 4 uit `src/main.cpp`**

- [ ] **Stap 6: Bouwen**

```bash
~/.platformio/penv/bin/pio run -e dashboard-x3 2>&1 | grep -E "^Flash:|SUCCESS|error"
```

- [ ] **Stap 7: Commit**

```bash
git add src/spikes/ble_handoff/InProcessReceiver.h src/spikes/ble_handoff/InProcessReceiver.cpp src/main.cpp
git commit -m "feat: add an in-process agenda receiver window"
```

---

## Taak 6: Routekeuze uitbreiden

**Files:**
- Modify: `src/spikes/ble_handoff/AgendaWakePolicy.h`
- Modify: `src/spikes/ble_handoff/AgendaWakePolicy.cpp`
- Test: `test/ble_handoff_record/AgendaWakePolicyTest.cpp`

- [ ] **Stap 1: Schrijf de falende test**

```cpp
// De route hangt nu ook af van of de ontvanger in het proces zelf kan draaien.
// Kan dat, dan mag er geen partitiewissel meer plaatsvinden: die wissel is
// precies de twee herstarts die dit werk wil besparen.
TEST(AgendaWakePolicy, PrefersTheInProcessReceiverWhenAvailable) {
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::Timer, true),
            dashboard::AgendaBootRoute::InProcessReceiver);
  // Zonder in-proces-ontvanger blijft het de oude partitiewissel.
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::Timer, false),
            dashboard::AgendaBootRoute::Receiver);
  // En een knopwake luistert nog steeds niet, ongeacht welke route beschikbaar is.
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(true, dashboard::WakeSource::PowerButton, true),
            dashboard::AgendaBootRoute::NormalReader);
  EXPECT_EQ(dashboard::chooseAgendaBootRoute(false, dashboard::WakeSource::Timer, true),
            dashboard::AgendaBootRoute::NormalReader);
}
```

- [ ] **Stap 2: Draai en zie hem falen**

```bash
cmake --build build/tests --target BleHandoffRecordTest -j8
```

Verwacht: compilatiefout, `chooseAgendaBootRoute` neemt geen derde argument.

- [ ] **Stap 3: Breid de enum en de functie uit**

In `AgendaWakePolicy.h`:

```cpp
enum class AgendaBootRoute : uint8_t { NormalReader, Receiver, InProcessReceiver };
```

en:

```cpp
// `inProcessAvailable` is waar wanneer de build de radio zelf kan draaien
// (`dashboard-x3`); dan vervalt de partitiewissel en de twee herstarts.
AgendaBootRoute chooseAgendaBootRoute(bool agendaCycleArmed, WakeSource wakeSource,
                                      bool inProcessAvailable = false);
```

In `AgendaWakePolicy.cpp`:

```cpp
AgendaBootRoute chooseAgendaBootRoute(const bool agendaCycleArmed, const WakeSource wakeSource,
                                      const bool inProcessAvailable) {
  if (!agendaCycleArmed || wakeSource != WakeSource::Timer) return AgendaBootRoute::NormalReader;
  return inProcessAvailable ? AgendaBootRoute::InProcessReceiver : AgendaBootRoute::Receiver;
}
```

- [ ] **Stap 4: Draai de tests**

```bash
cmake --build build/tests --target BleHandoffRecordTest -j8 && (cd build/tests && ctest --output-on-failure)
```

Verwacht: alles slaagt, inclusief de bestaande `RoutesOnlyTimerWakeToReceiver` — die roept met twee argumenten aan en krijgt via de standaardwaarde het oude gedrag.

- [ ] **Stap 5: Commit**

```bash
git add src/spikes/ble_handoff/AgendaWakePolicy.h src/spikes/ble_handoff/AgendaWakePolicy.cpp test/ble_handoff_record/AgendaWakePolicyTest.cpp
git commit -m "feat: route timer wakes to an in-process receiver when available"
```

---

## Taak 7: `main.cpp` de nieuwe route laten nemen

**Files:**
- Modify: `src/main.cpp:1046-1063`

- [ ] **Stap 1: Lees de huidige route**

```bash
sed -n '1044,1066p' src/main.cpp
```

- [ ] **Stap 2: Vervang het blok**

```cpp
#ifdef CROSSINK_BLE_HANDOFF_READER
  {
#ifdef CROSSINK_IN_PROCESS_RECEIVER
    constexpr bool inProcessAvailable = true;
#else
    constexpr bool inProcessAvailable = false;
#endif
    const auto route = dashboard::chooseAgendaBootRoute(
        retainedResult == dashboard::ReceiverResult::AwaitingWindow,
        wakeupReason == HalGPIO::WakeupReason::Timer ? dashboard::WakeSource::Timer
                                                     : dashboard::WakeSource::Other,
        inProcessAvailable);

    if (route == dashboard::AgendaBootRoute::InProcessReceiver) {
      // Geen partitiewissel: de radio draait hier, en het resultaat is meteen
      // bekend in plaats van pas na een herstart. De trace houdt dezelfde vorm
      // zodat oude en nieuwe logs naast elkaar te leggen zijn.
      dashboard::appendEarlyBootTrace(static_cast<uint8_t>(wakeupReason), retainedResult,
                                      dashboard::BootTraceStage::ReceiverHandoff,
                                      resetReasonName(rawResetReason));
      const dashboard::ReceiverResult result = dashboard::runReceiverWindow(20000);
      dashboard::retainReceiverResult(result);
      if (result == dashboard::ReceiverResult::Accepted) resumeAgendaAfterAccepted = true;
    } else if (route == dashboard::AgendaBootRoute::Receiver) {
      LOG_INF("BLEPAY", "Agenda timer wake; switching to isolated dashboard receiver");
      dashboard::appendEarlyBootTrace(static_cast<uint8_t>(wakeupReason), retainedResult,
                                      dashboard::BootTraceStage::ReceiverHandoff,
                                      resetReasonName(rawResetReason));
      if (dashboard_boot::switchToReceiver()) {
        delay(50);
        ESP.restart();
      }
      dashboard::retainReceiverResult(dashboard::ReceiverResult::None);
      LOG_ERR("BLEPAY", "Receiver slot unavailable; continuing reader boot");
    }
  }
#endif
```

Voeg bovenaan `#include "spikes/ble_handoff/InProcessReceiver.h"` toe bij de bestaande includes.

- [ ] **Stap 3: Zet de vlag aan in de dashboard-env**

In `platformio.ini`, bij `[env:dashboard-x3]`, aan `build_flags` toevoegen:

```ini
  -DCROSSINK_IN_PROCESS_RECEIVER=1
```

- [ ] **Stap 4: Bouw beide envs**

```bash
~/.platformio/penv/bin/pio run -e dashboard-x3 2>&1 | grep -E "SUCCESS|error"
~/.platformio/penv/bin/pio run -e spike-ble-reader-x3 2>&1 | grep -E "SUCCESS|error"
```

Beide moeten slagen: de oude env houdt de partitieroute, de nieuwe neemt de in-proces-route.

- [ ] **Stap 5: Commit**

```bash
git add src/main.cpp platformio.ini
git commit -m "feat: run the agenda receiver window without switching partitions"
```

---

## Taak 8: Op hardware bevestigen

- [ ] **Stap 1: Flash**

```bash
~/.platformio/penv/bin/pio run -e dashboard-x3 -t upload --upload-port /dev/cu.usbmodem31401
```

- [ ] **Stap 2: Kabel eruit en een uur laten lopen**

Op USB gedraagt het toestel zich anders; een meting aan de kabel zegt niets over accugedrag.

- [ ] **Stap 3: Lees de trace**

```bash
~/.platformio/penv/bin/python /tmp/x3-read-file.py /crossink-ble-trace.txt /tmp/x3pull/trace-eenpartitie.txt
tail -20 /tmp/x3pull/trace-eenpartitie.txt
```

Waar je op let:
- De wakes liggen op hele kwartieren (het raster uit commit `81c36f63`).
- Er staat **één** regel per cyclus in plaats van twee: er is geen terugkeerboot meer, dus geen `reset=SW`-regel die het oordeel nabrengt.
- `receiver=accepted` komt nog steeds voor. Zo niet, dan werkt de ontvangst niet en moet taak 5 nagelopen worden.

- [ ] **Stap 4: Commit de waarneming**

```bash
git commit --allow-empty -m "docs: confirm the in-process receiver on hardware"
```

---

## Taak 9: De besparing meten

Dit is de taak die bepaalt of het werk zin had. Sla hem niet over.

- [ ] **Stap 1: Laat het toestel een nacht en een ochtend op accu draaien**

Zelfde vorm als de nulmeting: een nachtperiode zonder wakes en een ochtendperiode met wakes.

- [ ] **Stap 2: Bereken opnieuw**

```bash
python3 scripts/trace_power_report.py /tmp/x3pull/trace-eenpartitie.txt
```

Het script zoekt zelf de langste ontlaadperiode (opladen onderbreekt een meting en maakt de uitkomst negatief) en daarbinnen het langste gat tussen twee traceregels als rustperiode.

- [ ] **Stap 3: Vergelijk met de nulmeting**

| | Nulmeting | Nu |
|---|---|---|
| Rustverbruik | 2,36 mA | ? |
| Per wakecyclus | ~313 µAh | ? |
| Etmaal | ~75 mAh | ? |

Verwacht: het rustverbruik is onveranderd (daar is niets aan gedaan) en de kosten per cyclus zijn 30 tot 50% lager.

- [ ] **Stap 4: Leg de uitkomst eerlijk vast**

```bash
git commit --allow-empty -m "docs: measure the battery effect of the single-partition receiver

per cyclus: <voor> -> <na>
per etmaal: <voor> -> <na>"
```

Valt de winst tegen, schrijf dat dan ook zo op. Een gemeten tegenvaller is bruikbare kennis; een niet-gemeten aanname is dat niet.

---

## Wat dit plan bewust niet doet

- **De receiver-partitie verwijderen.** `app1` blijft bestaan en `spike-ble-receiver-x3` blijft bouwen. Dat is de terugweg als de in-proces-ontvanger op hardware tegenvalt, en het kost niets om te laten staan.
- **Lettertypen weghalen.** Bitter is het leeslettertype, Inter de interface, Lexend Deca recht het dashboard. Alleen de cursieve Lexend-varianten zijn vrij (0,19 MB), en alleen als Lexend Deca geen leesoptie meer hoeft te zijn. Dat is een keuze voor de gebruiker.
- **De reader strippen.** Er wordt niets verwijderd, alleen anders geselecteerd per build. De CrossInk-fork blijft daardoor te mergen met upstream, wat voor de v1.5.1-update op de backlog uitmaakt.
- **Het luisteren tijdens een knopwake.** Dat wordt technisch mogelijk zodra de radio in het proces draait, maar het is een aparte gedragswijziging met een eigen afweging — een knopdruk zou dan stroom gaan kosten. Apart plannen.
