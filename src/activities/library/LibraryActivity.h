#pragma once

#include <string>

#include "../Activity.h"
#include "LibraryGridLayout.h"
#include "LibraryIndexCodec.h"
#include "util/ButtonNavigator.h"

// All books on the SD card as a 2x2 grid of large covers (spec:
// docs/superpowers/specs/2026-10-05-x3-library-design.md).
class LibraryActivity final : public Activity {
 public:
  explicit LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Library", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct PageBookState {
    std::string thumbPath;  // empty -> draw a text cover
    float progress = -1.0f;
    uint32_t secondsLeft = 0;
    bool hasSecondsLeft = false;
  };
  static constexpr int NO_PAGE = -1;

  ButtonNavigator buttonNavigator;
  LibraryIndexData index;
  PageBookState pageState[LibraryGrid::kBooksPerPage];
  int selectorIndex = 0;
  int preparedPage = NO_PAGE;  // page whose thumbnails/progress are loaded
  int shownPage = NO_PAGE;     // page currently on the panel, to pick FAST vs HALF refresh
  bool indexReady = false;
  bool longPressFired = false;

  void refreshIndex();
  void applyRecentStatus();
  void resort();
  void preparePage(int page, const LibraryGrid::Layout& layout);
  bool generateThumb(LibraryEntry& entry, int width, int height);
  void drawCover(const LibraryEntry& entry, const PageBookState& state, const LibraryGrid::Rect& rect) const;
  void drawProgressRow(const LibraryEntry& entry, const PageBookState& state, const LibraryGrid::Rect& rect) const;
  void drawFooter(const LibraryGrid::Layout& layout) const;
  void showBookActionMenu(int bookIndex);
  void showSortMenu();
  void removeEntry(const std::string& path);
};
