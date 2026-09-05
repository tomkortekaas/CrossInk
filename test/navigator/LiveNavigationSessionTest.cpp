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
}
