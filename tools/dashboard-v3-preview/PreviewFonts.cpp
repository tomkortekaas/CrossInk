#include "PreviewFonts.h"

#include <Utf8.h>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include <builtinFonts/lexenddeca_10_bold.h>
#include <builtinFonts/lexenddeca_10_bolditalic.h>
#include <builtinFonts/lexenddeca_10_italic.h>
#include <builtinFonts/lexenddeca_10_regular.h>
#include <builtinFonts/lexenddeca_12_bold.h>
#include <builtinFonts/lexenddeca_12_bolditalic.h>
#include <builtinFonts/lexenddeca_12_italic.h>
#include <builtinFonts/lexenddeca_12_regular.h>
#include <builtinFonts/lexenddeca_14_bold.h>
#include <builtinFonts/lexenddeca_14_bolditalic.h>
#include <builtinFonts/lexenddeca_14_italic.h>
#include <builtinFonts/lexenddeca_14_regular.h>
#include <builtinFonts/lexenddeca_16_bold.h>
#include <builtinFonts/lexenddeca_16_bolditalic.h>
#include <builtinFonts/lexenddeca_16_italic.h>
#include <builtinFonts/lexenddeca_16_regular.h>
#include <builtinFonts/lexenddeca_18_bold.h>
#include <builtinFonts/lexenddeca_18_bold_dash.h>
#include <builtinFonts/lexenddeca_18_bolditalic.h>
#include <builtinFonts/lexenddeca_18_italic.h>
#include <builtinFonts/lexenddeca_18_regular.h>
#include <builtinFonts/lexenddeca_20_bold.h>
#include <builtinFonts/lexenddeca_20_bolditalic.h>
#include <builtinFonts/lexenddeca_20_italic.h>
#include <builtinFonts/lexenddeca_20_regular.h>
#include <builtinFonts/lexenddeca_22_bold_dash.h>
#include <builtinFonts/lexenddeca_28_bold_dash.h>
#include <builtinFonts/lexenddeca_34_bold_dash.h>
#include <builtinFonts/lexenddeca_8_bold.h>
#include <builtinFonts/lexenddeca_9_bold.h>

#include "fontIds.h"

namespace dashboard::preview {
namespace {

// --- Glyph bitmaps ---------------------------------------------------------
//
// The built-in fonts store their bitmaps as raw-DEFLATE groups of byte-aligned
// 2-bit rows (see lib/EpdFont/scripts/fontconvert.py). On the device
// FontDecompressor unpacks a group into a cache; on the host there is no memory
// pressure, so a group is inflated once and kept for the process lifetime.

struct GroupKey {
  const EpdFontData* font;
  uint16_t group;

  bool operator<(const GroupKey& other) const {
    if (font != other.font) return font < other.font;
    return group < other.group;
  }
};

std::vector<uint8_t>* inflateGroup(const EpdFontData* font, const uint16_t groupIndex) {
  static std::map<GroupKey, std::vector<uint8_t>> cache;
  const GroupKey key{font, groupIndex};
  if (const auto it = cache.find(key); it != cache.end()) {
    return &it->second;
  }

  const EpdFontGroup& group = font->groups[groupIndex];
  std::vector<uint8_t> out(group.uncompressedSize);

  z_stream stream{};
  // Negative window bits select raw DEFLATE, matching fontconvert.py's
  // zlib.compressobj(wbits=-15) and the device's uzlib reader.
  if (inflateInit2(&stream, -15) != Z_OK) return nullptr;
  stream.next_in = const_cast<Bytef*>(font->bitmap + group.compressedOffset);
  stream.avail_in = group.compressedSize;
  stream.next_out = out.data();
  stream.avail_out = static_cast<uInt>(out.size());
  const int status = inflate(&stream, Z_FINISH);
  inflateEnd(&stream);
  if (status != Z_STREAM_END && stream.total_out != out.size()) return nullptr;

  return &cache.emplace(key, std::move(out)).first->second;
}

uint16_t groupIndexFor(const EpdFontData* font, const uint32_t glyphIndex) {
  if (font->glyphToGroup != nullptr) return font->glyphToGroup[glyphIndex];
  for (uint16_t i = 0; i < font->groupCount; ++i) {
    const uint32_t first = font->groups[i].firstGlyphIndex;
    if (glyphIndex >= first && glyphIndex < first + font->groups[i].glyphCount) return i;
  }
  return font->groupCount;
}

// Byte offset of this glyph's rows within its inflated group. Mirrors
// FontDecompressor::getAlignedOffset.
uint32_t alignedOffsetFor(const EpdFontData* font, const uint16_t groupIndex, const uint32_t glyphIndex) {
  uint32_t offset = 0;
  const auto accumulate = [&offset](const EpdGlyph& glyph) {
    if (glyph.width > 0 && glyph.height > 0) offset += ((glyph.width + 3) / 4) * glyph.height;
  };
  if (font->glyphToGroup != nullptr) {
    for (uint32_t i = 0; i < glyphIndex; ++i) {
      if (font->glyphToGroup[i] == groupIndex) accumulate(font->glyph[i]);
    }
  } else {
    for (uint32_t i = font->groups[groupIndex].firstGlyphIndex; i < glyphIndex; ++i) accumulate(font->glyph[i]);
  }
  return offset;
}

// Draws one glyph with its origin on the baseline at (cursorX, baselineY),
// mirroring renderCharImpl's unrotated branch. The device thresholds a 2-bit
// coverage value against its render mode; the panel is 1-bit, so any coverage
// at all becomes ink.
void drawGlyph(Framebuffer& canvas, const EpdFontData* font, const EpdGlyph* glyph, const int cursorX,
               const int baselineY, const bool black) {
  if (glyph->width == 0 || glyph->height == 0) return;

  const uint8_t* rows = nullptr;
  uint32_t rowStride = 0;
  std::vector<uint8_t>* group = nullptr;

  if (font->groups != nullptr) {
    const auto glyphIndex = static_cast<uint32_t>(glyph - font->glyph);
    const uint16_t groupIndex = groupIndexFor(font, glyphIndex);
    if (groupIndex >= font->groupCount) return;
    group = inflateGroup(font, groupIndex);
    if (group == nullptr) return;
    const uint32_t offset = alignedOffsetFor(font, groupIndex, glyphIndex);
    if (offset >= group->size()) return;
    rows = group->data() + offset;
    rowStride = (glyph->width + 3) / 4;  // byte-aligned rows inside a group
  } else {
    rows = font->bitmap + glyph->dataOffset;
    rowStride = 0;  // continuous packing; handled below
  }

  const int originX = cursorX + glyph->left;
  const int originY = baselineY - glyph->top;

  for (int row = 0; row < glyph->height; ++row) {
    for (int column = 0; column < glyph->width; ++column) {
      bool ink = false;
      if (font->is2Bit) {
        // Grouped fonts pad every row to a byte boundary; unpacked fonts store
        // one continuous pixel stream. Both are 4 pixels per byte, MSB first,
        // where raw 0 means paper and 1..3 mean increasing coverage.
        const size_t byteIndex = rowStride > 0
                                     ? static_cast<size_t>(row) * rowStride + (column >> 2)
                                     : (static_cast<size_t>(row) * glyph->width + column) >> 2;
        const int pixelInByte =
            rowStride > 0 ? column & 3 : static_cast<int>((static_cast<size_t>(row) * glyph->width + column) & 3);
        ink = ((rows[byteIndex] >> ((3 - pixelInByte) * 2)) & 0x3) != 0;
      } else {
        const size_t position = static_cast<size_t>(row) * glyph->width + column;
        ink = ((rows[position >> 3] >> (7 - (position & 7))) & 0x1) != 0;
      }
      if (ink) canvas.setPixel(originX + column, originY + row, black);
    }
  }
}

// --- Font families ---------------------------------------------------------
//
// Same construction as src/main.cpp, so a preview run resolves "Lexend 14 bold"
// to exactly the face the firmware inserts under that id.

#define LEXEND_FULL_FAMILY(size)                                                          \
  const EpdFont lexend##size##Regular(&lexenddeca_##size##_regular);                       \
  const EpdFont lexend##size##Bold(&lexenddeca_##size##_bold);                             \
  const EpdFont lexend##size##Italic(&lexenddeca_##size##_italic);                         \
  const EpdFont lexend##size##BoldItalic(&lexenddeca_##size##_bolditalic);                 \
  const EpdFontFamily lexend##size##Family(&lexend##size##Regular, &lexend##size##Bold,     \
                                           &lexend##size##Italic, &lexend##size##BoldItalic)

LEXEND_FULL_FAMILY(10);
LEXEND_FULL_FAMILY(12);
LEXEND_FULL_FAMILY(14);
LEXEND_FULL_FAMILY(16);
LEXEND_FULL_FAMILY(18);
LEXEND_FULL_FAMILY(20);

#undef LEXEND_FULL_FAMILY

// Bold-only families: the subsetted value ladder and the 8/9 test-card sizes
// point all four styles at the same face, exactly like the firmware does.
#define LEXEND_BOLD_ONLY_FAMILY(name, data)                                            \
  const EpdFont name##Face(&data);                                                     \
  const EpdFontFamily name##Family(&name##Face, &name##Face, &name##Face, &name##Face)

LEXEND_BOLD_ONLY_FAMILY(lexend8, lexenddeca_8_bold);
LEXEND_BOLD_ONLY_FAMILY(lexend9, lexenddeca_9_bold);
LEXEND_BOLD_ONLY_FAMILY(lexend18Dash, lexenddeca_18_bold_dash);
LEXEND_BOLD_ONLY_FAMILY(lexend22Dash, lexenddeca_22_bold_dash);
LEXEND_BOLD_ONLY_FAMILY(lexend28Dash, lexenddeca_28_bold_dash);
LEXEND_BOLD_ONLY_FAMILY(lexend34Dash, lexenddeca_34_bold_dash);

#undef LEXEND_BOLD_ONLY_FAMILY

}  // namespace

// --- Framebuffer -----------------------------------------------------------

Framebuffer::Framebuffer(const int width, const int height)
    : width_(width), height_(height), pixels_(static_cast<size_t>(width) * height, 0) {}

void Framebuffer::clear(const bool black) {
  std::fill(pixels_.begin(), pixels_.end(), black ? 1 : 0);
}

void Framebuffer::setPixel(const int x, const int y, const bool black) {
  if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
  pixels_[static_cast<size_t>(y) * width_ + x] = black ? 1 : 0;
}

bool Framebuffer::pixel(const int x, const int y) const {
  if (x < 0 || y < 0 || x >= width_ || y >= height_) return false;
  return pixels_[static_cast<size_t>(y) * width_ + x] != 0;
}

void Framebuffer::fillRect(const int x, const int y, const int width, const int height, const bool black) {
  for (int row = 0; row < height; ++row) {
    for (int column = 0; column < width; ++column) setPixel(x + column, y + row, black);
  }
}

void Framebuffer::drawRect(const int x, const int y, const int width, const int height, const bool black) {
  if (width <= 0 || height <= 0) return;
  for (int column = 0; column < width; ++column) {
    setPixel(x + column, y, black);
    setPixel(x + column, y + height - 1, black);
  }
  for (int row = 0; row < height; ++row) {
    setPixel(x, y + row, black);
    setPixel(x + width - 1, y + row, black);
  }
}

void Framebuffer::drawLine(const int x1, const int y1, const int x2, const int y2, const bool black) {
  int x = x1;
  int y = y1;
  const int dx = std::abs(x2 - x1);
  const int dy = -std::abs(y2 - y1);
  const int stepX = x1 < x2 ? 1 : -1;
  const int stepY = y1 < y2 ? 1 : -1;
  int error = dx + dy;
  while (true) {
    setPixel(x, y, black);
    if (x == x2 && y == y2) break;
    const int doubled = 2 * error;
    if (doubled >= dy) {
      error += dy;
      x += stepX;
    }
    if (doubled <= dx) {
      error += dx;
      y += stepY;
    }
  }
}

bool Framebuffer::writePng(const std::string& path) const {
  // Greyscale 8-bit, one filter byte per scanline, filter type 0 (None).
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(height_) * (width_ + 1));
  for (int y = 0; y < height_; ++y) {
    raw.push_back(0);
    for (int x = 0; x < width_; ++x) raw.push_back(pixel(x, y) ? 0x00 : 0xFF);
  }

  uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
  std::vector<uint8_t> compressed(compressedSize);
  if (compress2(compressed.data(), &compressedSize, raw.data(), static_cast<uLong>(raw.size()), 9) != Z_OK) {
    return false;
  }
  compressed.resize(compressedSize);

  const auto beU32 = [](std::vector<uint8_t>& out, const uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
  };
  const auto appendChunk = [&beU32](std::vector<uint8_t>& out, const char (&type)[5],
                                    const std::vector<uint8_t>& payload) {
    beU32(out, static_cast<uint32_t>(payload.size()));
    std::vector<uint8_t> typed(type, type + 4);
    typed.insert(typed.end(), payload.begin(), payload.end());
    out.insert(out.end(), typed.begin(), typed.end());
    beU32(out, static_cast<uint32_t>(crc32(0, typed.data(), static_cast<uInt>(typed.size()))));
  };

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> header;
  beU32(header, static_cast<uint32_t>(width_));
  beU32(header, static_cast<uint32_t>(height_));
  header.push_back(8);  // bit depth
  header.push_back(0);  // colour type: greyscale
  header.push_back(0);  // compression
  header.push_back(0);  // filter
  header.push_back(0);  // interlace
  appendChunk(png, "IHDR", header);
  appendChunk(png, "IDAT", compressed);
  appendChunk(png, "IEND", {});

  FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return false;
  const size_t written = std::fwrite(png.data(), 1, png.size(), file);
  std::fclose(file);
  return written == png.size();
}

// --- FontBook --------------------------------------------------------------

FontBook::FontBook()
    : entries_{
          {LEXENDDECA_8_FONT_ID, &lexend8Family},
          {LEXENDDECA_9_FONT_ID, &lexend9Family},
          {LEXENDDECA_10_FONT_ID, &lexend10Family},
          {LEXENDDECA_12_FONT_ID, &lexend12Family},
          {LEXENDDECA_14_FONT_ID, &lexend14Family},
          {LEXENDDECA_16_FONT_ID, &lexend16Family},
          {LEXENDDECA_18_FONT_ID, &lexend18Family},
          {LEXENDDECA_20_FONT_ID, &lexend20Family},
          {LEXENDDECA_18_BOLD_DASH_FONT_ID, &lexend18DashFamily},
          {LEXENDDECA_22_BOLD_DASH_FONT_ID, &lexend22DashFamily},
          {LEXENDDECA_28_BOLD_DASH_FONT_ID, &lexend28DashFamily},
          {LEXENDDECA_34_BOLD_DASH_FONT_ID, &lexend34DashFamily},
      } {}

const EpdFontFamily* FontBook::family(const int fontId) const {
  for (const Entry& entry : entries_) {
    if (entry.fontId == fontId) return entry.family;
  }
  return nullptr;
}

int FontBook::textWidth(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  if (text == nullptr || *text == '\0') return 0;
  const EpdFontFamily* font = family(fontId);
  if (font == nullptr) return 0;
  int width = 0;
  int height = 0;
  font->getTextDimensions(text, &width, &height, style);
  return width;
}

int FontBook::ascender(const int fontId) const {
  const EpdFontFamily* font = family(fontId);
  return font == nullptr ? 0 : font->getData(EpdFontFamily::REGULAR)->ascender;
}

int FontBook::lineHeight(const int fontId) const {
  const EpdFontFamily* font = family(fontId);
  return font == nullptr ? 0 : font->getData(EpdFontFamily::REGULAR)->advanceY;
}

std::string FontBook::truncated(const int fontId, const char* text, const int maxWidth,
                                const EpdFontFamily::Style style) const {
  if (text == nullptr || maxWidth <= 0) return "";
  std::string item = text;
  const char* ellipsis = "\xe2\x80\xa6";  // U+2026, same as GfxRenderer
  if (textWidth(fontId, item.c_str(), style) <= maxWidth) return item;
  while (!item.empty() && textWidth(fontId, (item + ellipsis).c_str(), style) >= maxWidth) {
    utf8RemoveLastChar(item);
  }
  return item.empty() ? ellipsis : item + ellipsis;
}

void FontBook::drawText(Framebuffer& canvas, const int fontId, const int x, const int y, const char* text,
                        const bool black, const EpdFontFamily::Style style) const {
  if (text == nullptr || *text == '\0') return;
  const EpdFontFamily* font = family(fontId);
  if (font == nullptr) return;

  // drawText's y is the top of the ascender box, not the baseline.
  const int baselineY = y + font->getData(EpdFontFamily::REGULAR)->ascender;
  int cursorX = x;
  int32_t previousAdvanceFP = 0;
  uint32_t previousCp = 0;

  const char* cursor = text;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const unsigned char**>(&cursor))) != 0) {
    cp = font->applyLigatures(cp, cursor, style);
    cp = font->getFallbackCodepoint(cp, style);

    // Differential rounding: the previous advance and this pair's kern are
    // snapped to a pixel together, so identical pairs always step identically.
    if (previousCp != 0) {
      const int32_t kernFP = font->getKerning(previousCp, cp, style);
      cursorX += fp4::toPixel(previousAdvanceFP + kernFP);
    }

    const EpdFontFamily::GlyphData resolved = font->getGlyphData(cp, style);
    if (resolved.glyph == nullptr) {
      previousCp = 0;
      previousAdvanceFP = 0;
      continue;
    }

    drawGlyph(canvas, resolved.fontData, resolved.glyph, cursorX, baselineY, black);
    previousAdvanceFP = resolved.glyph->advanceX;
    previousCp = cp;
  }
}

bool hasMissingGlyph(const FontBook& fonts, const int fontId, const char* text, const EpdFontFamily::Style style) {
  const EpdFontFamily* font = fonts.family(fontId);
  if (font == nullptr || text == nullptr) return false;
  const char* cursor = text;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const unsigned char**>(&cursor))) != 0) {
    if (cp == ' ') continue;
    if (font->findGlyphData(cp, style).glyph == nullptr) return true;
  }
  return false;
}

}  // namespace dashboard::preview
