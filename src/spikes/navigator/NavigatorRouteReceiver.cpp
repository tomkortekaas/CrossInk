#if defined(CROSSINK_NAVIGATOR) && defined(ARDUINO)
#include "NavigatorRouteReceiver.h"
#include <Arduino.h>
#include <BLEAdvertising.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEService.h>
#include <cstring>

namespace navigator {
namespace {
constexpr char kService[] = "8c9f9d10-7c6d-4c8e-a2cb-49586da45d10";
constexpr char kWrite[] = "8c9f9d11-7c6d-4c8e-a2cb-49586da45d10";
constexpr char kStatus[] = "8c9f9d12-7c6d-4c8e-a2cb-49586da45d10";
BLEServer* server = nullptr;
BLECharacteristic* status = nullptr;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t queued[512], processing[512];
uint16_t queuedLength = 0;
uint32_t epoch = 0;
bool connected = false, disconnected = false, overflow = false;
bool open = false;
uint32_t lastActivity = 0, acceptedAt = 0;
bool accepted = false;
LiveNavigationSession live;

void identity() {
  uint8_t bytes[7] = {0x21, 0, 0, 0, 0, 0, 0};
  status->setValue(bytes, sizeof(bytes));
}
class ServerCallbacks final : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    portENTER_CRITICAL(&mux);
    connected = true;
    ++epoch;
    portEXIT_CRITICAL(&mux);
    // Readable identity, not a notification that can be lost before subscribe.
    identity();
  }
  void onDisconnect(BLEServer*) override {
    portENTER_CRITICAL(&mux);
    connected = false;
    disconnected = true;
    queuedLength = 0;
    overflow = false;
    ++epoch;
    portEXIT_CRITICAL(&mux);
  }
} serverCallbacks;
class WriteCallbacks final : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    const size_t length = characteristic->getLength();
    const uint8_t* data = characteristic->getData();
    portENTER_CRITICAL(&mux);
    if (!data || !length || length > sizeof(queued) || queuedLength) {
      overflow = true;
    } else {
      std::memcpy(queued, data, length);
      queuedLength = length;
    }
    portEXIT_CRITICAL(&mux);
  }
} writeCallbacks;
}

bool beginRouteReceiver() {
  // Do not advertise a second connection while the previous link is closing.
  if (server && server->getConnectedCount()) return false;
  if (!server) {
    if (!BLEDevice::init("X3-Navigator")) return false;
    server = BLEDevice::createServer();
    if (!server) return false;
    server->setCallbacks(&serverCallbacks);
    server->advertiseOnDisconnect(false); // only poll owns the reception window
    auto* service = server->createService(kService);
    auto* writable = service->createCharacteristic(kWrite, BLECharacteristic::PROPERTY_WRITE);
    status = service->createCharacteristic(kStatus, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    writable->setCallbacks(&writeCallbacks);
    service->start();
    server->getAdvertising()->addServiceUUID(kService);
    server->getAdvertising()->setScanResponse(false);
  }
  identity();
  live.disconnect();
  accepted = false;
  open = true;
  lastActivity = millis();
  server->getAdvertising()->start();
  return true;
}

void stopRouteReceiver() {
  open = false;
  live.disconnect();
  if (!server) return;
  server->getAdvertising()->stop();
  // Keep stack objects alive: existing SDK teardown is unsafe with a live link.
  if (server->getConnectedCount()) server->disconnect(server->getConnId());
}

bool routeReceiverOpen() { return open; }
bool navigationSessionActive() { return live.active(); }
bool navigationPosition(uint32_t now, LivePosition& out) { return live.position(now, out); }
bool takeNavigationForceRefresh() { return live.takeForceRefresh(); }

bool pollRouteReceiver(NavigationRouteSession& session, RouteTransferStatus& result) {
  bool wasDisconnected, bad, link;
  uint16_t length;
  uint32_t generation;
  portENTER_CRITICAL(&mux);
  wasDisconnected = disconnected;
  disconnected = false;
  bad = overflow;
  overflow = false;
  length = queuedLength;
  if (length) std::memcpy(processing, queued, length);
  queuedLength = 0;
  generation = epoch;
  link = connected;
  portEXIT_CRITICAL(&mux);
  if (wasDisconnected) {
    live.disconnect();
    session.disconnect();
    if (open && !accepted && !link) {
      identity();
      server->getAdvertising()->start();
    }
  }
  if (!open) return false;
  const uint32_t now = millis();
  const bool wasLive = live.active();
  live.expire(now);
  const bool liveFrame = length && (processing[0] == 0x08 || processing[0] == 0x09 || processing[0] == 0x0B);
  // A queued START can have arrived during the slow initial e-paper refresh.
  // Process it before applying the route COMMIT grace timeout.
  if ((wasLive && !live.active()) ||
      (accepted && !liveFrame && !live.active() && uint32_t(now - acceptedAt) >= 1200) ||
      (!live.active() && uint32_t(now - lastActivity) >= 180000)) {
    stopRouteReceiver();
    session.disconnect();
    return false;
  }
  if (!link || (!length && !bad)) return false;
  if (accepted && !liveFrame) return false; // only live START may follow COMMIT
  lastActivity = now;
  if (!bad && liveFrame) {
    const auto reply = live.receive(processing, length, session.hasRoute() ? session.index().routeId : 0,
                                    session.transferring(), now);
    portENTER_CRITICAL(&mux);
    const bool samePeer = connected && epoch == generation;
    portEXIT_CRITICAL(&mux);
    if (samePeer) {
      uint8_t bytes[7];
      reply.writeEnvelope(bytes);
      status->setValue(bytes, sizeof(bytes));
      status->notify();
    } else {
      live.disconnect();
    }
    if (reply.code == LiveNavigationCode::Ready && samePeer) {
      accepted = false;
      server->getAdvertising()->stop();
    } else if (reply.code == LiveNavigationCode::Stopped) {
      accepted = true;
      acceptedAt = now;
    }
    return false; // live receipt alone never requests a display refresh
  }
  if (bad) {
    live.disconnect();
    result = {RouteTransferCode::RouteInvalid, 0, 0};
    session.disconnect();
  } else if (live.active()) {
    // End a walk before replacing its authoritative stored route.
    result = {RouteTransferCode::RouteInvalid, 0, 0};
  } else {
    result = session.receive(processing, length);
  }
  portENTER_CRITICAL(&mux);
  const bool samePeer = connected && epoch == generation;
  portEXIT_CRITICAL(&mux);
  if (samePeer) {
    uint8_t bytes[7];
    result.writeEnvelope(bytes);
    status->setValue(bytes, sizeof(bytes));
    status->notify();
  }
  if (result.code == RouteTransferCode::RouteAccepted) {
    accepted = true;
    acceptedAt = millis();
    server->getAdvertising()->stop();
  }
  return true;
}
}  // namespace navigator
#endif
