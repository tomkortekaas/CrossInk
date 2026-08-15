#include "BleHandoffNvs.h"

#include <Logging.h>
#include <nvs.h>

#include <algorithm>

#include "DashboardSlotSelection.h"

namespace dashboard {
namespace {

constexpr char NVS_NAMESPACE[] = "x3dashboard";
constexpr char SLOT_KEYS[][6] = {"slot0", "slot1"};
constexpr char SELECTED_KEY[] = "selected";
constexpr uint32_t SLOT_MAGIC = 0x44503358U;
// Bumped when MAX_PACKAGE_SIZE grew: a SlotRecord written by the previous
// version is a different size and would fail readSlot's length check anyway,
// but rejecting it by version says why. A device flashed across this boundary
// forgets its stored dashboard and renders the next package it receives.
constexpr uint8_t STORAGE_VERSION = 2;

struct SlotRecord {
  uint32_t magic = SLOT_MAGIC;
  uint8_t version = STORAGE_VERSION;
  uint8_t reserved = 0;
  uint16_t length = 0;
  PackageBytes bytes{};
  uint32_t crc = 0;
};

static SlotRecord slotWorkspace[2];
static PackageHeader decodeWorkspace;

bool readSlot(nvs_handle_t handle, int index, SlotState& state) {
  state = {};
  size_t size = sizeof(SlotRecord);
  if (nvs_get_blob(handle, SLOT_KEYS[index], &slotWorkspace[index], &size) != ESP_OK || size != sizeof(SlotRecord)) {
    return false;
  }
  const SlotRecord& record = slotWorkspace[index];
  // peekPackageHeader (not decodePackage/decodeWidgetGridPackage) so any
  // current or future template's package can be validated and persisted
  // without this storage layer knowing its content shape.
  if (record.magic != SLOT_MAGIC || record.version != STORAGE_VERSION || record.length > MAX_PACKAGE_SIZE ||
      crc32(reinterpret_cast<const uint8_t*>(&record), sizeof(SlotRecord) - sizeof(uint32_t)) != record.crc ||
      peekPackageHeader(record.bytes.data(), record.length, decodeWorkspace) != Status::Ok) {
    return false;
  }
  state = {true, decodeWorkspace.packageId};
  return true;
}

PersistStatus load(nvs_handle_t handle, PersistedPackage& output, SlotDecision* decisionOut = nullptr) {
  SlotState states[2]{};
  readSlot(handle, 0, states[0]);
  readSlot(handle, 1, states[1]);
  uint8_t selectedRaw = 0xFF;
  const int8_t recorded = nvs_get_u8(handle, SELECTED_KEY, &selectedRaw) == ESP_OK && selectedRaw < 2
                              ? static_cast<int8_t>(selectedRaw)
                              : static_cast<int8_t>(-1);
  const SlotDecision decision = chooseSlots(states[0], states[1], recorded);
  if (decisionOut != nullptr) *decisionOut = decision;
  if (decision.selected < 0) return PersistStatus::NotFound;

  const SlotRecord& record = slotWorkspace[decision.selected];
  output.bytes = record.bytes;
  output.length = record.length;
  output.slot = decision.selected;
  if (peekPackageHeader(output.bytes.data(), output.length, output.header) != Status::Ok) {
    return PersistStatus::ReadFailed;
  }
  return PersistStatus::Ok;
}

}  // namespace

PersistStatus readLastKnownGood(PersistedPackage& output) {
  nvs_handle_t handle = 0;
  const esp_err_t opened = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return PersistStatus::NotFound;
  if (opened != ESP_OK) return PersistStatus::OpenFailed;
  const PersistStatus status = load(handle, output);
  nvs_close(handle);
  return status;
}

PersistStatus persistIfNewer(const uint8_t* bytes, const size_t length, PersistedPackage& output,
                             Status* detailOut) {
  PackageHeader candidate{};
  const Status peekStatus = peekPackageHeader(bytes, length, candidate);
  if (peekStatus != Status::Ok) {
    if (detailOut != nullptr) *detailOut = peekStatus;
    LOG_ERR("BLEPAY", "peekPackageHeader failed status=%u length=%u", static_cast<unsigned>(peekStatus), length);
    return PersistStatus::InvalidPackage;
  }

  nvs_handle_t handle = 0;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    LOG_ERR("BLEPAY", "persistIfNewer: nvs_open failed");
    return PersistStatus::OpenFailed;
  }
  // Static for the same reason as the slot workspace above: a PersistedPackage
  // embeds a full PackageBytes, which is too large to place on the stack.
  // persistIfNewer is only ever called from the receiver's loop().
  static PersistedPackage current;
  current = {};
  SlotDecision decision{};
  const PersistStatus currentStatus = load(handle, current, &decision);
  if (currentStatus != PersistStatus::Ok && currentStatus != PersistStatus::NotFound) {
    LOG_ERR("BLEPAY", "persistIfNewer: load current failed status=%u", static_cast<unsigned>(currentStatus));
    nvs_close(handle);
    return currentStatus;
  }
  if (!isNewerPackage(currentStatus == PersistStatus::Ok && current.slot == 0 ? SlotState{true, current.header.packageId}
                                                                             : SlotState{},
                      currentStatus == PersistStatus::Ok && current.slot == 1 ? SlotState{true, current.header.packageId}
                                                                             : SlotState{},
                      candidate.packageId)) {
    LOG_ERR("BLEPAY", "persistIfNewer: stale candidate=%u currentSlot=%d currentId=%u", candidate.packageId,
            currentStatus == PersistStatus::Ok ? current.slot : -1, current.header.packageId);
    output = current;
    nvs_close(handle);
    return PersistStatus::Stale;
  }

  SlotRecord& record = slotWorkspace[decision.writeTarget];
  record = {};
  record.magic = SLOT_MAGIC;
  record.version = STORAGE_VERSION;
  record.length = static_cast<uint16_t>(length);
  std::copy_n(bytes, length, record.bytes.begin());
  record.crc = crc32(reinterpret_cast<const uint8_t*>(&record), sizeof(SlotRecord) - sizeof(uint32_t));
  if (nvs_set_blob(handle, SLOT_KEYS[decision.writeTarget], &record, sizeof(record)) != ESP_OK) {
    LOG_ERR("BLEPAY", "persistIfNewer: nvs_set_blob failed target=%d", decision.writeTarget);
    nvs_close(handle);
    return PersistStatus::WriteFailed;
  }
  if (nvs_commit(handle) != ESP_OK) {
    LOG_ERR("BLEPAY", "persistIfNewer: nvs_commit (blob) failed");
    nvs_close(handle);
    return PersistStatus::CommitFailed;
  }
  SlotState verified{};
  const bool readOk = readSlot(handle, decision.writeTarget, verified);
  if (!readOk || verified.packageId != candidate.packageId) {
    LOG_ERR("BLEPAY", "persistIfNewer: verify failed readOk=%d verifiedId=%u candidateId=%u", readOk,
            verified.packageId, candidate.packageId);
    nvs_close(handle);
    return PersistStatus::VerifyFailed;
  }
  if (nvs_set_u8(handle, SELECTED_KEY, static_cast<uint8_t>(decision.writeTarget)) != ESP_OK || nvs_commit(handle) != ESP_OK) {
    LOG_ERR("BLEPAY", "persistIfNewer: nvs_set_u8/commit (selected) failed");
    nvs_close(handle);
    return PersistStatus::CommitFailed;
  }
  const PersistStatus finalStatus = load(handle, output);
  nvs_close(handle);
  if (!(finalStatus == PersistStatus::Ok && output.header.packageId == candidate.packageId)) {
    LOG_ERR("BLEPAY", "persistIfNewer: final verify mismatch finalStatus=%u outputId=%u candidateId=%u",
            static_cast<unsigned>(finalStatus), output.header.packageId, candidate.packageId);
  }
  return finalStatus == PersistStatus::Ok && output.header.packageId == candidate.packageId ? PersistStatus::Ok
                                                                                             : PersistStatus::VerifyFailed;
}

}  // namespace dashboard
