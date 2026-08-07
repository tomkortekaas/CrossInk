#pragma once

inline bool shouldRebuildSettingsListsAfterOptionSelection(const bool sleepScreenChanged) {
  return !sleepScreenChanged;
}
