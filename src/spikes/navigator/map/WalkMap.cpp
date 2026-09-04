#include "WalkMap.h"
#include <algorithm>
#include <cstring>
namespace navigator {
namespace {
uint32_t u32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}
uint16_t u16(const uint8_t *p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
int32_t i32(const uint8_t *p) {
  const uint32_t v = u32(p);
  return v <= 0x7fffffffu ? int32_t(v) : int32_t(int64_t(v) - 0x100000000ll);
}
// IEEE reflected CRC-32 (polynomial 0xedb88320) processed four bits at a time.
// 16 uint32_t entries (64 bytes). Entry i equals the bitwise result after the
// four low bits of the register (value i) are shifted out.
static constexpr uint32_t kCrc32Nibble[16] = {
    0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
    0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
    0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
    0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu};
uint32_t update(uint32_t crc, const uint8_t *p, uint32_t n) {
  while (n--) {
    crc ^= *p++;
    crc = (crc >> 4) ^ kCrc32Nibble[crc & 0x0fu];
    crc = (crc >> 4) ^ kCrc32Nibble[crc & 0x0fu];
  }
  return crc;
}
bool read(WalkMapByteSource &s, uint32_t off, uint8_t *p, uint32_t n) {
  while (n) {
    const uint32_t got = s.read(off, p, n);
    if (!got || got > n)
      return false;
    off += got;
    p += got;
    n -= got;
  }
  return true;
}
bool entryValid(const uint8_t *e, uint32_t data, uint32_t size, uint32_t recordSize) {
  const uint32_t off = u32(e), count = u32(e + 4);
  if (!count)
    return off == 0 && u32(e + 8) == 0;
  return count <= (recordSize == 20 ? 16384u : 65536u) && off >= data && off <= size &&
         uint64_t(count) * recordSize <= size - off;
}
WalkMapEdge edge(const uint8_t *p, uint32_t recordSize) {
  WalkMapEdge result{i32(p), i32(p + 4), i32(p + 8), i32(p + 12), p[16], p[17]};
  if (recordSize == 64 && p[18] <= 44) std::memcpy(result.label, p + 20, p[18]);
  return result;
}
bool validEdge(const uint8_t *p, int64_t lat, int64_t lon, uint32_t cellSize, uint32_t recordSize) {
  auto e = edge(p, recordSize);
  if (recordSize == 20) {
    if ((e.kind != 1 && e.kind != 2) || u16(p + 18)) return false;
  } else {
    if (e.kind < 1 || e.kind > 7 || p[18] > 44 || p[19]) return false;
    if (e.kind == 7) {
      if (!p[18] || e.latitude1E7 != e.latitude2E7 || e.longitude1E7 != e.longitude2E7) return false;
    } else if (p[18]) return false;
    for (uint32_t i = 0; i < 44; ++i) {
      if (i < p[18] ? (p[20 + i] < 32 || p[20 + i] > 126) : p[20 + i] != 0) return false;
    }
  }
  return e.flags <= 1 &&
         e.latitude1E7 >= lat && e.latitude1E7 <= lat + cellSize &&
         e.latitude2E7 >= lat && e.latitude2E7 <= lat + cellSize &&
         e.longitude1E7 >= lon && e.longitude1E7 <= lon + cellSize &&
         e.longitude2E7 >= lon && e.longitude2E7 <= lon + cellSize;
}
WalkMapStatus cell(WalkMapByteSource &s, const uint8_t *entry, int64_t lat,
                   int64_t lon, uint32_t cellSize, uint32_t recordSize, WalkMapVisitor visitor, void *context) {
  const uint32_t count = u32(entry + 4), offset = u32(entry);
  if (!count)
    return WalkMapStatus::Ok;
  // 240 bytes is twelve records. Reused on both passes; never a whole cell.
  uint8_t buffer[240];
  for (int pass = 0; pass < 2; ++pass) {
    uint32_t crc = ~0u;
    for (uint32_t start = 0; start < count;) {
      const uint32_t batch = std::min<uint32_t>(sizeof(buffer) / recordSize, count - start), bytes = batch * recordSize;
      if (!read(s, offset + start * recordSize, buffer, bytes))
        return WalkMapStatus::ReadFailed;
      crc = update(crc, buffer, bytes);
      for (uint32_t j = 0; j < batch; ++j) {
        const auto *p = buffer + j * recordSize;
        if (!validEdge(p, lat, lon, cellSize, recordSize))
          return WalkMapStatus::InvalidData;
        if (pass)
          visitor(context, edge(p, recordSize));
      }
      start += batch;
    }
    if (~crc != u32(entry + 8))
      return WalkMapStatus::InvalidData;
  }
  return WalkMapStatus::Ok;
}
} // namespace
WalkMapStatus WalkMap::open(WalkMapByteSource &s) {
  valid_ = false;
  const uint32_t size = s.size();
  if (size < 48 || size > kMaxBytes)
    return WalkMapStatus::InvalidData;
  uint8_t h[48];
  if (!read(s, 0, h, 48))
    return WalkMapStatus::ReadFailed;
  const uint32_t rows = u16(h + 24), cols = u16(h + 26), count = rows * cols,
                 data = u32(h + 32);
  const int64_t lat = i32(h + 12), lon = i32(h + 16);
  const uint32_t version = u16(h + 4), recordSize = version == 1 ? 20 : 64;
  const uint32_t cellSize = version == 1 ? kCellE7 : 100000;
  if (std::memcmp(h, "X3WM", 4) || (version != 1 && version != 2) || u16(h + 6) != 48 ||
      u32(h + 8) != size || (version == 1 && size > 512u * 1024u * 1024u) || u32(h + 20) != cellSize || !rows || !cols ||
      count > 65536 || u32(h + 28) != 48 || data != 48 + count * 12 ||
      data > size || u32(h + 40) || ~update(~0u, h, 44) != u32(h + 44) ||
      lat < -850000000ll || lat + int64_t(rows) * cellSize > 850000000ll ||
      lon < -1800000000ll || lon + int64_t(cols) * cellSize > 1800000000ll)
    return WalkMapStatus::InvalidData;
  uint8_t buffer[240];
  uint32_t crc = ~0u, expected = data;
  for (uint32_t first = 0; first < count;) {
    const uint32_t batch = std::min<uint32_t>(20, count - first), bytes = batch * 12;
    if (!read(s, 48 + first * 12, buffer, bytes))
      return WalkMapStatus::ReadFailed;
    crc = update(crc, buffer, bytes);
    for (uint32_t j = 0; j < batch; ++j) {
      const auto *e = buffer + j * 12;
      if (!entryValid(e, data, size, recordSize))
        return WalkMapStatus::InvalidData;
      if (u32(e + 4)) {
        if (u32(e) != expected)
          return WalkMapStatus::InvalidData;
        expected += u32(e + 4) * recordSize;
      }
    }
    first += batch;
  }
  if (~crc != u32(h + 36) || expected != size)
    return WalkMapStatus::InvalidData;
  std::memcpy(header_, h, 48);
  valid_ = true;
  return WalkMapStatus::Ok;
}
WalkMapStatus WalkMap::visit(WalkMapByteSource &s, const GeoBounds &b,
                             WalkMapVisitor visitor, void *context) const {
  if (!valid_)
    return WalkMapStatus::NotOpen;
  if (!visitor || b.southE7 > b.northE7 || b.westE7 > b.eastE7 ||
      b.southE7 < -850000000 || b.northE7 > 850000000 ||
      b.westE7 < -1800000000 || b.eastE7 > 1800000000)
    return WalkMapStatus::InvalidData;
  // Recheck streamed metadata: an SD swap must not reuse a previous index.
  WalkMap current;
  auto status = current.open(s);
  if (status != WalkMapStatus::Ok)
    return status;
  if (std::memcmp(header_, current.header_, 48))
    return WalkMapStatus::InvalidData;
  const int64_t lat = i32(header_ + 12), lon = i32(header_ + 16);
  const uint32_t cellSize = u32(header_ + 20), recordSize = u16(header_ + 4) == 1 ? 20 : 64;
  const uint32_t rows = u16(header_ + 24), cols = u16(header_ + 26);
  const int64_t north = lat + int64_t(rows) * cellSize,
                east = lon + int64_t(cols) * cellSize;
  if (b.northE7 < lat || b.southE7 > north || b.eastE7 < lon || b.westE7 > east)
    return WalkMapStatus::Ok;
  // Include the preceding cell for a query exactly on a grid boundary.
  auto first = [cellSize](int64_t coordinate, int64_t origin, uint32_t count) {
    int64_t delta = std::max(int64_t(0), coordinate - origin);
    if (delta > 0)
      --delta;
    return uint32_t(std::min(int64_t(count - 1), delta / cellSize));
  };
  auto last = [cellSize](int64_t coordinate, int64_t origin, uint32_t count) {
    return uint32_t(
        std::min(int64_t(count - 1),
                 std::max(int64_t(0), coordinate - origin) / cellSize));
  };
  const uint32_t r0 = first(b.southE7, lat, rows),
                 r1 = last(b.northE7, lat, rows),
                 c0 = first(b.westE7, lon, cols),
                 c1 = last(b.eastE7, lon, cols);
  if (uint64_t(r1 - r0 + 1) * (c1 - c0 + 1) > 1024)
    return WalkMapStatus::BudgetExceeded;
  uint32_t edges = 0;
  uint8_t entry[12];
  // Preflight total work before any callback. Small paths are never truncated.
  for (uint32_t r = r0; r <= r1; ++r)
    for (uint32_t c = c0; c <= c1; ++c) {
      if (!read(s, 48 + (r * cols + c) * 12, entry, 12))
        return WalkMapStatus::ReadFailed;
      if (!entryValid(entry, u32(header_ + 32), u32(header_ + 8), recordSize))
        return WalkMapStatus::InvalidData;
      edges += u32(entry + 4);
      if (edges > (recordSize == 20 ? 100000u : 200000u))
        return WalkMapStatus::BudgetExceeded;
    }
  // Cache only one small directory batch. Seeking back to the index after
  // every cell repeatedly traverses the large file's FAT chain on real SD.
  // 240 bytes (20 entries), independent of region size; cell() keeps its own
  // existing 240-byte record buffer and both CRC-validation passes.
  uint8_t entries[240];
  for (uint32_t r = r0; r <= r1; ++r) {
    for (uint32_t c = c0; c <= c1;) {
      const uint32_t batch = std::min<uint32_t>(20, c1 - c + 1);
      if (!read(s, 48 + (r * cols + c) * 12, entries, batch * 12))
        return WalkMapStatus::ReadFailed;
      for (uint32_t j = 0; j < batch; ++j) {
        const auto *e = entries + j * 12;
        if (!entryValid(e, u32(header_ + 32), u32(header_ + 8), recordSize))
          return WalkMapStatus::InvalidData;
        status = cell(s, e, lat + int64_t(r) * cellSize,
                      lon + int64_t(c + j) * cellSize, cellSize, recordSize, visitor, context);
        if (status != WalkMapStatus::Ok)
          return status;
      }
      c += batch;
    }
  }
  return WalkMapStatus::Ok;
}
} // namespace navigator
