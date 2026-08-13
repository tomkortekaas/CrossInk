#pragma once

#include "BleHandoffRecord.h"

namespace ble_handoff {

Status readPersisted(DecodedRecord& decoded, RecordBytes* rawBytes = nullptr);
Status persistAndVerify(const uint8_t* payload, size_t length, DecodedRecord& decoded);

}  // namespace ble_handoff
