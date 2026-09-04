#ifdef NDEBUG
#undef NDEBUG // Standalone acceptance checks must run in Release builds too.
#endif
#include "NavScreenRenderer.h"
#include "map/RouteMapRenderer.h"
#include "map/WalkMap.h"
#include "route/RoutePackageV1.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
using namespace navigator;
struct Bytes : RouteByteSource, WalkMapByteSource {
  std::vector<uint8_t> b;
  uint32_t size() const override { return b.size(); }
  uint32_t read(uint32_t o, uint8_t *p, uint32_t n) override {
    if (o >= b.size())
      return 0;
    n = std::min(n, uint32_t(b.size() - o));
    std::memcpy(p, b.data() + o, n);
    return n;
  }
};
void put(Bytes &s, int o, uint32_t v, int n = 4) {
  for (int i = 0; i < n; ++i)
    s.b[o + i] = v >> (8 * i);
}
uint32_t crc(const uint8_t *p, int n) {
  uint32_t c = ~0u;
  while (n--) {
    c ^= *p++;
    for (int i = 0; i < 8; ++i)
      c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
  }
  return ~c;
}
struct Canvas : RouteCanvas {
  std::vector<int> widths;
  void clear() override { widths.clear(); }
  void line(int, int, int, int, int w) override { widths.push_back(w); }
  void disc(int, int, int) override {}
  void ring(int, int, int, int) override {}
};
int main(int argc, char **argv) {
  assert(argc == 2 || argc == 4);
  Bytes route;
  std::ifstream file(argv[1], std::ios::binary);
  route.b.assign(std::istreambuf_iterator<char>(file), {});
  RouteIndex index;
  assert(validateRoutePackageV1(route, index) == DecodeStatus::Ok);
  Bytes data;
  data.b.resize(80);
  std::memcpy(data.b.data(), "X3WM", 4);
  put(data, 4, 1, 2);
  put(data, 6, 48, 2);
  put(data, 8, 80);
  int32_t lat = (index.originLatitudeE7 / 200000) * 200000,
          lon = (index.originLongitudeE7 / 200000) * 200000;
  put(data, 12, lat);
  put(data, 16, lon);
  put(data, 20, 200000);
  put(data, 24, 1, 2);
  put(data, 26, 1, 2);
  put(data, 28, 48);
  put(data, 32, 60);
  put(data, 48, 60);
  put(data, 52, 1);
  put(data, 60, index.originLatitudeE7 + 1000);
  put(data, 64, lon);
  put(data, 68, index.originLatitudeE7 + 1000);
  put(data, 72, lon + 200000);
  data.b[76] = 1;
  put(data, 56, crc(data.b.data() + 60, 20));
  put(data, 36, crc(data.b.data() + 48, 12));
  put(data, 44, crc(data.b.data(), 44));
  if (argc == 4) {
    std::ifstream mapFile(argv[2], std::ios::binary);
    assert(mapFile);
    data.b.assign(std::istreambuf_iterator<char>(mapFile), {});
  }
  WalkMap map;
  assert(map.open(data) == WalkMapStatus::Ok);
  WalkMapLayer layer{&data, &map};
  Canvas canvas;
  auto view = RouteViewport::fitOverview(index, Rect{0, 0, 480, 600});
  assert(RouteMapRenderer::draw(canvas, route, index, view, nullptr, &layer) ==
         RenderStatus::Ok);
  assert(layer.status == WalkMapStatus::Ok);
  assert(canvas.widths.size() > 1 && canvas.widths.front() == 1 &&
         canvas.widths.back() == 3);
  std::vector<uint8_t> plain(792 * 528 / 8), background(plain.size()),
      fallback(plain.size());
  assert(NavScreenRenderer::drawOverview(plain.data(), 792, 528, route, index));
  assert(NavScreenRenderer::drawOverview(background.data(), 792, 528, route,
                                         index, &layer));
  assert(background != plain);
  // Live view uses a real fix at walking scale; null preserves overview bytes.
  std::vector<uint8_t> live(plain.size()), explicitNull(plain.size()), labelled(plain.size());
  assert(NavScreenRenderer::drawOverview(explicitNull.data(), 792, 528, route, index, nullptr, nullptr, nullptr));
  assert(explicitNull == plain);
  CurrentPosition position{{index.originLatitudeE7, index.originLongitudeE7}, 5, 0};
  assert(NavScreenRenderer::drawOverview(live.data(), 792, 528, route, index, nullptr, &position));
  assert(live != plain);
  // Logical center (264, 406) maps to physical (406, 263).
  assert((live[263 * 99 + 406 / 8] & (0x80 >> (406 % 8))) == 0);
  assert(NavScreenRenderer::drawOverview(labelled.data(), 792, 528, route, index, nullptr, &position, "GPS POSITIE"));
  assert(labelled != live);
  for (int y = 0; y < 528; ++y)
    for (int x = 0; x < 744; ++x)
      assert((live[y * 99 + x / 8] & (0x80 >> (x % 8))) ==
             (labelled[y * 99 + x / 8] & (0x80 >> (x % 8))));
  if (argc == 4) {
    std::ofstream image(argv[3], std::ios::binary);
    image.write(reinterpret_cast<const char*>(background.data()), background.size());
    assert(image.good());
  }
  RouteProximity proximity;
  auto away = RouteViewport::centered({index.originLatitudeE7 + 1000000, index.originLongitudeE7}, Rect{0,0,480,600}, 400);
  CurrentPosition far{{index.originLatitudeE7 + 1000000,index.originLongitudeE7},5,0};
  assert(RouteMapRenderer::draw(canvas,route,index,away,&far,nullptr,&proximity)==RenderStatus::Ok);
  assert(proximity.valid && proximity.distanceMeters > 1000);
  data.b[60] ^= 1;
  assert(NavScreenRenderer::drawOverview(fallback.data(), 792, 528, route,
                                         index, &layer));
  assert(layer.status == WalkMapStatus::InvalidData);
  assert(fallback == plain);
}
