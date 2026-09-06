#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <vector>

#include "LiveNavigationSession.h"
using namespace navigator;
void put(std::vector<uint8_t>& b, int o, uint32_t v, int n = 4) {
  for (int i = 0; i < n; ++i) b[o + i] = v >> (8 * i);
}
std::vector<uint8_t> start(uint32_t route = 7, uint32_t session = 9) {
  std::vector<uint8_t> b(10);
  b[0] = 8;
  b[1] = 1;
  put(b, 2, route);
  put(b, 6, session);
  return b;
}
std::vector<uint8_t> startV2(uint32_t route = 7, uint32_t session = 9, uint8_t mode = 2) {
  std::vector<uint8_t> b(11);
  b[0] = 8;
  b[1] = 2;
  put(b, 2, route);
  put(b, 6, session);
  b[10] = mode;
  return b;
}
std::vector<uint8_t> fix(uint16_t seq = 0, uint32_t session = 9) {
  std::vector<uint8_t> b(20);
  b[0] = 9;
  put(b, 1, session);
  put(b, 5, seq, 2);
  put(b, 7, 523700000);
  put(b, 11, 49000000);
  put(b, 15, 5, 2);
  return b;
}
int main() {
  LiveNavigationSession s;
  LivePosition p;
  auto a = start();
  auto send = [&](const std::vector<uint8_t>& b, uint32_t now = 100) {
    return s.receive(b.data(), b.size(), 7, false, now);
  };
  assert(!s.position(0, p));
  assert(send(fix()).code == LiveNavigationCode::Invalid);
  assert(send(start(8)).code == LiveNavigationCode::Invalid);
  assert(!s.active());
  assert(s.receive(a.data(), a.size(), 7, true, 100).code == LiveNavigationCode::Invalid);
  assert(send(a).code == LiveNavigationCode::Ready);
  assert(s.active());
  assert(send(start(7, 10)).code == LiveNavigationCode::Invalid);
  auto f = fix(65535);
  assert(send(f, 200).code == LiveNavigationCode::FixAccepted);
  assert(s.position(200, p));
  assert(p.latitudeE7 == 523700000);
  assert(send(f, 5000).code == LiveNavigationCode::FixAccepted);
  assert(!s.position(30201, p));  // replay does not renew freshness
  auto altered = f;
  altered[7]++;
  assert(send(altered, 5001).code == LiveNavigationCode::Invalid);
  assert(send(fix(0), 5002).code == LiveNavigationCode::FixAccepted);
  assert(send(f, 5003).code == LiveNavigationCode::Invalid);
  assert(send(fix(1, 10), 5003).code == LiveNavigationCode::Invalid);
  for (auto field : {15, 17, 19}) {
    auto bad = fix(1);
    bad[field] = 255;
    if (field == 17) put(bad, 17, 15001, 2);
    assert(send(bad, 5004).code == LiveNavigationCode::Invalid);
  }
  auto old = fix(1);
  put(old, 17, 15000, 2);
  assert(send(old, 6000).code == LiveNavigationCode::FixAccepted);
  assert(s.position(21000, p));
  assert(!s.position(21001, p));
  std::vector<uint8_t> stop{11, 9, 0, 0, 0};
  assert(send(stop, 22000).code == LiveNavigationCode::Stopped);
  assert(!s.position(100, p));
  assert(send(stop, 22000).code == LiveNavigationCode::Stopped);
  assert(send(fix(2), 22001).code == LiveNavigationCode::Invalid);
  a = start(7, 11);
  assert(send(a).code == LiveNavigationCode::Ready);
  s.disconnect();
  assert(!s.active());
  assert(!s.position(100, p));
  a = start(7, 12);
  assert(send(a, 0xffffff00u).code == LiveNavigationCode::Ready);
  auto wrap = fix(0, 12);
  assert(send(wrap, 0xfffffff0u).code == LiveNavigationCode::FixAccepted);
  assert(s.position(20, p));
  s.expire(90000);
  assert(!s.active());
  // Live FIX flags byte: bit 0 = off-route, bit 1 = force refresh. Values 0..3
  // are accepted and survive position(); any other bit is rejected.
  LiveNavigationSession flags;
  auto sendFlags = [&](const std::vector<uint8_t>& b, uint32_t now = 100) {
    return flags.receive(b.data(), b.size(), 7, false, now);
  };
  auto startFlags = start(7, 13);
  assert(sendFlags(startFlags, 100).code == LiveNavigationCode::Ready);
  uint16_t seq = 1;
  for (uint8_t raw = 0; raw <= 3; ++raw) {
    auto f = fix(seq++, 13);
    f[19] = raw;
    assert(sendFlags(f, 100 + seq).code == LiveNavigationCode::FixAccepted);
    assert(flags.position(100 + seq, p));
    assert(p.offRoute == ((raw & 1) != 0));
    assert(flags.takeForceRefresh() == ((raw & 2) != 0));
    assert(!flags.takeForceRefresh());  // consumed exactly once
  }
  for (uint8_t raw : {4u, 8u, 0x80u, 255u}) {
    auto bad = fix(seq++, 13);
    bad[19] = raw;
    assert(sendFlags(bad, 100 + seq).code == LiveNavigationCode::Invalid);
  }
  auto good = fix(seq++, 13);
  good[19] = 2;  // force refresh without off-route
  assert(sendFlags(good, 100 + seq).code == LiveNavigationCode::FixAccepted);
  assert(flags.position(100 + seq, p));
  assert(!p.offRoute);
  assert(flags.takeForceRefresh());
  assert(!flags.takeForceRefresh());
  // Task 6 - WalkingRefreshMode: LIVE_START v1 (10 bytes, version 1) is the
  // legacy Economical session; LIVE_START v2 (11 bytes, version 2) names the
  // mode at byte 10 (0 balanced, 1 fast, 2 economical). A failed fresh START
  // must never partially activate or change the session, and the mode resets
  // to Economical when the session disconnects, stops or expires.
  LiveNavigationSession legacy;
  auto legacyStart = start(7, 70);
  assert(legacy.receive(legacyStart.data(), legacyStart.size(), 7, false, 100).code ==
         LiveNavigationCode::Ready);
  assert(legacy.active());
  assert(legacy.refreshMode() == WalkingRefreshMode::Economical);
  for (uint8_t m = 0; m <= 2; ++m) {
    LiveNavigationSession fresh;
    auto v2 = startV2(7, 30 + m, m);
    assert(fresh.receive(v2.data(), v2.size(), 7, false, 100).code == LiveNavigationCode::Ready);
    assert(fresh.active());
    assert(fresh.refreshMode() == static_cast<WalkingRefreshMode>(m));
  }
  // Unknown v2 mode values are rejected without activating the session.
  for (uint8_t m : {3u, 4u, 255u}) {
    LiveNavigationSession fresh;
    auto v2 = startV2(7, 40, m);
    assert(fresh.receive(v2.data(), v2.size(), 7, false, 100).code == LiveNavigationCode::Invalid);
    assert(!fresh.active());
    assert(fresh.refreshMode() == WalkingRefreshMode::Economical);
  }
  // Version/length mismatches are rejected without activating the session.
  LiveNavigationSession strict;
  auto v2NoMode = startV2(7, 41, 1);
  v2NoMode.resize(10);  // version 2 but legacy v1 length
  assert(strict.receive(v2NoMode.data(), v2NoMode.size(), 7, false, 100).code ==
         LiveNavigationCode::Invalid);
  auto v1WithMode = start(7, 42);
  v1WithMode.push_back(0);  // version 1 but v2 length
  assert(strict.receive(v1WithMode.data(), v1WithMode.size(), 7, false, 100).code ==
         LiveNavigationCode::Invalid);
  auto unknownVersion = startV2(7, 43, 1);
  unknownVersion[1] = 3;  // protocol version 3 is unknown
  assert(strict.receive(unknownVersion.data(), unknownVersion.size(), 7, false, 100).code ==
         LiveNavigationCode::Invalid);
  auto tooLong = startV2(7, 44, 1);
  tooLong.push_back(0);  // 12 bytes matches no live START length
  assert(strict.receive(tooLong.data(), tooLong.size(), 7, false, 100).code ==
         LiveNavigationCode::Invalid);
  assert(!strict.active());
  assert(strict.refreshMode() == WalkingRefreshMode::Economical);
  // An active session cannot be replaced by a fresh START and is never
  // partially changed: the invalid fresh frame leaves it active in Fast, and
  // the subsequent STOP resets the mode to Economical.
  LiveNavigationSession active;
  auto fastStart = startV2(7, 50, 1);
  assert(active.receive(fastStart.data(), fastStart.size(), 7, false, 100).code ==
         LiveNavigationCode::Ready);
  assert(active.refreshMode() == WalkingRefreshMode::Fast);
  auto invalidFresh = startV2(7, 51, 3);
  assert(active.receive(invalidFresh.data(), invalidFresh.size(), 7, false, 200).code ==
         LiveNavigationCode::Invalid);
  assert(active.active());
  assert(active.refreshMode() == WalkingRefreshMode::Fast);
  std::vector<uint8_t> stopActive{11, 50, 0, 0, 0};
  assert(active.receive(stopActive.data(), stopActive.size(), 7, false, 300).code ==
         LiveNavigationCode::Stopped);
  assert(!active.active());
  assert(active.refreshMode() == WalkingRefreshMode::Economical);
  // disconnect() and expiry reset the mode to Economical as well.
  LiveNavigationSession ended;
  auto fastDisconnect = startV2(7, 60, 1);
  assert(ended.receive(fastDisconnect.data(), fastDisconnect.size(), 7, false, 100).code ==
         LiveNavigationCode::Ready);
  assert(ended.refreshMode() == WalkingRefreshMode::Fast);
  ended.disconnect();
  assert(!ended.active());
  assert(ended.refreshMode() == WalkingRefreshMode::Economical);
  LiveNavigationSession stale;
  auto fastExpire = startV2(7, 61, 1);
  assert(stale.receive(fastExpire.data(), fastExpire.size(), 7, false, 100).code ==
         LiveNavigationCode::Ready);
  stale.expire(100 + 90000);
  assert(!stale.active());
  assert(stale.refreshMode() == WalkingRefreshMode::Economical);
}
