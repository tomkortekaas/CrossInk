#ifdef NDEBUG
#undef NDEBUG // Standalone acceptance checks must run in Release builds too.
#endif
#include "WalkMap.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
using namespace navigator;
struct Source : WalkMapByteSource {
  std::vector<uint8_t> bytes;
  uint32_t largest = 0;
  bool fail = false;
  uint32_t payloadStart = 0, payloadToIndexReads = 0;
  bool lastReadWasPayload = false;
  uint32_t size() const override { return bytes.size(); }
  uint32_t read(uint32_t o, uint8_t *d, uint32_t n) override {
    largest = std::max(largest, n);
    if (payloadStart) {
      if (lastReadWasPayload && o < payloadStart) ++payloadToIndexReads;
      lastReadWasPayload = o >= payloadStart;
    }
    if (fail || o >= bytes.size())
      return 0;
    n = std::min(n, uint32_t(bytes.size() - o));
    std::memcpy(d, bytes.data() + o, n);
    return n;
  }
};
static void put(std::vector<uint8_t> &b, size_t o, uint32_t v, int n = 4) {
  for (int i = 0; i < n; ++i)
    b[o + i] = v >> (8 * i);
}
static uint32_t crc(const uint8_t *b, size_t n) {
  uint32_t c = ~0u;
  while (n--) {
    c ^= *b++;
    for (int i = 0; i < 8; ++i)
      c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
  }
  return ~c;
}
static void seal(Source &s) {
  put(s.bytes, 56, crc(s.bytes.data() + 60, 20));
  put(s.bytes, 36, crc(s.bytes.data() + 48, 12));
  put(s.bytes, 44, crc(s.bytes.data(), 44));
}
static Source fixture() {
  Source s;
  s.bytes.resize(80);
  std::memcpy(s.bytes.data(), "X3WM", 4);
  put(s.bytes, 4, 1, 2);
  put(s.bytes, 6, 48, 2);
  put(s.bytes, 8, 80);
  put(s.bytes, 12, 520000000);
  put(s.bytes, 16, 40000000);
  put(s.bytes, 20, 200000);
  put(s.bytes, 24, 1, 2);
  put(s.bytes, 26, 1, 2);
  put(s.bytes, 28, 48);
  put(s.bytes, 32, 60);
  put(s.bytes, 48, 60);
  put(s.bytes, 52, 1);
  put(s.bytes, 60, 520010000);
  put(s.bytes, 64, 40010000);
  put(s.bytes, 68, 520100000);
  put(s.bytes, 72, 40100000);
  s.bytes[76] = 1;
  seal(s);
  return s;
}
struct Seen {
  int count = 0;
  WalkMapEdge edge{};
};
static void capture(void *p, const WalkMapEdge &e) {
  auto &s = *static_cast<Seen *>(p);
  ++s.count;
  s.edge = e;
}
int main(int argc, char **argv) {
  auto s = fixture();
  WalkMap map;
  Seen seen;
  GeoBounds bounds{520000000, 40000000, 520200000, 40200000};
  assert(map.open(s) == WalkMapStatus::Ok);
  assert(map.visit(s, bounds, capture, &seen) == WalkMapStatus::Ok);
  assert(seen.count == 1);
  assert(seen.edge.latitude1E7 == 520010000);
  assert(s.largest <= 256);
  seen.count = 0;
  GeoBounds outside{0, 0, 1, 1};
  assert(map.visit(s, outside, capture, &seen) == WalkMapStatus::Ok);
  assert(seen.count == 0);
  auto bad = fixture();
  bad.bytes[60] ^= 1;
  seen.count = 0;
  assert(map.open(bad) == WalkMapStatus::Ok);
  assert(map.visit(bad, bounds, capture, &seen) == WalkMapStatus::InvalidData);
  assert(seen.count == 0);
  bad = fixture();
  put(bad.bytes, 48, 0xfffffff0);
  put(bad.bytes, 36, crc(bad.bytes.data() + 48, 12));
  put(bad.bytes, 44, crc(bad.bytes.data(), 44));
  assert(map.open(bad) == WalkMapStatus::InvalidData);
  bad = fixture();
  put(bad.bytes, 60, 0);
  seal(bad);
  assert(map.open(bad) == WalkMapStatus::Ok);
  assert(map.visit(bad, bounds, capture, &seen) == WalkMapStatus::InvalidData);
  bad = fixture();
  bad.bytes[77] = 2;
  seal(bad);
  assert(map.open(bad) == WalkMapStatus::Ok);
  assert(map.visit(bad, bounds, capture, &seen) == WalkMapStatus::InvalidData);
  bad = fixture();
  bad.bytes[0] = 'Y';
  assert(map.open(bad) == WalkMapStatus::InvalidData);
  assert(map.visit(s, bounds, capture, &seen) == WalkMapStatus::NotOpen);
  s = fixture();
  assert(map.open(s) == WalkMapStatus::Ok);
  s.fail = true;
  assert(map.visit(s, bounds, capture, &seen) == WalkMapStatus::ReadFailed);
  s = fixture();
  assert(map.open(s) == WalkMapStatus::Ok);
  assert(map.visit(s, bounds, nullptr, nullptr) == WalkMapStatus::InvalidData);
  Source grid;
  grid.bytes.resize(48 + 1025 * 12);
  std::memcpy(grid.bytes.data(), "X3WM", 4);
  put(grid.bytes, 4, 1, 2);
  put(grid.bytes, 6, 48, 2);
  put(grid.bytes, 8, grid.bytes.size());
  put(grid.bytes, 20, 200000);
  put(grid.bytes, 24, 1, 2);
  put(grid.bytes, 26, 1025, 2);
  put(grid.bytes, 28, 48);
  put(grid.bytes, 32, grid.bytes.size());
  put(grid.bytes, 36, crc(grid.bytes.data() + 48, 1025 * 12));
  put(grid.bytes, 44, crc(grid.bytes.data(), 44));
  assert(map.open(grid) == WalkMapStatus::Ok);
  seen.count = 0;
  assert(map.visit(grid, GeoBounds{0, 0, 200000, 205000000}, capture, &seen) ==
         WalkMapStatus::BudgetExceeded);
  assert(seen.count == 0);
  s = fixture();
  put(s.bytes, 12, uint32_t(-200000));
  put(s.bytes, 16, uint32_t(-200000));
  put(s.bytes, 60, uint32_t(-190000));
  put(s.bytes, 64, uint32_t(-190000));
  put(s.bytes, 68, uint32_t(-100000));
  put(s.bytes, 72, uint32_t(-100000));
  seal(s);
  assert(map.open(s) == WalkMapStatus::Ok);
  seen.count = 0;
  assert(map.visit(s, GeoBounds{-200000, -200000, 0, 0}, capture, &seen) ==
         WalkMapStatus::Ok);
  assert(seen.count == 1);
  // Cross two complete directory batches and a short tail, on two rows.
  Source wide;
  constexpr uint32_t cells = 86, directoryEnd = 48 + cells * 12;
  wide.bytes = fixture().bytes;
  wide.bytes.resize(directoryEnd + cells * 20);
  put(wide.bytes, 8, wide.bytes.size());
  put(wide.bytes, 24, 2, 2);
  put(wide.bytes, 26, 43, 2);
  put(wide.bytes, 32, directoryEnd);
  for (uint32_t k = 0; k < cells; ++k) {
    const uint32_t off = directoryEnd + k * 20;
    put(wide.bytes, off, 520000000 + (k / 43) * 200000 + 1000);
    put(wide.bytes, off + 4, 40000000 + (k % 43) * 200000 + 1000);
    put(wide.bytes, off + 8, 520000000 + (k / 43) * 200000 + 2000);
    put(wide.bytes, off + 12, 40000000 + (k % 43) * 200000 + 2000);
    put(wide.bytes, off + 16, 1);
    put(wide.bytes, 48 + k * 12, off);
    put(wide.bytes, 52 + k * 12, 1);
    put(wide.bytes, 56 + k * 12, crc(wide.bytes.data() + off, 20));
  }
  put(wide.bytes, 36, crc(wide.bytes.data() + 48, cells * 12));
  put(wide.bytes, 44, crc(wide.bytes.data(), 44));
  wide.payloadStart = directoryEnd;
  assert(map.open(wide) == WalkMapStatus::Ok);
  seen.count = 0;
  assert(map.visit(wide, GeoBounds{-850000000, -1800000000, 850000000, 1800000000},
                   capture, &seen) == WalkMapStatus::Ok);
  assert(seen.count == cells);
  assert(wide.payloadToIndexReads <= 6);
  if (argc == 2) {
    Source generated;
    std::ifstream f(argv[1], std::ios::binary);
    assert(f);
    generated.bytes.assign(std::istreambuf_iterator<char>(f), {});
    generated.payloadStart = uint32_t(generated.bytes[32]) | (uint32_t(generated.bytes[33]) << 8) |
                             (uint32_t(generated.bytes[34]) << 16) | (uint32_t(generated.bytes[35]) << 24);
    assert(map.open(generated) == WalkMapStatus::Ok);
    seen.count = 0;
    assert(map.visit(generated,
                     GeoBounds{-850000000, -1800000000, 850000000, 1800000000},
                     capture, &seen) == WalkMapStatus::Ok);
    assert(seen.count > 0);
    // Six columns fit in one 240-byte directory batch per row. Do not seek
    // back through the regional FAT chain after each individual cell.
    assert(generated.payloadToIndexReads <= 6);
    std::cout << "Converter fixture edges=" << seen.count
              << " bytes=" << generated.size() << "\n";
  }
  std::cout << "WalkMap meaningful corruption/bounds/read tests passed\n";
}
