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
std::vector<uint8_t> startV3(uint32_t route = 7, uint32_t session = 9, uint8_t mode = 2) {
  std::vector<uint8_t> b(11);
  b[0] = 8;
  b[1] = 3;
  put(b, 2, route);
  put(b, 6, session);
  b[10] = mode;
  return b;
}
std::vector<uint8_t> fixV3(uint16_t seq = 0, uint32_t session = 9, uint32_t progress = 0) {
  std::vector<uint8_t> b = fix(seq, session);
  b.resize(24);
  put(b, 20, progress);
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
  assert(!s.position(200, p, false));
  assert(s.position(200, p, true));
  assert(send(f, 5000).code == LiveNavigationCode::FixAccepted);
  // A stationary walk may not produce another CLLocation callback. Retain the
  // accepted fix for the connected live session instead of losing GPS after
  // the X3's former 30-second freshness window.
  assert(s.position(60000, p));
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
  assert(s.position(80000, p));
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
  s.expire(90000, true);
  assert(s.active());
  assert(s.position(90000, p));
  s.expire(90000, false);
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
  unknownVersion[1] = 4;  // protocol version 4 is unknown (3 is live progress)
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
  // Task 4 - live protocol v3: START v3 (11 bytes, version byte 3) negotiates
  // 24-byte progress-bearing FIX frames; the refresh-mode byte keeps the v2
  // meaning. Legacy v1/v2 sessions still demand exactly 20-byte FIX frames and
  // reject the 24-byte layout, while a v3 session rejects the 20-byte layout.
  // Replay comparison covers the whole negotiated frame including the progress
  // u32, and every exit path clears both the fix and the v3 capability.
  auto send3 = [&](LiveNavigationSession& s, const std::vector<uint8_t>& b, uint32_t now = 100,
                   uint32_t route = 7) { return s.receive(b.data(), b.size(), route, false, now); };
  // START v3 acceptance; the mode byte is honored exactly as in v2.
  LiveNavigationSession v3;
  auto s3 = startV3(7, 90, 1);
  assert(send3(v3, s3, 100).code == LiveNavigationCode::Ready);
  assert(v3.active());
  assert(v3.refreshMode() == WalkingRefreshMode::Fast);
  assert(!v3.position(100, p));
  // A v3 START naming an unknown refresh mode is rejected without activating.
  LiveNavigationSession v3Strict;
  auto s3BadMode = startV3(7, 91, 3);
  assert(send3(v3Strict, s3BadMode, 100).code == LiveNavigationCode::Invalid);
  assert(!v3Strict.active());
  // v3 sessions require exactly 24-byte FIX frames.
  auto v3LegacyFix = fix(1, 90);  // 20-byte frame
  assert(send3(v3, v3LegacyFix, 101).code == LiveNavigationCode::Invalid);
  auto v3FixA = fixV3(1, 90, 12345);
  assert(send3(v3, v3FixA, 102).code == LiveNavigationCode::FixAccepted);
  assert(v3.position(102, p));
  assert(p.latitudeE7 == 523700000 && p.longitudeE7 == 49000000);
  assert(p.accuracyMeters == 5);
  assert(p.hasRouteProgress && p.distanceFromStartMeters == 12345);
  // Progress parses as an unsigned u32 even at the top of its range.
  LiveNavigationSession v3Max;
  auto s3Max = startV3(7, 92, 0);
  assert(send3(v3Max, s3Max, 100).code == LiveNavigationCode::Ready);
  auto v3MaxFix = fixV3(1, 92, 0xffffffffu);
  assert(send3(v3Max, v3MaxFix, 101).code == LiveNavigationCode::FixAccepted);
  assert(v3Max.position(101, p));
  assert(p.hasRouteProgress && p.distanceFromStartMeters == 0xffffffffu);
  // Replay comparison covers progress: the identical frame replays cleanly,
  // while a same-sequence frame with different progress is Invalid.
  assert(send3(v3, v3FixA, 103).code == LiveNavigationCode::FixAccepted);
  auto v3FixADifferentProgress = v3FixA;
  put(v3FixADifferentProgress, 20, 12346);
  assert(send3(v3, v3FixADifferentProgress, 104).code == LiveNavigationCode::Invalid);
  // A force-refresh bit still re-arms only on a freshly accepted FIX.
  auto v3Force = fixV3(2, 90, 12400);
  v3Force[19] = 2;
  assert(send3(v3, v3Force, 105).code == LiveNavigationCode::FixAccepted);
  assert(v3.takeForceRefresh());
  assert(!v3.takeForceRefresh());
  // START replay is idempotent only for the negotiated version: replaying the
  // same v3 START returns Ready, but a same-session v2 START cannot downgrade
  // the live session.
  assert(send3(v3, s3, 106).code == LiveNavigationCode::Ready);
  auto v3Downgrade = startV2(7, 90, 1);
  assert(send3(v3, v3Downgrade, 107).code == LiveNavigationCode::Invalid);
  assert(v3.active());
  // Legacy v1/v2 sessions reject the 24-byte FIX layout and never expose
  // progress to position().
  LiveNavigationSession v1s;
  auto s1b = start(7, 93);
  assert(send3(v1s, s1b, 100).code == LiveNavigationCode::Ready);
  auto v1Fix24 = fixV3(1, 93, 777);
  assert(send3(v1s, v1Fix24, 101).code == LiveNavigationCode::Invalid);
  auto v1Fix20 = fix(1, 93);
  assert(send3(v1s, v1Fix20, 102).code == LiveNavigationCode::FixAccepted);
  assert(v1s.position(102, p));
  assert(!p.hasRouteProgress && p.distanceFromStartMeters == 0);
  LiveNavigationSession v2s;
  auto s2b = startV2(7, 94, 2);
  assert(send3(v2s, s2b, 100).code == LiveNavigationCode::Ready);
  auto v2Fix24 = fixV3(1, 94, 888);
  assert(send3(v2s, v2Fix24, 101).code == LiveNavigationCode::Invalid);
  auto v2Fix20 = fix(1, 94);
  assert(send3(v2s, v2Fix20, 102).code == LiveNavigationCode::FixAccepted);
  assert(v2s.position(102, p));
  assert(!p.hasRouteProgress && p.distanceFromStartMeters == 0);
  // STOP clears progress and the v3 capability: a later legacy session on the
  // same object accepts 20-byte FIX frames again without progress.
  LiveNavigationSession stopV3;
  auto s3Stop = startV3(7, 95, 0);
  assert(send3(stopV3, s3Stop, 100).code == LiveNavigationCode::Ready);
  auto v3StopFix = fixV3(1, 95, 500);
  assert(send3(stopV3, v3StopFix, 101).code == LiveNavigationCode::FixAccepted);
  assert(stopV3.position(101, p) && p.hasRouteProgress);
  std::vector<uint8_t> v3Stop{11, 95, 0, 0, 0};
  assert(send3(stopV3, v3Stop, 102).code == LiveNavigationCode::Stopped);
  assert(!stopV3.position(102, p));
  auto s1AfterStop = start(7, 96);  // new session id on the same object
  assert(send3(stopV3, s1AfterStop, 103).code == LiveNavigationCode::Ready);
  auto v1AfterStopFix = fix(1, 96);
  assert(send3(stopV3, v1AfterStopFix, 104).code == LiveNavigationCode::FixAccepted);
  assert(stopV3.position(104, p));
  assert(!p.hasRouteProgress && p.distanceFromStartMeters == 0);
  // disconnect() clears progress and capability the same way.
  LiveNavigationSession disconnectV3;
  auto s3Disc = startV3(7, 97, 1);
  assert(send3(disconnectV3, s3Disc, 100).code == LiveNavigationCode::Ready);
  auto v3DiscFix = fixV3(1, 97, 900);
  assert(send3(disconnectV3, v3DiscFix, 101).code == LiveNavigationCode::FixAccepted);
  assert(disconnectV3.position(101, p) && p.hasRouteProgress);
  disconnectV3.disconnect();
  assert(!disconnectV3.active());
  assert(!disconnectV3.position(101, p));
  // A route mismatch disconnects the session, so progress cannot outlive it.
  LiveNavigationSession mismatchV3;
  auto s3Mismatch = startV3(7, 98, 0);
  assert(send3(mismatchV3, s3Mismatch, 100).code == LiveNavigationCode::Ready);
  auto v3MismatchFix = fixV3(1, 98, 600);
  assert(send3(mismatchV3, v3MismatchFix, 101).code == LiveNavigationCode::FixAccepted);
  assert(mismatchV3.position(101, p) && p.hasRouteProgress);
  assert(send3(mismatchV3, v3MismatchFix, 102, 8).code == LiveNavigationCode::Invalid);
  assert(!mismatchV3.active());
  assert(!mismatchV3.position(102, p));
}
