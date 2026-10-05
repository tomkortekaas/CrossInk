#ifdef SIMULATOR
#include "SimulatorLibraryTest.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdlib>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/ActivityManager.h"
#include "activities/library/LibraryActivity.h"
#include "components/UITheme.h"

extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

namespace {
[[noreturn]] void fail(const char* message) {
  LOG_ERR("LIBTEST", "%s", message);
  std::_Exit(2);
}
void assertActivity(const char* name) {
  if (!activityManager.isCurrentActivityNamed(name)) fail(name);
}
LibraryActivity& currentLibrary() {
  assertActivity("Library");
  return *static_cast<LibraryActivity*>(activityManager.simulatorCurrentActivity());
}
void render(const char* message) {
  if (activityManager.requestUpdateAndWait() != RequestUpdateResult::Rendered) fail("render rejected");
  LOG_INF("LIBTEST", "%s (%d x %d)", message, renderer.getScreenWidth(), renderer.getScreenHeight());
}
}  // namespace

bool runSimulatorLibraryTestTick() {
  if (!std::getenv("CROSSINK_SIMULATOR_LIBRARY_TEST")) return false;
  static int step = 0;
  static int settle = 30;
  mappedInputManager.simulatorClearInputFrame();
  if (settle-- > 0) return true;
  settle = 30;
  switch (step++) {
    case 0: {
      RenderLock lock;
      SETTINGS.uiTheme = std::getenv("CROSSINK_SIMULATOR_LIBRARY_DASHBOARD") ? CrossPointSettings::DASHBOARD
                                                                        : CrossPointSettings::COVER_GRID;
      SETTINGS.recentBooksView = CrossPointSettings::RECENT_BOOKS_GRID;
      SETTINGS.librarySortMethod = 4;
      UITheme::getInstance().reload();
      const bool empty = std::getenv("CROSSINK_SIMULATOR_LIBRARY_EMPTY") != nullptr;
      if (!empty) {
        for (int i = 0; i < 12; ++i) {
          const std::string path = "/books/book" + std::to_string(i) + ".epub";
          Epub book(path, "/.crosspoint");
          RECENT_BOOKS.addOrUpdateBook(path, "Library test " + std::to_string(i), "Test Author", book.getThumbBmpPath(),
                                     RecentBook::CoverState::Unknown);
        }
      }
      activityManager.goHome();
      break;
    }
    case 1:
      assertActivity("Home");
      render("Home rendered; dashboard/Cover Grid preserved");
      activityManager.goToLibrary();
      break;
    case 2:
      render("Library grid rendered; missing/malformed covers tolerated");
      if (currentLibrary().simulatorRowCount() != (std::getenv("CROSSINK_SIMULATOR_LIBRARY_EMPTY") ? 0 : 12))
        fail("recent grid row count");
      currentLibrary().simulatorSetView(1, false);
      break;
    case 3:
      render("Indexed title grid rendered");
      if (!currentLibrary().simulatorGridEnabled()) fail("full Library index has no cover grid");
      if (currentLibrary().simulatorRowCount() != (std::getenv("CROSSINK_SIMULATOR_LIBRARY_EMPTY") ? 0 : 24))
        fail("full Library index row count");
      currentLibrary().simulatorSetView(4, true);
      mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Back);
      settle = 2;
      break;
    case 4:
      mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Back);
      break;
    case 5:
      assertActivity("Home");
      render("Back returned to Home");
      activityManager.goToLibrary();
      break;
    case 6:
      assertActivity("Library");
      if (std::getenv("CROSSINK_SIMULATOR_LIBRARY_EMPTY")) {
        LOG_INF("LIBTEST", "Empty Library passed");
        std::_Exit(0);
      }
      mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Down);
      settle = 2;
      break;
    case 7:
      mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Down);
      break;
    case 8:
      if (currentLibrary().simulatorSelection() != 6) fail("physical Down did not select next cover");
      mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Confirm);
      settle = 2;
      break;
    case 9:
      mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Confirm);
      break;
    case 10:
      assertActivity("EpubReader");
      render("Select opened book from Library grid");
      if (activityManager.getCurrentBookPath().empty()) fail("reader has no book path");
      mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Back);
      settle = 2;
      break;
    case 11:
      mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Back);
      break;
    case 12:
      assertActivity("Home");
      render("Reader Back returned to Home");
      activityManager.goToLibrary();
      break;
    case 13:
      currentLibrary().simulatorSetView(1, false, "Library test 2");
      break;
    case 14:
      if (currentLibrary().simulatorRowCount() != 4) fail("indexed search row count");
      mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Down);
      settle = 2;
      break;
    case 15:
      mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Down);
      break;
    case 16:
      if (currentLibrary().simulatorSelection() != 6) fail("indexed grid Down selection");
      mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Confirm);
      settle = 2;
      break;
    case 17:
      mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Confirm);
      break;
    case 18:
      assertActivity("EpubReader");
      if (activityManager.getCurrentBookPath() != "/books/book21.epub") fail("indexed grid opened wrong book");
      render("Indexed grid opened a book outside Recent Books");
      mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Back);
      settle = 2;
      break;
    case 19:
      mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Back);
      break;
    case 20:
      assertActivity("Home");
      LOG_INF("LIBTEST", "Library integration passed");
      std::_Exit(0);
  }
  return true;
}
#endif
