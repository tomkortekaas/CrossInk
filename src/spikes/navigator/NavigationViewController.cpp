#include "NavigationViewController.h"

#include <InputManager.h>

// Centralized released-button -> semantic-action mapping for the X3 walking
// navigator. NavigatorMain feeds the actual FreeInk InputManager::BTN_*
// release indices here, so remapping physical controls later touches this one
// function (and its host tests) and nothing else.
namespace navigator {

NavigatorAction mapNavigatorButton(uint8_t releasedButton) {
  switch (releasedButton) {
    case InputManager::BTN_UP:
      return NavigatorAction::SelectGpsZoom;
    case InputManager::BTN_DOWN:
      return NavigatorAction::SelectOverview;
    case InputManager::BTN_CONFIRM:
      return NavigatorAction::ManualRefresh;
    case InputManager::BTN_BACK:
      return NavigatorAction::ReturnToDashboard;
    default:
      return NavigatorAction::None;
  }
}

}  // namespace navigator
