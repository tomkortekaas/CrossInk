// Shared 1-bpp framebuffer helpers for the navigator host tests.
//
// The physical buffer is row-major 1-bpp MSB-first (bit 7 of each byte =
// leftmost pixel), 1 = white, 0 = black, stride ceil(widthPx / 8). The
// navigator authors screens in logical PORTRAIT coordinates of heightPx x
// widthPx and stores logical (lx, ly) at the physical pixel
// (ly, physH - 1 - lx). Both physical and logical probe helpers live here;
// the logical helpers keep dynamic-content tests readable without threading
// the rotation through every assertion.
#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace navtest {

inline constexpr int kGuard = 16;
inline constexpr uint8_t kSentinel = 0xA5;

constexpr size_t rowBytes(int widthPx) { return (static_cast<size_t>(widthPx) + 7U) / 8U; }

struct Frame {
  int width;
  int height;
  std::vector<uint8_t> bytes;  // [guard][framebuffer][guard]

  Frame(int w, int h, uint8_t fill) : width(w), height(h) {
    bytes.assign(static_cast<size_t>(kGuard) + rowBytes(w) * static_cast<size_t>(h) + static_cast<size_t>(kGuard),
                 fill);
  }

  uint8_t* pixels() { return bytes.data() + kGuard; }
  const uint8_t* pixels() const { return bytes.data() + kGuard; }
};

inline bool pixelIsBlack(const Frame& frame, int x, int y) {
  if (x < 0 || y < 0 || x >= frame.width || y >= frame.height) {
    return false;
  }
  const uint8_t* p = frame.pixels();
  const size_t wb = rowBytes(frame.width);
  const uint8_t byte = p[static_cast<size_t>(y) * wb + static_cast<size_t>(x) / 8U];
  return ((byte >> (7 - (x & 7))) & 1U) == 0U;
}

inline int countBlack(const Frame& frame, int x0, int y0, int x1, int y1) {
  int black = 0;
  for (int y = std::max(0, y0); y < y1 && y < frame.height; ++y) {
    for (int x = std::max(0, x0); x < x1 && x < frame.width; ++x) {
      black += pixelIsBlack(frame, x, y) ? 1 : 0;
    }
  }
  return black;
}

// Logical portrait coordinates: logical (lx, ly) -> physical (ly, h - 1 - lx).
inline int physX(int ly) { return ly; }
inline int physY(int lx, int physH) { return physH - 1 - lx; }

inline bool logicalPixelIsBlack(const Frame& frame, int lx, int ly) {
  if (lx < 0 || ly < 0 || lx >= frame.height || ly >= frame.width) {
    return false;
  }
  return pixelIsBlack(frame, physX(ly), physY(lx, frame.height));
}

inline int countLogicalBlack(const Frame& frame, int lx0, int ly0, int lx1, int ly1) {
  int black = 0;
  for (int ly = std::max(0, ly0); ly < ly1 && ly < frame.width; ++ly) {
    for (int lx = std::max(0, lx0); lx < lx1 && lx < frame.height; ++lx) {
      black += logicalPixelIsBlack(frame, lx, ly) ? 1 : 0;
    }
  }
  return black;
}

// Bounding logical-x span of black pixels within logical rows [ly0, ly1) and
// logical columns [lx0, lx1). Returns false when the band is empty.
inline bool logicalSpan(const Frame& frame, int ly0, int ly1, int lx0, int lx1, int* minLx, int* maxLx) {
  bool found = false;
  int lo = 0;
  int hi = 0;
  for (int ly = std::max(0, ly0); ly < ly1 && ly < frame.width; ++ly) {
    for (int lx = std::max(0, lx0); lx < lx1 && lx < frame.height; ++lx) {
      if (logicalPixelIsBlack(frame, lx, ly)) {
        if (!found) {
          lo = lx;
          hi = lx;
          found = true;
        } else {
          lo = std::min(lo, lx);
          hi = std::max(hi, lx);
        }
      }
    }
  }
  *minLx = lo;
  *maxLx = hi;
  return found;
}

inline void expectBlack(const Frame& frame, int x, int y, const char* label) {
  SCOPED_TRACE(label);
  EXPECT_TRUE(pixelIsBlack(frame, x, y)) << "expected black at (" << x << "," << y << ")";
}

inline void expectWhite(const Frame& frame, int x, int y, const char* label) {
  SCOPED_TRACE(label);
  EXPECT_FALSE(pixelIsBlack(frame, x, y)) << "expected white at (" << x << "," << y << ")";
}

inline void expectLogicalBlack(const Frame& frame, int lx, int ly, const char* label) {
  SCOPED_TRACE(label);
  EXPECT_TRUE(logicalPixelIsBlack(frame, lx, ly)) << "expected black at logical (" << lx << "," << ly << ")";
}

inline void expectLogicalWhite(const Frame& frame, int lx, int ly, const char* label) {
  SCOPED_TRACE(label);
  EXPECT_FALSE(logicalPixelIsBlack(frame, lx, ly)) << "expected white at logical (" << lx << "," << ly << ")";
}

inline void expectGuardsUntouched(const Frame& frame) {
  for (int i = 0; i < kGuard; ++i) {
    EXPECT_EQ(frame.bytes[static_cast<size_t>(i)], kSentinel) << "leading guard " << i;
    EXPECT_EQ(frame.bytes[frame.bytes.size() - 1U - static_cast<size_t>(i)], kSentinel) << "trailing guard " << i;
  }
}

// A region must hold meaningful black strokes and white gaps: neither empty
// nor a solid block.
inline void expectTextEvidence(const Frame& frame, int x0, int y0, int x1, int y1, int minBlack, int maxBlack,
                               const char* label) {
  SCOPED_TRACE(label);
  const int total = (x1 - x0) * (y1 - y0);
  const int black = countBlack(frame, x0, y0, x1, y1);
  EXPECT_GE(black, minBlack) << "not enough black strokes in " << label;
  EXPECT_LE(black, maxBlack) << "region in " << label << " is too solid";
  EXPECT_LT(black, total) << "region in " << label << " should not be fully black";
  EXPECT_GT(total - black, 0) << "region in " << label << " has no white evidence";
}

}  // namespace navtest
