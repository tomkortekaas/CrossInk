#include "BleHandoffNvs.h"

#include <algorithm>

#include <nvs.h>

namespace ble_handoff {
namespace {

constexpr char NVS_NAMESPACE[] = "blehandoff";
constexpr char NVS_KEY[] = "payload";

bool isMissingOrInvalid(const Status status) {
  switch (status) {
    case Status::NotFound:
    case Status::InvalidSize:
    case Status::InvalidMagic:
    case Status::InvalidVersion:
    case Status::InvalidLength:
    case Status::InvalidCrc:
      return true;
    default:
      return false;
  }
}

}  // namespace

Status readPersisted(DecodedRecord& decoded, RecordBytes* rawBytes) {
  nvs_handle_t handle = 0;
  const esp_err_t openResult = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
  if (openResult == ESP_ERR_NVS_NOT_FOUND) return Status::NotFound;
  if (openResult != ESP_OK) return Status::OpenFailed;

  size_t storedSize = 0;
  const esp_err_t sizeResult = nvs_get_blob(handle, NVS_KEY, nullptr, &storedSize);
  if (sizeResult == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return Status::NotFound;
  }
  if (sizeResult != ESP_OK) {
    nvs_close(handle);
    return Status::ReadFailed;
  }
  if (storedSize != RECORD_SIZE) {
    nvs_close(handle);
    return Status::InvalidSize;
  }

  RecordBytes stored{};
  const esp_err_t readResult = nvs_get_blob(handle, NVS_KEY, stored.data(), &storedSize);
  nvs_close(handle);
  if (readResult != ESP_OK || storedSize != stored.size()) return Status::ReadFailed;

  const Status validation = validateRecord(stored.data(), stored.size(), decoded);
  if (validation == Status::Ok && rawBytes != nullptr) *rawBytes = stored;
  return validation;
}

Status persistAndVerify(const uint8_t* payload, const size_t length, DecodedRecord& decoded) {
  DecodedRecord current{};
  const Status currentStatus = readPersisted(current);
  if (currentStatus != Status::Ok && !isMissingOrInvalid(currentStatus)) return currentStatus;

  uint32_t sequence = 0;
  const Status sequenceStatus = nextSequence(currentStatus == Status::Ok, current.sequence, sequence);
  if (sequenceStatus != Status::Ok) return sequenceStatus;

  RecordBytes candidate{};
  const Status buildStatus = buildRecord(payload, length, sequence, candidate);
  if (buildStatus != Status::Ok) return buildStatus;

  nvs_handle_t handle = 0;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return Status::OpenFailed;

  if (nvs_set_blob(handle, NVS_KEY, candidate.data(), candidate.size()) != ESP_OK) {
    nvs_close(handle);
    return Status::WriteFailed;
  }
  if (nvs_commit(handle) != ESP_OK) {
    nvs_close(handle);
    return Status::CommitFailed;
  }
  nvs_close(handle);

  DecodedRecord verified{};
  const Status verifyStatus = readPersisted(verified);
  if (verifyStatus != Status::Ok || verified.sequence != sequence || verified.length != length ||
      !std::equal(payload, payload + length, verified.payload.begin())) {
    return Status::VerifyFailed;
  }

  decoded = verified;
  return Status::Ok;
}

}  // namespace ble_handoff
