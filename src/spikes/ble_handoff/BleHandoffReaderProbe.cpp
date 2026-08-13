#include "BleHandoffReaderProbe.h"

#ifdef CROSSINK_BLE_HANDOFF_READER

#include <Logging.h>

#include "BleHandoffNvs.h"

namespace BleHandoffReaderProbe {

void logPersistedPayload() {
  ble_handoff::DecodedRecord decoded{};
  const ble_handoff::Status status = ble_handoff::readPersisted(decoded);
  if (status != ble_handoff::Status::Ok) {
    LOG_INF("BLEPAY", "No valid retained payload (status=%u)", static_cast<unsigned>(status));
    return;
  }

  char printable[ble_handoff::MAX_PAYLOAD_SIZE + 1]{};
  for (size_t index = 0; index < decoded.length; ++index) {
    const uint8_t value = decoded.payload[index];
    printable[index] = (value >= 0x20U && value <= 0x7EU) ? static_cast<char>(value) : '.';
  }

  LOG_INF("BLEPAY", "PAYLOAD VALID sequence=%u length=%u crc=%08x payload=%s", decoded.sequence,
          static_cast<unsigned>(decoded.length), decoded.crc, printable);
}

}  // namespace BleHandoffReaderProbe

#endif
