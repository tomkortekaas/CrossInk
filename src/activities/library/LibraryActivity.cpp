#include "LibraryActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "LibraryIndexStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/home/BookActions.h"
#include "activities/home/FileBrowserActionActivity.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/reader/ReadingTimeEstimate.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kCoverCornerRadius = 2;
constexpr int kSelectionPadding = 4;
constexpr int kSelectionOuterInset = kSelectionPadding + 2;
constexpr int kProgressBarHeight = 4;
constexpr int kLabelGap = 6;
constexpr unsigned long kLongPressMs = 1000;

static_assert(static_cast<uint8_t>(LibrarySortMode::InProgressFirst) == CrossPointSettings::LIBRARY_SORT_IN_PROGRESS);
static_assert(static_cast<uint8_t>(LibrarySortMode::RecentlyAdded) == CrossPointSettings::LIBRARY_SORT_RECENTLY_ADDED);
static_assert(static_cast<uint8_t>(LibrarySortMode::Title) == CrossPointSettings::LIBRARY_SORT_TITLE);
static_assert(static_cast<uint8_t>(LibrarySortMode::Author) == CrossPointSettings::LIBRARY_SORT_AUTHOR);
static_assert(kLibrarySortModeCount == CrossPointSettings::LIBRARY_SORT_COUNT);

LibrarySortMode currentSortMode() {
  const uint8_t raw = SETTINGS.librarySortMode;
  return raw < kLibrarySortModeCount ? static_cast<LibrarySortMode>(raw) : LibrarySortMode::InProgressFirst;
}

std::string thumbPathFor(const std::string& path, const int width, const int height) {
  if (FsHelpers::hasEpubExtension(path)) return Epub(path, "/.crosspoint").getThumbBmpPath(width, height);
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crosspoint").getThumbBmpPath(static_cast<uint16_t>(width), static_cast<uint16_t>(height));
  }
  return "";
}

// Small drawn check mark; avoids depending on a ✓ glyph in the UI font.
void drawCheckMark(const GfxRenderer& renderer, const int x, const int y, const int size) {
  renderer.drawLine(x, y + size / 2, x + size / 3, y + size, 2, true);
  renderer.drawLine(x + size / 3, y + size, x + size, y, 2, true);
}
}  // namespace

void LibraryActivity::onEnter() {
  Activity::onEnter();
  selectorIndex = 0;
  preparedPage = NO_PAGE;
  shownPage = NO_PAGE;
  pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  indexReady = false;
  longPressFired = false;
  requestUpdate();
}

void LibraryActivity::onExit() {
  Activity::onExit();
  index.entries.clear();
  index.entries.shrink_to_fit();
  for (auto& state : pageState) state = PageBookState{};
}

void LibraryActivity::refreshIndex() {
  LOG_INF("LIB", "refresh start free=%u", static_cast<unsigned>(ESP.getFreeHeap()));
  LibraryIndexStore::load(index);  // on failure `index` is empty and gets rebuilt below
  std::vector<std::string> paths;
  paths.reserve(index.entries.size() + 16);
  LibraryIndexStore::scanBookPaths(paths);
  bool changed = LibraryIndexCodec::mergeScannedPaths(index, std::move(paths));
  // mergeScannedPaths only moves the new paths out; free the rest before the heap-heavy metadata pass.
  paths.clear();
  paths.shrink_to_fit();

  int missing = 0;
  for (const auto& entry : index.entries) {
    if (!entry.metadataLoaded) ++missing;
  }
  if (missing > 0) {
    const Rect popup = GUI.drawPopup(renderer, tr(STR_LIBRARY_UPDATING));
    int done = 0;
    int lastShown = -1;
    const unsigned long start = millis();
    for (auto& entry : index.entries) {
      if (entry.metadataLoaded) continue;
      LibraryIndexStore::loadMetadata(entry);
      ++done;
      if (done % 10 == 0) {
        LOG_INF("LIB", "metadata %d/%d elapsed=%lums free=%u maxAlloc=%u", done, missing, millis() - start,
                static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      }
      const int percent = (done * 100) / missing;
      if (percent / 5 != lastShown / 5) {  // every 5 %: each popup update is a panel refresh
        GUI.fillPopupProgress(renderer, popup, percent);
        lastShown = percent;
      }
    }
    LOG_INF("LIB", "metadata done %d elapsed=%lums free=%u maxAlloc=%u", done, millis() - start,
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    changed = true;
  }

  applyRecentStatus();
  if (changed && !LibraryIndexStore::save(index)) {
    LOG_ERR("LIB", "Failed to save library index");
  }
  resort();
  LOG_INF("LIB", "refresh done books=%u free=%u maxAlloc=%u", static_cast<unsigned>(index.entries.size()),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

void LibraryActivity::applyRecentStatus() {
  const auto& recents = RECENT_BOOKS.getBooks();
  for (auto& entry : index.entries) {
    entry.recentRank = -1;
    for (size_t r = 0; r < recents.size(); ++r) {
      if (recents[r].path == entry.path) {
        entry.recentRank = static_cast<int16_t>(r);
        break;
      }
    }
    // Books outside the recents keep their persisted status (an older half-read book stays "in progress").
    if (entry.recentRank >= 0) {
      entry.status = LibrarySort::deriveStatus(true, BookActions::isBookCompleted(entry.path));
    }
  }
}

void LibraryActivity::resort() {
  const int count = static_cast<int>(index.entries.size());
  const std::string selectedPath =
      (selectorIndex >= 0 && selectorIndex < count) ? index.entries[selectorIndex].path : std::string();
  LibrarySort::sort(index.entries, currentSortMode());
  selectorIndex = 0;
  for (int i = 0; i < count; ++i) {
    if (index.entries[i].path == selectedPath) {
      selectorIndex = i;
      break;
    }
  }
  preparedPage = NO_PAGE;
}

bool LibraryActivity::generateThumb(LibraryEntry& entry, const int width, const int height) {
  if (FsHelpers::hasEpubExtension(entry.path)) {
    Epub epub(entry.path, "/.crosspoint");
    if (!epub.load(true, true, Epub::XLocationLoadMode::Skip)) {
      LOG_ERR("LIB", "EPUB load failed for thumbnail: %s", entry.path.c_str());
      return false;
    }
    if (epub.generateThumbBmp(width, height, &renderer, SETTINGS.getReaderFontId())) return true;
    LOG_ERR("LIB", "Thumbnail generation failed: %s", entry.path.c_str());
    if (!epub.hasCoverImage()) entry.coverMissing = true;
    return false;
  }
  if (FsHelpers::hasXtcExtension(entry.path)) {
    Xtc xtc(entry.path, "/.crosspoint");
    if (!xtc.load()) {
      LOG_ERR("LIB", "XTC load failed for thumbnail: %s", entry.path.c_str());
      return false;
    }
    if (xtc.generateThumbBmp(static_cast<uint16_t>(width), static_cast<uint16_t>(height))) return true;
    LOG_ERR("LIB", "Thumbnail generation failed: %s", entry.path.c_str());
    return false;
  }
  return false;
}

void LibraryActivity::preparePage(const int page, const LibraryGrid::Layout& layout) {
  const int pageStart = page * LibraryGrid::kBooksPerPage;
  const int count = std::min(LibraryGrid::kBooksPerPage, static_cast<int>(index.entries.size()) - pageStart);
  bool showingPopup = false;
  Rect popupRect;
  bool indexDirty = false;

  for (auto& state : pageState) state = PageBookState{};
  for (int i = 0; i < count; ++i) {
    LibraryEntry& entry = index.entries[pageStart + i];
    PageBookState& state = pageState[i];

    if (!entry.coverMissing) {
      std::string thumb = thumbPathFor(entry.path, layout.coverWidth, layout.coverHeight);
      if (!thumb.empty() && !Storage.exists(thumb.c_str())) {
        if (!showingPopup) {
          showingPopup = true;
          popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
        }
        GUI.fillPopupProgress(renderer, popupRect, 10 + (i * 90) / count);
        const bool wasMissing = entry.coverMissing;
        if (!generateThumb(entry, layout.coverWidth, layout.coverHeight)) thumb.clear();
        if (entry.coverMissing != wasMissing) indexDirty = true;
      }
      state.thumbPath = std::move(thumb);
    }

    if (FsHelpers::hasEpubExtension(entry.path)) {
      RecentBook book;
      book.path = entry.path;
      state.progress = RecentBookProgress::loadCachedEpubPercent(book);  // never opens the EPUB
    } else if (entry.status != LibraryBookStatus::New) {
      RecentBook book;
      book.path = entry.path;
      state.progress = RecentBookProgress::loadPercent(book);
    }

    const std::string cachePath = BookActions::bookStatsCachePath(entry.path);
    if (!cachePath.empty() && Storage.exists(cachePath.c_str())) {
      const BookReadingStats stats = BookReadingStats::load(cachePath);
      if (stats.isCompleted && entry.status != LibraryBookStatus::Finished) {
        entry.status = LibraryBookStatus::Finished;  // finished long ago, no longer in the recents
        indexDirty = true;
      }
      state.hasSecondsLeft = ReadingTimeEstimate::secondsLeft(stats.estimatedTimeLeftSeconds, stats.totalReadingSeconds,
                                                              state.progress, state.secondsLeft);
    }
  }
  if (indexDirty && !LibraryIndexStore::save(index)) {
    LOG_ERR("LIB", "Failed to save library index");
  }
  if (showingPopup) shownPage = NO_PAGE;  // the popup was drawn over the page: redraw counts in the refresh cycle
  preparedPage = page;
}

void LibraryActivity::drawCover(const LibraryEntry& entry, const PageBookState& state,
                                const LibraryGrid::Rect& r) const {
  if (!state.thumbPath.empty()) {
    FsFile file;
    if (Storage.openFileForRead("LIB", state.thumbPath, file)) {
      Bitmap bmp(file);
      const bool ok = bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() > 0 && bmp.getHeight() > 0;
      if (ok) {
        renderer.fillRoundedRect(r.x, r.y, r.w, r.h, kCoverCornerRadius, Color::White);
        renderer.drawBitmap(bmp, r.x, r.y, r.w, r.h, 0.0f, 0.0f);
        renderer.maskRoundedRectOutsideCorners(r.x, r.y, r.w, r.h, kCoverCornerRadius, Color::White);
        renderer.drawRoundedRect(r.x, r.y, r.w, r.h, 2, kCoverCornerRadius, true);
      }
      file.close();
      if (ok) return;
    }
  }

  // Text cover: title large and bold in the upper third, author below.
  renderer.fillRoundedRect(r.x, r.y, r.w, r.h, kCoverCornerRadius, Color::White);
  renderer.drawRoundedRect(r.x, r.y, r.w, r.h, 2, kCoverCornerRadius, true);
  const int pad = 14;
  const int maxWidth = r.w - 2 * pad;
  const int titleLh = renderer.getLineHeight(UI_12_FONT_ID);
  const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, entry.title.c_str(), maxWidth, 5, EpdFontFamily::BOLD);
  int y = r.y + r.h / 3 - (static_cast<int>(titleLines.size()) * titleLh) / 2;
  for (const auto& line : titleLines) {
    const int w = renderer.getTextWidth(UI_12_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, r.x + (r.w - w) / 2, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += titleLh;
  }
  if (!entry.author.empty()) {
    y += titleLh / 2;
    const int authorLh = renderer.getLineHeight(UI_10_FONT_ID);
    for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, entry.author.c_str(), maxWidth, 2)) {
      const int w = renderer.getTextWidth(UI_10_FONT_ID, line.c_str());
      renderer.drawText(UI_10_FONT_ID, r.x + (r.w - w) / 2, y, line.c_str());
      y += authorLh;
    }
  }
}

void LibraryActivity::drawProgressRow(const LibraryEntry& entry, const PageBookState& state,
                                      const LibraryGrid::Rect& r) const {
  // Start below the selection frame, which reaches kSelectionOuterInset under the cover.
  const int barY = r.y + kSelectionOuterInset + 5;
  const int lh = renderer.getLineHeight(UI_10_FONT_ID);
  const int textY = barY + kProgressBarHeight / 2 - lh / 2;
  const bool finished = entry.status == LibraryBookStatus::Finished;
  const bool hasPercent = RecentBookProgress::hasPercent(state.progress);

  char label[8] = "";
  if (!finished && hasPercent) snprintf(label, sizeof(label), "%d%%", static_cast<int>(state.progress + 0.5f));
  const int checkSize = 10;
  const int labelWidth =
      finished ? checkSize + kLabelGap : (label[0] ? renderer.getTextWidth(UI_10_FONT_ID, label) + kLabelGap : 0);
  const int barWidth = r.w - labelWidth;

  renderer.drawRect(r.x, barY, barWidth, kProgressBarHeight, true);
  const float fraction = finished ? 1.0f : (hasPercent ? std::clamp(state.progress / 100.0f, 0.0f, 1.0f) : 0.0f);
  const int fillWidth = static_cast<int>(static_cast<float>(barWidth) * fraction);
  if (fillWidth > 0) renderer.fillRect(r.x, barY, fillWidth, kProgressBarHeight, true);

  if (finished) {
    drawCheckMark(renderer, r.x + barWidth + kLabelGap, barY + kProgressBarHeight / 2 - checkSize / 2, checkSize);
  } else if (label[0]) {
    renderer.drawText(UI_10_FONT_ID, r.x + barWidth + kLabelGap, textY, label);
  }
}

void LibraryActivity::drawFooter(const LibraryGrid::Layout& layout) const {
  if (selectorIndex < 0 || selectorIndex >= static_cast<int>(index.entries.size())) return;
  const LibraryEntry& entry = index.entries[selectorIndex];
  const int page = selectorIndex / LibraryGrid::kBooksPerPage;
  const PageBookState& state = pageState[selectorIndex % LibraryGrid::kBooksPerPage];

  char suffix[48] = "";
  if (page == preparedPage && state.hasSecondsLeft && entry.status != LibraryBookStatus::Finished) {
    char duration[24];
    ReadingTimeEstimate::formatCompact(state.secondsLeft, tr(STR_STATS_LESS_THAN_MIN), duration, sizeof(duration));
    char timeLeft[40];
    snprintf(timeLeft, sizeof(timeLeft), tr(STR_LIBRARY_TIME_LEFT), duration);
    snprintf(suffix, sizeof(suffix), "  \xC2\xB7  %s", timeLeft);  // " · nog 3h 40m"
  }
  const int suffixWidth = suffix[0] ? renderer.getTextWidth(UI_10_FONT_ID, suffix) : 0;
  const std::string title =
      renderer.truncatedText(UI_10_FONT_ID, entry.title.c_str(), layout.footer.w - suffixWidth, EpdFontFamily::BOLD);
  const int titleWidth = renderer.getTextWidth(UI_10_FONT_ID, title.c_str(), EpdFontFamily::BOLD);
  const int y = layout.footer.y + (layout.footer.h - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  renderer.drawText(UI_10_FONT_ID, layout.footer.x, y, title.c_str(), true, EpdFontFamily::BOLD);
  if (suffix[0]) renderer.drawText(UI_10_FONT_ID, layout.footer.x + titleWidth, y, suffix);
}

void LibraryActivity::render(RenderLock&&) {
  if (!indexReady) {
    refreshIndex();
    indexReady = true;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto layout =
      LibraryGrid::compute(renderer.getScreenWidth(), renderer.getScreenHeight(), metrics.buttonHintsHeight);
  const int total = static_cast<int>(index.entries.size());
  const int page = total > 0 ? selectorIndex / LibraryGrid::kBooksPerPage : 0;
  if (total > 0 && preparedPage != page) preparePage(page, layout);  // may show a cover-generation popup

  renderer.clearScreen();
  if (total == 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_LIBRARY_EMPTY));
  } else {
    const int pageStart = page * LibraryGrid::kBooksPerPage;
    const int count = std::min(LibraryGrid::kBooksPerPage, total - pageStart);
    for (int i = 0; i < count; ++i) {
      const LibraryEntry& entry = index.entries[pageStart + i];
      const LibraryGrid::Rect& cover = layout.covers[i];
      drawCover(entry, pageState[i], cover);
      drawProgressRow(entry, pageState[i], layout.bars[i]);
      if (pageStart + i == selectorIndex) {
        renderer.drawRoundedRect(cover.x - kSelectionPadding, cover.y - kSelectionPadding,
                                 cover.w + kSelectionPadding * 2, cover.h + kSelectionPadding * 2, 3,
                                 kCoverCornerRadius + kSelectionPadding, true);
        renderer.drawRoundedRect(cover.x - kSelectionOuterInset, cover.y - kSelectionOuterInset,
                                 cover.w + kSelectionOuterInset * 2, cover.h + kSelectionOuterInset * 2, 1,
                                 kCoverCornerRadius + kSelectionOuterInset, true);
      }
    }
    drawFooter(layout);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (shownPage == page) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);  // selection moved on the same page
  } else {
    // New page or redraw after an overlay: count it like a reader page turn, so the X3's
    // full resync (HALF_REFRESH) only runs every N turns instead of on every page.
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  shownPage = page;
}

void LibraryActivity::loop() {
  if (!indexReady) return;  // index is still being built on the render task
  const int count = static_cast<int>(index.entries.size());
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) longPressFired = false;
    return;
  }
  if (count > 0 && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= kLongPressMs) {
    longPressFired = true;
    showBookActionMenu(selectorIndex);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (count > 0) onSelectBook(index.entries[selectorIndex].path);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }
  if (count == 0) return;

  auto moveTo = [this](const int next) {
    if (next == selectorIndex) return;
    selectorIndex = next;
    requestUpdate();
  };
  const int perPage = LibraryGrid::kBooksPerPage;
  buttonNavigator.onRelease({MappedInputManager::Button::Right},
                            [&] { moveTo(ButtonNavigator::nextIndex(selectorIndex, count)); });
  buttonNavigator.onRelease({MappedInputManager::Button::Left},
                            [&] { moveTo(ButtonNavigator::previousIndex(selectorIndex, count)); });
  buttonNavigator.onRelease({MappedInputManager::Button::Down},
                            [&] { moveTo(ButtonNavigator::nextPageIndex(selectorIndex, count, perPage)); });
  buttonNavigator.onRelease({MappedInputManager::Button::Up},
                            [&] { moveTo(ButtonNavigator::previousPageIndex(selectorIndex, count, perPage)); });
  buttonNavigator.onContinuous({MappedInputManager::Button::Right},
                               [&] { moveTo(ButtonNavigator::nextIndex(selectorIndex, count)); });
  buttonNavigator.onContinuous({MappedInputManager::Button::Left},
                               [&] { moveTo(ButtonNavigator::previousIndex(selectorIndex, count)); });
}

void LibraryActivity::showSortMenu() {
  std::vector<std::string> options = {tr(STR_LIBRARY_SORT_IN_PROGRESS), tr(STR_LIBRARY_SORT_RECENTLY_ADDED),
                                      tr(STR_LIBRARY_SORT_TITLE), tr(STR_LIBRARY_SORT_AUTHOR)};
  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "LibrarySortSelect", StrId::STR_LIBRARY_SORT,
                                                std::move(options), static_cast<uint8_t>(currentSortMode())),
      [this](const ActivityResult& result) {
        shownPage = NO_PAGE;  // an overlay was drawn: redraw counts in the refresh cycle
        if (result.isCancelled) return;
        const auto* selection = std::get_if<OptionSelectionResult>(&result.data);
        if (selection == nullptr || selection->index >= kLibrarySortModeCount) return;
        SETTINGS.librarySortMode = selection->index;
        SETTINGS.saveToFile();
        resort();
        requestUpdate(true);
      });
}

void LibraryActivity::removeEntry(const std::string& path) {
  auto& entries = index.entries;
  entries.erase(
      std::remove_if(entries.begin(), entries.end(), [&path](const LibraryEntry& e) { return e.path == path; }),
      entries.end());
  if (!LibraryIndexStore::save(index)) LOG_ERR("LIB", "Failed to save library index");
  if (selectorIndex >= static_cast<int>(entries.size()))
    selectorIndex = std::max(0, static_cast<int>(entries.size()) - 1);
  preparedPage = NO_PAGE;
  shownPage = NO_PAGE;
}

void LibraryActivity::showBookActionMenu(const int bookIndex) {
  if (bookIndex < 0 || bookIndex >= static_cast<int>(index.entries.size())) return;
  const LibraryEntry book = index.entries[bookIndex];

  // The library offers the actions that make sense when choosing a book, plus sorting.
  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.reserve(4);  // up to 3 filtered actions + SortLibrary
  for (const auto& item : BookActions::buildBookActionItems(book.path, /*includeRemoveFromRecents=*/false)) {
    if (item.action == FileBrowserAction::ToggleCompleted || item.action == FileBrowserAction::DeleteCache ||
        item.action == FileBrowserAction::Delete) {
      items.push_back(item);
    }
  }
  items.push_back({FileBrowserAction::SortLibrary, StrId::STR_LIBRARY_SORT});

  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, book.title, std::move(items), true),
      [this, book](const ActivityResult& result) {
        shownPage = NO_PAGE;  // a menu/toast/confirmation was drawn: redraw counts in the refresh cycle
        longPressFired = false;
        if (result.isCancelled) return;
        const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
        if (!actionResult) {
          LOG_ERR("LIB", "Book action result missing");
          return;
        }
        switch (static_cast<FileBrowserAction>(actionResult->action)) {
          case FileBrowserAction::SortLibrary:
            showSortMenu();
            return;
          case FileBrowserAction::ToggleCompleted: {
            bool completed = false;
            if (BookActions::toggleBookCompleted(book.path, book.title, completed)) {
              BookActions::drawToast(renderer, completed ? tr(STR_MARKED_FINISHED) : tr(STR_MARKED_UNFINISHED));
              delay(1000);
              for (auto& entry : index.entries) {
                if (entry.path != book.path) continue;
                entry.status =
                    completed ? LibraryBookStatus::Finished : LibrarySort::deriveStatus(entry.recentRank >= 0, false);
              }
              if (!LibraryIndexStore::save(index)) LOG_ERR("LIB", "Failed to save library index");
            }
            preparedPage = NO_PAGE;
            requestUpdate(true);
            return;
          }
          case FileBrowserAction::DeleteCache:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(
                    renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_CACHE), book.title),
                [this, book](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled && !BookActions::clearBookCache(book.path)) {
                    LOG_ERR("LIB", "Failed to clear book cache for: %s", book.path.c_str());
                  }
                  preparedPage = NO_PAGE;  // thumbnails lived in the cache; regenerate
                  requestUpdate(true);
                });
            return;
          case FileBrowserAction::Delete:
            startActivityForResult(std::make_unique<ConfirmationActivity>(
                                       renderer, mappedInput, tr(STR_DELETE) + std::string("? "), book.title),
                                   [this, book](const ActivityResult& confirmation) {
                                     if (confirmation.isCancelled) return;
                                     BookActions::clearFileMetadata(book.path);
                                     if (!Storage.remove(book.path.c_str())) {
                                       LOG_ERR("LIB", "Failed to delete file: %s", book.path.c_str());
                                       return;
                                     }
                                     RECENT_BOOKS.removeByPath(book.path);
                                     removeEntry(book.path);
                                     requestUpdate(true);
                                   });
            return;
          default:
            return;
        }
      });
}
