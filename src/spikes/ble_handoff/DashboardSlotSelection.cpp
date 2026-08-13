#include "DashboardSlotSelection.h"

namespace dashboard {

SlotDecision chooseSlots(const SlotState& slot0, const SlotState& slot1, const int8_t recordedSelection) {
  SlotDecision decision{};
  if (!slot0.valid && !slot1.valid) return decision;

  if (slot0.valid && (!slot1.valid || slot0.packageId >= slot1.packageId)) {
    decision.selected = 0;
    decision.writeTarget = 1;
  } else {
    decision.selected = 1;
    decision.writeTarget = 0;
  }
  decision.selectionNeedsRepair = recordedSelection != decision.selected;
  return decision;
}

bool isNewerPackage(const SlotState& slot0, const SlotState& slot1, const uint32_t candidateId) {
  uint32_t highest = 0;
  bool haveCurrent = false;
  if (slot0.valid) {
    highest = slot0.packageId;
    haveCurrent = true;
  }
  if (slot1.valid && (!haveCurrent || slot1.packageId > highest)) {
    highest = slot1.packageId;
    haveCurrent = true;
  }
  return !haveCurrent || candidateId > highest;
}

}  // namespace dashboard
