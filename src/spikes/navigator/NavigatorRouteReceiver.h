#pragma once
#include "route/NavigationRouteSession.h"
#include "LiveNavigationSession.h"

namespace navigator {
// BLE callbacks enqueue only; all card IO and route validation run in poll().
bool beginRouteReceiver();
void stopRouteReceiver();
bool routeReceiverOpen();
bool navigationSessionActive();
bool navigationPosition(uint32_t now, LivePosition& out);
bool takeNavigationForceRefresh();
// Active live session's routine refresh mode; Economical while none is active
// (the session fails closed on disconnect, stop and expiry).
WalkingRefreshMode navigationRefreshMode();
// Returns true only when a status was processed; caller decides if UI changes.
bool pollRouteReceiver(NavigationRouteSession& session, RouteTransferStatus& result);
}
