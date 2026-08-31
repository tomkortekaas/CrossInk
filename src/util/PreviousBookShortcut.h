#pragma once

#include <cstdint>
#include <string>

// Release-triggered so the destination reader never receives the held gesture.
// A fresh, unchorded press is required, including after wake or activity changes.
class PreviousBookGesture {
 public:
  enum class Result { None, PageTurn, SwitchBook };
  Result update(bool enabled, bool pressed, bool held, bool released, bool otherButton, uint32_t now) {
    if (!enabled || otherButton) {
      armed = false;
      return Result::None;
    }
    if (pressed) {
      armed = true;
      startedAt = now;
    }
    if (released) {
      const bool wasArmed = armed;
      armed = false;
      if (wasArmed) return static_cast<uint32_t>(now - startedAt) >= 700 ? Result::SwitchBook : Result::PageTurn;
    } else if (!held) {
      armed = false;
    }
    return Result::None;
  }

 private:
  uint32_t startedAt = 0;
  bool armed = false;
};

// Recents are ordered newest first. Copy the selected path before an activity
// transition can reorder or prune that store; no second document is kept open.
template <typename Books, typename IsReadable>
std::string previousBookPath(const Books& books, const std::string& currentPath, IsReadable isReadable) {
  for (const auto& book : books) {
    if (!book.path.empty() && book.path != currentPath && isReadable(book.path)) return book.path;
  }
  return {};
}
