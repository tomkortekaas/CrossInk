#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "activities/library/LibraryIndexCodec.h"

namespace {
class VectorWriter final : public LibraryByteWriter {
 public:
  std::vector<uint8_t> bytes;
  bool write(const void* data, size_t len) override {
    const auto* p = static_cast<const uint8_t*>(data);
    bytes.insert(bytes.end(), p, p + len);
    return true;
  }
};

class VectorReader final : public LibraryByteReader {
 public:
  explicit VectorReader(const std::vector<uint8_t>& b) : bytes(b) {}
  bool read(void* data, size_t len) override {
    if (pos + len > bytes.size()) return false;
    std::memcpy(data, bytes.data() + pos, len);
    pos += len;
    return true;
  }

 private:
  const std::vector<uint8_t>& bytes;
  size_t pos = 0;
};

const LibraryEntry* find(const LibraryIndexData& d, const std::string& path) {
  for (const auto& e : d.entries)
    if (e.path == path) return &e;
  return nullptr;
}
}  // namespace

int main() {
  // Merge: first scan assigns one shared generation.
  LibraryIndexData index;
  assert(LibraryIndexCodec::mergeScannedPaths(index, {"/b.epub", "/a.epub"}));
  assert(index.entries.size() == 2 && index.nextSeq == 2);
  assert(find(index, "/a.epub")->addedSeq == 1 && find(index, "/a.epub")->title == "a");

  // Known metadata survives; vanished books drop; new ones get the next generation.
  for (auto& e : index.entries) {
    if (e.path == "/a.epub") {
      e.title = "Real A";
      e.metadataLoaded = true;
      e.status = LibraryBookStatus::Finished;
    }
  }
  assert(LibraryIndexCodec::mergeScannedPaths(index, {"/a.epub", "/dir/c.txt"}));
  assert(index.entries.size() == 2 && index.nextSeq == 3);
  assert(find(index, "/b.epub") == nullptr);
  assert(find(index, "/a.epub")->title == "Real A" && find(index, "/a.epub")->metadataLoaded);
  assert(find(index, "/dir/c.txt")->addedSeq == 2 && find(index, "/dir/c.txt")->title == "c");

  // Unchanged scan reports no change and keeps the generation.
  assert(!LibraryIndexCodec::mergeScannedPaths(index, {"/dir/c.txt", "/a.epub"}));
  assert(index.nextSeq == 3);

  // Cap.
  LibraryIndexData big;
  std::vector<std::string> many;
  for (int i = 0; i < 301; ++i) many.push_back("/b" + std::to_string(i) + ".epub");
  LibraryIndexCodec::mergeScannedPaths(big, many);
  assert(big.entries.size() == LibraryIndexCodec::kMaxBooks);

  // Round trip.
  index.entries[0].coverMissing = true;
  index.entries[0].author = "Umberto Eco";
  VectorWriter writer;
  assert(LibraryIndexCodec::encode(index, writer));
  LibraryIndexData decoded;
  VectorReader reader(writer.bytes);
  assert(LibraryIndexCodec::decode(reader, decoded));
  assert(decoded.nextSeq == index.nextSeq && decoded.entries.size() == index.entries.size());
  for (size_t i = 0; i < index.entries.size(); ++i) {
    const auto& a = index.entries[i];
    const auto& b = decoded.entries[i];
    assert(a.path == b.path && a.title == b.title && a.author == b.author && a.addedSeq == b.addedSeq);
    assert(a.status == b.status && a.metadataLoaded == b.metadataLoaded && a.coverMissing == b.coverMissing);
    assert(b.recentRank == -1);
  }

  // UTF-8 clamp never splits a character: 100 x "é" (2 bytes each) -> 47 characters = 94 bytes.
  LibraryIndexData accents;
  LibraryEntry e;
  e.path = "/x.epub";
  for (int i = 0; i < 100; ++i) e.title += "\xC3\xA9";
  accents.entries.push_back(e);
  VectorWriter accentWriter;
  assert(LibraryIndexCodec::encode(accents, accentWriter));
  LibraryIndexData accentsDecoded;
  VectorReader accentReader(accentWriter.bytes);
  assert(LibraryIndexCodec::decode(accentReader, accentsDecoded));
  assert(accentsDecoded.entries[0].title.size() == 94);

  // Corrupt input.
  std::vector<uint8_t> bad = writer.bytes;
  bad[0] = 'Y';
  VectorReader badReader(bad);
  LibraryIndexData out;
  assert(!LibraryIndexCodec::decode(badReader, out));
  std::vector<uint8_t> truncated(writer.bytes.begin(), writer.bytes.end() - 3);
  VectorReader truncatedReader(truncated);
  assert(!LibraryIndexCodec::decode(truncatedReader, out));
  std::vector<uint8_t> wrongVersion = writer.bytes;
  wrongVersion[4] = 99;
  VectorReader versionReader(wrongVersion);
  assert(!LibraryIndexCodec::decode(versionReader, out));
  return 0;
}
