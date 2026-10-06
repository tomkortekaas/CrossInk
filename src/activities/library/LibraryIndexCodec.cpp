#include "LibraryIndexCodec.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace LibraryIndexCodec {
namespace {
constexpr char kMagic[4] = {'X', 'L', 'I', 'B'};
constexpr uint8_t kFlagMetadataLoaded = 0x01;
constexpr uint8_t kFlagCoverMissing = 0x02;

bool writeU8(LibraryByteWriter& out, const uint8_t v) { return out.write(&v, 1); }
bool writeU16(LibraryByteWriter& out, const uint16_t v) {
  const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
  return out.write(b, 2);
}
bool writeU32(LibraryByteWriter& out, const uint32_t v) {
  const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                        static_cast<uint8_t>(v >> 24)};
  return out.write(b, 4);
}

// Longest prefix of at most maxBytes that does not end inside a UTF-8 sequence.
size_t utf8ClampedLength(const std::string& s, const size_t maxBytes) {
  if (s.size() <= maxBytes) return s.size();
  size_t len = maxBytes;
  while (len > 0 && (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80) --len;
  return len;
}

bool writeString(LibraryByteWriter& out, const std::string& s, const size_t maxBytes) {
  const size_t len = utf8ClampedLength(s, maxBytes);
  return writeU8(out, static_cast<uint8_t>(len)) && (len == 0 || out.write(s.data(), len));
}

bool readU8(LibraryByteReader& in, uint8_t& v) { return in.read(&v, 1); }
bool readU16(LibraryByteReader& in, uint16_t& v) {
  uint8_t b[2];
  if (!in.read(b, 2)) return false;
  v = static_cast<uint16_t>(b[0] | (b[1] << 8));
  return true;
}
bool readU32(LibraryByteReader& in, uint32_t& v) {
  uint8_t b[4];
  if (!in.read(b, 4)) return false;
  v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
      (static_cast<uint32_t>(b[3]) << 24);
  return true;
}
bool readString(LibraryByteReader& in, std::string& s) {
  uint8_t len = 0;
  if (!readU8(in, len)) return false;
  s.resize(len);
  return len == 0 || in.read(s.data(), len);
}
}  // namespace

bool encode(const LibraryIndexData& data, LibraryByteWriter& out) {
  const size_t count = std::min(data.entries.size(), kMaxBooks);
  if (!out.write(kMagic, sizeof(kMagic)) || !writeU8(out, kVersion) || !writeU32(out, data.nextSeq) ||
      !writeU16(out, static_cast<uint16_t>(count))) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    const LibraryEntry& e = data.entries[i];
    const uint8_t flags =
        static_cast<uint8_t>((e.metadataLoaded ? kFlagMetadataLoaded : 0) | (e.coverMissing ? kFlagCoverMissing : 0));
    if (!writeU8(out, flags) || !writeU8(out, static_cast<uint8_t>(e.status)) || !writeU32(out, e.addedSeq) ||
        !writeString(out, e.path, kMaxPathLength) || !writeString(out, e.title, kMaxTitleLength) ||
        !writeString(out, e.author, kMaxAuthorLength)) {
      return false;
    }
  }
  return true;
}

bool decode(LibraryByteReader& in, LibraryIndexData& out) {
  char magic[4];
  uint8_t version = 0;
  uint16_t count = 0;
  out = LibraryIndexData{};
  if (!in.read(magic, sizeof(magic)) || std::char_traits<char>::compare(magic, kMagic, 4) != 0) return false;
  if (!readU8(in, version) || version != kVersion) return false;
  if (!readU32(in, out.nextSeq) || !readU16(in, count) || count > kMaxBooks) return false;
  out.entries.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    LibraryEntry e;
    uint8_t flags = 0;
    uint8_t status = 0;
    if (!readU8(in, flags) || !readU8(in, status) || !readU32(in, e.addedSeq) || !readString(in, e.path) ||
        !readString(in, e.title) || !readString(in, e.author)) {
      return false;
    }
    e.metadataLoaded = (flags & kFlagMetadataLoaded) != 0;
    e.coverMissing = (flags & kFlagCoverMissing) != 0;
    e.status = status <= static_cast<uint8_t>(LibraryBookStatus::Finished) ? static_cast<LibraryBookStatus>(status)
                                                                           : LibraryBookStatus::New;
    out.entries.push_back(std::move(e));
  }
  return true;
}

bool mergeScannedPaths(LibraryIndexData& index, std::vector<std::string>&& scannedPaths) {
  std::sort(scannedPaths.begin(), scannedPaths.end());
  scannedPaths.erase(std::unique(scannedPaths.begin(), scannedPaths.end()), scannedPaths.end());

  const size_t before = index.entries.size();
  index.entries.erase(std::remove_if(index.entries.begin(), index.entries.end(),
                                     [&scannedPaths](const LibraryEntry& e) {
                                       return !std::binary_search(scannedPaths.begin(), scannedPaths.end(), e.path);
                                     }),
                      index.entries.end());
  bool changed = index.entries.size() != before;

  std::vector<std::string_view> known;
  known.reserve(index.entries.size());
  for (const auto& e : index.entries) known.push_back(e.path);
  std::sort(known.begin(), known.end());

  // Collect which scanned paths are new before appending: `known` views point into
  // index.entries, so they must not be used once push_back may move those strings.
  std::vector<size_t> newIndices;
  newIndices.reserve(scannedPaths.size());
  for (size_t i = 0; i < scannedPaths.size(); ++i) {
    if (index.entries.size() + newIndices.size() >= kMaxBooks) break;
    if (std::binary_search(known.begin(), known.end(), std::string_view(scannedPaths[i]))) continue;
    newIndices.push_back(i);
  }

  if (!newIndices.empty()) {
    index.entries.reserve(std::min(kMaxBooks, index.entries.size() + newIndices.size()));
    for (const size_t i : newIndices) {
      LibraryEntry e;
      e.path = std::move(scannedPaths[i]);
      e.title = LibrarySort::titleFromPath(e.path);
      e.addedSeq = index.nextSeq;
      index.entries.push_back(std::move(e));
    }
    ++index.nextSeq;
    changed = true;
  }
  return changed;
}
}  // namespace LibraryIndexCodec
