#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <vector>

#include "map/GrayMap.h"
#include "map/RouteMapRenderer.h"
using namespace navigator;
struct Source : WalkMapByteSource {
  std::vector<uint8_t> b;
  uint32_t size() const override { return b.size(); }
  uint32_t read(uint32_t o, uint8_t* p, uint32_t n) override {
    if (o >= b.size()) return 0;
    n = std::min<uint32_t>(n, std::min<uint32_t>(73, b.size() - o));
    std::memcpy(p, b.data() + o, n);
    return n;
  }
};
void put(Source& s, int o, uint32_t v, int n = 4) {
  for (int i = 0; i < n; i++) s.b[o + i] = v >> (8 * i);
}
uint32_t crc(const uint8_t* p, int n) {
  uint32_t c = ~0u;
  while (n--) {
    c ^= *p++;
    for (int i = 0; i < 8; i++) c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
  }
  return ~c;
}
struct Canvas : RouteCanvas {
  int tones[4]{};
  int calls = 0;
  void clear() override {}
  void line(int, int, int, int, int) override {}
  void disc(int, int, int) override {}
  void ring(int, int, int, int) override {}
  void toneSpan(int x, int y, int w, uint8_t tone) override {
    assert(x >= 0 && y >= 0 && x + w <= 120 && y < 160 && w > 0 && tone < 4);
    tones[tone] += w;
    ++calls;
  }
};
int main() {
  Source s;
  s.b.resize(60 + 512 * 832 / 4, 0x1b);
  std::fill(s.b.begin(), s.b.begin() + 60, 0);
  std::memcpy(s.b.data(), "X3GM", 4);
  put(s, 4, 1, 2);
  put(s, 6, 48, 2);
  put(s, 8, s.b.size());
  put(s, 12, 520000000);
  put(s, 16, 40000000);
  put(s, 20, 100000);
  put(s, 24, 1, 2);
  put(s, 26, 1, 2);
  put(s, 28, 512, 2);
  put(s, 30, 832, 2);
  put(s, 32, 48);
  put(s, 36, 60);
  put(s, 48, 60);
  auto sums = [&]() {
    put(s, 56, crc(s.b.data() + 60, s.b.size() - 60));
    put(s, 40, crc(s.b.data() + 48, 12));
    put(s, 44, crc(s.b.data(), 44));
  };
  sums();
  GrayMap map;
  Canvas canvas;
  auto view = RouteViewport::centered({520050000, 40050000}, {0, 0, 120, 160}, 200, 8);
  assert(map.open(s) == WalkMapStatus::Ok);
  assert(map.draw(s, view, canvas) == WalkMapStatus::Ok);
  for (auto n : canvas.tones) assert(n > 0);
  assert(canvas.tones[0] + canvas.tones[1] + canvas.tones[2] + canvas.tones[3] == 120 * 160);
  s.b[100] ^= 1;
  canvas.calls = 0;
  assert(map.draw(s, view, canvas) == WalkMapStatus::InvalidData);
  assert(canvas.calls == 0);
  sums();
  put(s, 28, 65532, 2);
  put(s, 44, crc(s.b.data(), 44));
  assert(map.open(s) == WalkMapStatus::InvalidData);
  put(s, 28, 512, 2);
  sums();
  assert(map.open(s) == WalkMapStatus::Ok);
  auto outside = RouteViewport::centered({510000000, 40050000}, {0, 0, 120, 160}, 200, 8);
  assert(map.draw(s, outside, canvas) != WalkMapStatus::Ok);
  s.b.resize(70);
  assert(map.open(s) != WalkMapStatus::Ok);
}
