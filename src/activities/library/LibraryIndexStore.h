#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "LibraryIndexCodec.h"

namespace LibraryIndexStore {
constexpr const char* kIndexPath = "/.crosspoint/library_index.bin";

// False (and `out` reset) when the index is missing or unreadable; the caller then rebuilds it.
bool load(LibraryIndexData& out);
bool save(const LibraryIndexData& data);
// Recursive scan for supported books; skips hidden entries (so /.crosspoint too) and OS metadata.
void scanBookPaths(std::vector<std::string>& outPaths);
// Reads title/author from EPUB/XTC metadata, keeping the filename title on failure.
// Always marks the entry metadataLoaded so a broken book is not retried on every open.
void loadMetadata(LibraryEntry& entry);
bool isLibraryBook(std::string_view filename);
}  // namespace LibraryIndexStore
