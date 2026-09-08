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
  int labelCalls = 0;
  void label(int, int, const char*) override { ++labelCalls; }
  void toneSpan(int x, int y, int w, uint8_t tone) override {
    assert(x >= 0 && y >= 0 && x + w <= 120 && y < 160 && w > 0 && tone < 4);
    tones[tone] += w;
    ++calls;
  }
};
// Builds a single-tile X3GM fixture whose raster is `fill` (0x55 = solid
// dark-gray/water tone 1, 0x1b = all four tones) and, when `withLabel` is
// set, carries one valid 48-byte label anchored at the tile centre.
Source makeFixture(uint8_t fill, bool withLabel) {
  Source s;
  const uint32_t labels = withLabel ? 1u : 0u;
  const uint32_t payload = 512 * 832 / 4 + labels * 48;
  s.b.resize(60 + payload, fill);
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
  put(s, 48, 60);  // cell payload offset
  put(s, 52, labels);
  if (withLabel) {
    const uint32_t at = 60 + 512 * 832 / 4;
    std::memset(s.b.data() + at, 0, 48);  // label bytes must be NUL padded
    put(s, at, 520050000);
    put(s, at + 4, 40050000);
    const char text[] = "TEST";
    std::memcpy(s.b.data() + at + 8, text, sizeof(text));
  }
  put(s, 56, crc(s.b.data() + 60, payload));
  put(s, 40, crc(s.b.data() + 48, 12));
  put(s, 44, crc(s.b.data(), 44));
  return s;
}
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

  // The calm four-gray background never paints map labels: a tile that
  // carries a label payload must still draw its raster without a single
  // canvas.label call (labels belong to the vector detail layer, where they
  // stay readable; on the calm gray background they crowd the panel).
  GrayMap labelledMap;
  Canvas labelledCanvas;
  Source labelled = makeFixture(0x55, true);
  assert(labelledMap.open(labelled) == WalkMapStatus::Ok);
  assert(labelledMap.draw(labelled, view, labelledCanvas) == WalkMapStatus::Ok);
  assert(labelledCanvas.labelCalls == 0);
  // The water-toned raster itself still reaches the canvas unchanged.
  assert(labelledCanvas.tones[1] > 0);
  assert(labelledCanvas.tones[1] + labelledCanvas.tones[3] == 120 * 160);
}
