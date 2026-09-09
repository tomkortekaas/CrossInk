#include "GrayMap.h"

#include <algorithm>
#include <cstring>

#include "RouteMapRenderer.h"
#include "WalkMapViewport.h"
namespace navigator {
namespace {
uint32_t u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t u16(const uint8_t* p) { return p[0] | (uint16_t(p[1]) << 8); }
int32_t i32(const uint8_t* p) {
  uint32_t v = u32(p);
  return v <= 0x7fffffffu ? int32_t(v) : int32_t(int64_t(v) - 0x100000000ll);
}
constexpr uint32_t tileBytes = 512 * 832 / 4;
constexpr uint32_t crcTable[] = {0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu, 0x76dc4190u, 0x6b6b51f4u,
                                 0x4db26158u, 0x5005713cu, 0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
                                 0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu};
uint32_t update(uint32_t c, const uint8_t* p, uint32_t n) {
  while (n--) {
    c ^= *p++;
    c = (c >> 4) ^ crcTable[c & 15];
    c = (c >> 4) ^ crcTable[c & 15];
  }
  return c;
}
bool read(WalkMapByteSource& s, uint32_t o, uint8_t* p, uint32_t n) {
  while (n) {
    auto k = s.read(o, p, n);
    if (!k || k > n) return false;
    o += k;
    p += k;
    n -= k;
  }
  return true;
}
bool entryValid(const uint8_t* e, uint32_t data, uint32_t size) {
  auto o = u32(e), n = u32(e + 4);
  return o == 0 ? (n == 0 && u32(e + 8) == 0)
                : (n <= 1024 && o >= data && o <= size && uint64_t(tileBytes) + n * 48 <= size - o);
}
WalkMapStatus checksum(WalkMapByteSource& s, uint32_t off, uint32_t n, uint32_t expected) {
  uint8_t work[256];
  uint32_t crc = ~0u;
  while (n) {
    auto k = std::min<uint32_t>(sizeof(work), n);
    if (!read(s, off, work, k)) return WalkMapStatus::ReadFailed;
    crc = update(crc, work, k);
    off += k;
    n -= k;
  }
  return ~crc == expected ? WalkMapStatus::Ok : WalkMapStatus::InvalidData;
}
}  // namespace
WalkMapStatus GrayMap::open(WalkMapByteSource& s) {
  valid_ = false;
  if (s.size() < 48 || s.size() > WalkMap::kMaxBytes || !read(s, 0, header_, 48)) return WalkMapStatus::ReadFailed;
  auto* h = header_;
  uint32_t cells = uint32_t(u16(h + 24)) * u16(h + 26), data = 48 + cells * 12;
  const int64_t south = i32(h + 12), west = i32(h + 16);
  if (std::memcmp(h, "X3GM", 4) || u16(h + 4) != 1 || u16(h + 6) != 48 || u32(h + 8) != s.size() ||
      u32(h + 20) != 100000 || !cells || cells > 65536 || u16(h + 28) != 512 || u16(h + 30) != 832 ||
      u32(h + 32) != 48 || u32(h + 36) != data || data > s.size() || south < -850000000 ||
      south + int64_t(u16(h + 24)) * 100000 > 850000000 || west < -1800000000 ||
      west + int64_t(u16(h + 26)) * 100000 > 1800000000 || ~update(~0u, h, 44) != u32(h + 44))
    return WalkMapStatus::InvalidData;
  uint8_t entry[12];
  uint32_t crc = ~0u, next = data;
  for (uint32_t i = 0; i < cells; ++i) {
    if (!read(s, 48 + i * 12, entry, 12)) return WalkMapStatus::ReadFailed;
    crc = update(crc, entry, 12);
    if (!entryValid(entry, data, s.size())) return WalkMapStatus::InvalidData;
    if (u32(entry)) {
      if (u32(entry) != next) return WalkMapStatus::InvalidData;
      next += tileBytes + u32(entry + 4) * 48;
    }
  }
  if (~crc != u32(h + 40) || next != s.size()) return WalkMapStatus::InvalidData;
  valid_ = true;
  return WalkMapStatus::Ok;
}
WalkMapStatus GrayMap::draw(WalkMapByteSource& s, const RouteViewport& v, RouteCanvas& canvas) {
  if (!valid_ || s.size() != u32(header_ + 8)) return WalkMapStatus::NotOpen;
  GeoBounds b{};
  if (!walkMapBounds(v, b)) return WalkMapStatus::InvalidData;
  // south/west are int64 for the same reason open() keeps them so: a map may
  // legally span the full 360 degrees of longitude (cols up to 36000), and
  // then both `b.westE7 - west` and `c * 100000` exceed int32.
  const int64_t south = i32(header_ + 12), west = i32(header_ + 16);
  const int rows = u16(header_ + 24), cols = u16(header_ + 26);
  if (b.southE7 < south || b.westE7 < west || b.northE7 > south + rows * 100000ll ||
      b.eastE7 > west + cols * 100000ll)
    return WalkMapStatus::NotOpen;
  const int r0 = (b.southE7 - south) / 100000, r1 = std::min<int>(rows - 1, (b.northE7 - south) / 100000);
  const int c0 = (b.westE7 - west) / 100000, c1 = std::min<int>(cols - 1, (b.eastE7 - west) / 100000);
  if ((r1 - r0 + 1) * (c1 - c0 + 1) > 9) return WalkMapStatus::BudgetExceeded;
  const auto rect = v.mapRect();
  uint8_t entry[12];
  // Validate every visible tile before painting any of it. A tile payload's
  // CRC covers its raster rows and any label bytes that follow them, so
  // integrity is checked even though the calm map never reads or paints the
  // labels themselves (the vector detail layer owns the label rendering).
  for (int r = r0; r <= r1; ++r)
    for (int c = c0; c <= c1; ++c) {
      if (!read(s, 48 + (r * cols + c) * 12, entry, 12)) return WalkMapStatus::ReadFailed;
      if (!entryValid(entry, u32(header_ + 36), s.size())) return WalkMapStatus::InvalidData;
      if (u32(entry)) {
        auto status = checksum(s, u32(entry), tileBytes + u32(entry + 4) * 48, u32(entry + 8));
        if (status != WalkMapStatus::Ok) return status;
      }
    }
  for (int r = r0; r <= r1; ++r)
    for (int c = c0; c <= c1; ++c) {
      if (!read(s, 48 + (r * cols + c) * 12, entry, 12)) return WalkMapStatus::ReadFailed;
      if (!entryValid(entry, u32(header_ + 36), s.size())) return WalkMapStatus::InvalidData;
      // open() bounds every corner to the E7 range, so the int64 sums narrow
      // back to int32 without loss; the products are what needed the width.
      const auto tl = v.project({int32_t(south + (r + 1) * 100000ll), int32_t(west + c * 100000ll)});
      const auto br = v.project({int32_t(south + r * 100000ll), int32_t(west + (c + 1) * 100000ll)});
      if (br.x <= tl.x || br.y <= tl.y) return WalkMapStatus::BudgetExceeded;
      int left = std::max(rect.x, tl.x), right = std::min(rect.x + rect.width, br.x);
      int top = std::max(rect.y, tl.y), bottom = std::min(rect.y + rect.height, br.y);
      uint8_t row[128];
      int lastRow = -1;
      for (int y = top; y < bottom; ++y) {
        int sy = std::clamp(int(int64_t(y - tl.y) * 832 / (br.y - tl.y)), 0, 831);
        if (sy != lastRow) {
          if (u32(entry)) {
            if (!read(s, u32(entry) + sy * 128, row, 128)) return WalkMapStatus::ReadFailed;
          } else
            std::memset(row, 255, sizeof(row));
          lastRow = sy;
        }
        int run = left;
        uint8_t prior = 255;
        for (int x = left; x < right; ++x) {
          int sx = std::clamp(int(int64_t(x - tl.x) * 512 / (br.x - tl.x)), 0, 511);
          uint8_t tone = (row[sx / 4] >> (6 - 2 * (sx % 4))) & 3;
          if (tone != prior) {
            if (x > run) canvas.toneSpan(run, y, x - run, prior);
            run = x;
            prior = tone;
          }
        }
        if (right > run) canvas.toneSpan(run, y, right - run, prior);
      }
    }
  return WalkMapStatus::Ok;
}
}  // namespace navigator
