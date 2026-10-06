#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "LibrarySort.h"

struct LibraryIndexData {
  uint32_t nextSeq = 1;  // generation given to books found by the next scan
  std::vector<LibraryEntry> entries;
};

// Byte sinks/sources so the codec stays independent of the SD HAL (host-testable).
class LibraryByteWriter {
 public:
  virtual ~LibraryByteWriter() = default;
  virtual bool write(const void* data, size_t len) = 0;
};

class LibraryByteReader {
 public:
  virtual ~LibraryByteReader() = default;
  virtual bool read(void* data, size_t len) = 0;
};

namespace LibraryIndexCodec {
constexpr uint8_t kVersion = 1;
constexpr size_t kMaxBooks = 300;
constexpr size_t kMaxPathLength = 255;
constexpr size_t kMaxTitleLength = 95;
constexpr size_t kMaxAuthorLength = 63;

bool encode(const LibraryIndexData& data, LibraryByteWriter& out);
// Returns false on bad magic, unknown version, oversize count or truncation; `out` is then unspecified.
bool decode(LibraryByteReader& in, LibraryIndexData& out);
// Keeps entries whose path is still on the card, drops vanished ones and appends unknown
// paths (title from the filename) under one new generation. Returns true when anything changed.
bool mergeScannedPaths(LibraryIndexData& index, const std::vector<std::string>& scannedPaths);
}  // namespace LibraryIndexCodec
