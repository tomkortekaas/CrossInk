#pragma once

#include <cstdint>

namespace dashboard {

struct SlotState {
  bool valid = false;
  uint32_t packageId = 0;
};

struct SlotDecision {
  int8_t selected = -1;
  int8_t writeTarget = 0;
  bool selectionNeedsRepair = false;
};

SlotDecision chooseSlots(const SlotState& slot0, const SlotState& slot1, int8_t recordedSelection);
bool isNewerPackage(const SlotState& slot0, const SlotState& slot1, uint32_t candidateId);

}  // namespace dashboard
