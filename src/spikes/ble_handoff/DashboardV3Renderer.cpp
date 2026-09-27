#include "DashboardV3Renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "DashboardDateFields.h"
#include "DashboardV3Quotes.h"

namespace dashboard::v3 {
namespace {

constexpr int PAD = 14;

// Height a text box must reserve for a rung, measured from the built-in faces.
// drawText() places the top of the ascender box at the given y, so a row that
// puts something else at y + ROW_HEIGHT[rung] cannot collide with the text.
// Verified by tools/dashboard-v3-preview, which prints the same numbers.
constexpr int ascenderFor(const FontRole role) {
  switch (role) {
    case FontRole::Micro: return 17;
    case FontRole::Small: return 19;
    case FontRole::Body: return 21;
    case FontRole::Heading: return 25;
    case FontRole::Value: return 30;
    case FontRole::Hero: return 34;
  }
  return 21;
}

// Icon ids from the dashboard icon catalog (dashboardIconTable.h). The
// catalog is generated from dashboard-icons.txt; these constants are the
// stable ids the wire format and the icon table agree on.
constexpr uint8_t ICON_SUN = 1;
constexpr uint8_t ICON_CLOUD_RAIN = 3;
constexpr uint8_t ICON_WIND = 6;
constexpr uint8_t ICON_SUNRISE = 11;
constexpr uint8_t ICON_SUNSET = 12;
constexpr uint8_t ICON_BATTERY = 17;
constexpr uint8_t ICON_FLAME = 14;
constexpr uint8_t ICON_HOUSE = 20;
constexpr uint8_t ICON_CAR = 25;
constexpr uint8_t ICON_TRAFFIC_CONE = 27;
constexpr uint8_t ICON_TRENDING_UP = 33;
constexpr uint8_t ICON_FOOTPRINTS = 47;
constexpr uint8_t ICON_MESSAGE = 65;

constexpr int ICON_SMALL = 24;
constexpr int ICON_LARGE = 32;

// Each rain bucket covers five minutes, so the twenty-four buckets are the two
// hours the band is labelled with. The strip groups them in pairs: ten-minute
// blocks are wide enough to read at arm's length, where twenty-four slivers on
// a 500 px strip turn into noise.
constexpr int RAIN_MINUTES_PER_BUCKET = 5;
constexpr int RAIN_BUCKETS_PER_SEGMENT = 2;
constexpr int RAIN_SEGMENTS = static_cast<int>(RAIN_BUCKET_COUNT) / RAIN_BUCKETS_PER_SEGMENT;
constexpr int RAIN_SEGMENT_GAP = 2;
constexpr int RAIN_STRIP_TOP = 40;
constexpr int RAIN_STRIP_HEIGHT = 24;

void label(DashboardV3Canvas& canvas, const Rect bounds, const char* value, const FontRole font = FontRole::Body,
           const bool bold = false, const bool black = true, const TextAlign align = TextAlign::Left,
           const bool dithered = false) {
  canvas.text({bounds, font, align, bold, black, dithered}, value);
}

/// A text box that starts at (x, y) and is exactly tall enough for its rung, so
/// callers reason about rows in real pixels instead of guessing a height.
Rect textBox(const int x, const int y, const int width, const FontRole font) {
  return {x, y, width, ascenderFor(font)};
}

/// A spec for asking the canvas how wide a string would be. The bounds carry no
/// width on purpose: measureText() only ever reads the rung and the weight, and
/// a zero-width box cannot be mistaken for a box something was drawn into.
TextSpec measureSpec(const int y, const FontRole role, const bool bold) {
  return TextSpec{{0, y, 0, ascenderFor(role)}, role, TextAlign::Left, bold, true};
}

void formatPercent(const uint8_t value, char (&out)[8]) {
  if (value == UINT8_MAX) {
    std::snprintf(out, sizeof(out), "-");
  } else {
    std::snprintf(out, sizeof(out), "%u%%", static_cast<unsigned>(value));
  }
}

/// A basis-point change as an unsigned percentage. The triangle beside it
/// carries the direction, so a sign would say the same thing twice.
void formatChange(const int16_t basisPoints, char (&out)[16]) {
  const int value = basisPoints;
  std::snprintf(out, sizeof(out), "%d,%02d%%", std::abs(value) / 100, std::abs(value) % 100);
}

template <size_t Size>
void copyField(const std::array<uint8_t, Size>& source, const uint8_t length, char (&out)[Size + 1]) {
  const size_t boundedLength = std::min(static_cast<size_t>(length), Size);
  std::memcpy(out, source.data(), boundedLength);
  out[boundedLength] = '\0';
}

/// True when a decoded field carries at least one printable byte. The phone
/// trims a location before it puts it on the wire, so a detail that is only
/// padding is not a location: printing it would leave a bare middle dot or a
/// leading gap where a room belongs.
bool hasVisibleText(const char* const value) {
  for (const char* cursor = value; *cursor != '\0'; ++cursor) {
    if (*cursor != ' ' && *cursor != '\t' && *cursor != '\n' && *cursor != '\r') return true;
  }
  return false;
}

void formatMinute(const uint16_t minute, char (&out)[8]) {
  std::snprintf(out, sizeof(out), "%02u:%02u", static_cast<unsigned>(minute / 60),
                static_cast<unsigned>(minute % 60));
}

/// A solid triangle filling `bounds`, drawn as one filled row per scanline.
/// The market rows need an up/down mark and the Lexend faces carry no U+25B2 /
/// U+25BC, which would land on the panel as a replacement box.
void triangle(DashboardV3Canvas& canvas, const Rect bounds, const bool pointingUp) {
  if (bounds.width <= 0 || bounds.height <= 0) return;
  for (int row = 0; row < bounds.height; ++row) {
    // Row 0 is the apex for an up triangle and the base for a down one.
    const int grown = pointingUp ? row + 1 : bounds.height - row;
    const int rowWidth = std::max(1, grown * bounds.width / bounds.height);
    canvas.fill({bounds.x + (bounds.width - rowWidth) / 2, bounds.y + row, rowWidth, 1}, true);
  }
}

struct CivilDate {
  int year;
  int month;
  int day;
};

// Howard Hinnant's civil_from_days, expressed against the Unix epoch
// (1970-01-01 is day 0). Pure integer arithmetic: no <ctime>, no timezone
// database, so the header can format the package's generatedAt on the C3
// without pulling in the SDK. generatedAt is treated as UTC epoch seconds;
// the package carries no timezone field, which is the only way to interpret
// it on the receiver.
CivilDate civilFromDays(int64_t days) {
  int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return {static_cast<int>(y + (m <= 2)), static_cast<int>(m), static_cast<int>(d)};
}

void uppercaseAscii(const char* input, char (&output)[8]) {
  size_t index = 0;
  for (; input[index] != '\0' && index + 1 < sizeof(output); ++index) {
    const char c = input[index];
    output[index] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  output[index] = '\0';
}

/// The package timestamp shifted into the panel's local time. `generatedAt` is
/// UTC epoch seconds and the wire carries no timezone; the device knows its own
/// offset, so this is where the two are reconciled. Everything downstream — the
/// header date, the day separators, the "ververst" time — reads local.
uint64_t localGeneratedAt(const uint64_t generatedAt, const uint8_t utcOffsetQ) {
  if (generatedAt == 0) return 0;
  const int quarters = utcOffsetQ <= 104 ? static_cast<int>(utcOffsetQ) : UTC_OFFSET_Q_UTC;
  const int64_t shifted = static_cast<int64_t>(generatedAt) + (quarters - UTC_OFFSET_Q_UTC) * 15LL * 60LL;
  return shifted < 0 ? 0 : static_cast<uint64_t>(shifted);
}

/// A compact Dutch date, `dayOffset` days after the package timestamp. Offset 0
/// is the header's date; the agenda uses larger offsets for its day separators.
/// A zero timestamp means the phone never sent a usable time, so a dash is shown
/// rather than an invented date (same rule the date widget uses).
void formatDate(const uint64_t generatedAt, const int dayOffset, char (&dateLabel)[16]) {
  if (generatedAt == 0) {
    std::snprintf(dateLabel, sizeof(dateLabel), "-");
    return;
  }
  const int64_t days = static_cast<int64_t>(generatedAt / 86400ULL) + dayOffset;
  const CivilDate date = civilFromDays(days);
  // Epoch day 0 (1970-01-01) was a Thursday; Weekday::Thursday == 3.
  const auto weekday = static_cast<dashboard::Weekday>(((days % 7) + 7 + 3) % 7);
  char weekdayUpper[8];
  char monthUpper[8];
  uppercaseAscii(dashboard::weekdayAbbreviation(weekday), weekdayUpper);
  uppercaseAscii(dashboard::monthAbbreviation(static_cast<uint8_t>(date.month)), monthUpper);
  std::snprintf(dateLabel, sizeof(dateLabel), "%s %d %s", weekdayUpper, date.day, monthUpper);
}

/// Dutch 16-point compass rose. The wire carries a sector index, not degrees,
/// so a value outside the rose is missing data rather than a rounding error.
const char* windRose(const uint8_t sector) {
  static const char* const kRose[16] = {"N",  "NNO", "NO", "ONO", "O",  "OZO", "ZO", "ZZO",
                                        "Z",  "ZZW", "ZW", "WZW", "W",  "WNW", "NW", "NNW"};
  return sector < 16 ? kRose[sector] : "";
}

void formatSteps(const uint16_t steps, char (&out)[16]) {
  if (steps == UINT16_MAX) {
    std::snprintf(out, sizeof(out), "-");
    return;
  }
  if (steps < 1000) {
    std::snprintf(out, sizeof(out), "%u", static_cast<unsigned>(steps));
    return;
  }
  std::snprintf(out, sizeof(out), "%u.%03u", static_cast<unsigned>(steps / 1000),
                static_cast<unsigned>(steps % 1000));
}

// Width of a meter row's right-hand value. "7.850" is the widest thing that
// lands there, so the slot is sized for it rather than for "100%".
constexpr int METER_VALUE_WIDTH = 64;

/// One status row: icon, name and value share the top line, the bar sits below
/// the name's ascender box. Drawing the bar inside that box is what used to
/// strike a line through "X3", "AUTO" and "THUIS".
///
/// `valueText` and `fillPercentage` are separate because they are not always the
/// same number: a battery shows the percentage its bar draws, while steps show
/// the count and let the bar carry progress towards the goal. A negative
/// percentage leaves the track empty.
void meterRow(DashboardV3Canvas& canvas, const Rect bounds, const char* name, const uint8_t iconId,
              const char* valueText, const int fillPercentage) {
  const int textLeft = bounds.x + ICON_SMALL + 8;
  const int textWidth = bounds.width - ICON_SMALL - 8;
  canvas.icon(iconId, {bounds.x, bounds.y, ICON_SMALL, ICON_SMALL}, true);
  label(canvas, textBox(textLeft, bounds.y + 2, textWidth - METER_VALUE_WIDTH, FontRole::Micro), name,
        FontRole::Micro, true);
  label(canvas, textBox(bounds.x + bounds.width - METER_VALUE_WIDTH, bounds.y, METER_VALUE_WIDTH, FontRole::Small),
        valueText, FontRole::Small, true, true, TextAlign::Right);

  const int barY = bounds.y + ascenderFor(FontRole::Micro) + 6;
  const int barHeight = 8;
  canvas.rect({textLeft, barY, textWidth, barHeight}, true);
  if (fillPercentage > 0) {
    const int filled = (textWidth - 2) * std::min(fillPercentage, 100) / 100;
    if (filled > 0) canvas.fill({textLeft + 1, barY + 1, filled, barHeight - 2}, true);
  }
}

/// The bar fraction for a battery reading, or -1 when the value is absent.
int meterFillFor(const uint8_t percentage) {
  return percentage == UINT8_MAX ? -1 : static_cast<int>(percentage);
}

void renderHeader(DashboardV3Canvas& canvas, const Rect rect, const DashboardV3Package& package,
                  const uint16_t minuteOfDay, const uint64_t localGenerated) {
  canvas.fill(rect, true);

  // Four columns sized to their content rather than split evenly: the date is
  // the longest string in the band and the wind the shortest, so equal quarters
  // truncate the date while leaving the wind column half empty.
  const int columnX[5] = {rect.x, rect.x + rect.width * 33 / 100, rect.x + rect.width * 53 / 100,
                          rect.x + rect.width * 75 / 100, rect.x + rect.width};
  for (int column = 1; column < 4; ++column) {
    canvas.line(columnX[column], rect.y + 10, columnX[column], rect.y + rect.height - 10, false);
  }

  const int valueY = rect.y + 12;
  const int captionY = rect.y + 12 + ascenderFor(FontRole::Heading) + 4;

  // Column 1 - date with today's low/high underneath.
  {
    const int x = columnX[0] + PAD;
    const int width = columnX[1] - columnX[0] - PAD - 8;
    char dateLabel[16];
    formatDate(localGenerated, 0, dateLabel);
    label(canvas, textBox(x, valueY, width, FontRole::Heading), dateLabel, FontRole::Heading, true, false);
    char range[20] = "-";
    if (package.weather.minimumCelsius != INT8_MIN && package.weather.maximumCelsius != INT8_MIN) {
      std::snprintf(range, sizeof(range), "%d\xc2\xb0 / %d\xc2\xb0", package.weather.minimumCelsius,
                    package.weather.maximumCelsius);
    }
    label(canvas, textBox(x, captionY, width, FontRole::Micro), range, FontRole::Micro, false, false);
  }

  // Column 2 - current weather. The package carries no free-text description,
  // so the caption names the reading instead of inventing one.
  {
    const int x = columnX[1] + 10;
    const int width = columnX[2] - columnX[1] - 18;
    if (package.weather.conditionIconId != 0) {
      canvas.icon(package.weather.conditionIconId, {x, rect.y + 14, ICON_LARGE, ICON_LARGE}, false);
    }
    const int textX = x + ICON_LARGE + 6;
    const int textWidth = width - ICON_LARGE - 6;
    char temperature[12] = "-";
    if (package.weather.currentCelsius != INT8_MIN) {
      std::snprintf(temperature, sizeof(temperature), "%d\xc2\xb0", package.weather.currentCelsius);
    }
    label(canvas, textBox(textX, valueY, textWidth, FontRole::Heading), temperature, FontRole::Heading, true, false);
    // The column's subject, not a description: the package has no condition
    // text, and naming the reading is honest where inventing "ZONNIG" is not.
    // Like the other captions it takes the whole column, including the space
    // under the icon, so it never has to compete with the value for width.
    label(canvas, textBox(x, captionY, width, FontRole::Micro), "WEER", FontRole::Micro, false, false);
  }

  // Column 3 - wind speed. The compass sector rides in the caption: at 150 km/h
  // "150 NNW" is wider than the column, and the speed is the number that matters.
  {
    const int x = columnX[2] + 8;
    const int width = columnX[3] - columnX[2] - 14;
    canvas.icon(ICON_WIND, {x, rect.y + 14, ICON_SMALL, ICON_SMALL}, false);
    const int textX = x + ICON_SMALL + 4;
    char wind[12] = "-";
    char caption[16] = "KM/U";
    if (package.weather.windKilometersPerHour != UINT8_MAX) {
      std::snprintf(wind, sizeof(wind), "%u", static_cast<unsigned>(package.weather.windKilometersPerHour));
      const char* rose = windRose(package.weather.windDirection);
      if (rose[0] != '\0') std::snprintf(caption, sizeof(caption), "KM/U %s", rose);
    }
    label(canvas, textBox(textX, valueY, width - ICON_SMALL - 4, FontRole::Heading), wind, FontRole::Heading, true,
          false);
    // The caption gets the whole column, including the space under the icon.
    label(canvas, textBox(x, captionY, width, FontRole::Micro), caption, FontRole::Micro, false, false);
  }

  // Column 4 - the next sun event, chosen against the device's own clock so the
  // header stays correct between phone packages.
  {
    const int x = columnX[3] + 8;
    const int width = columnX[4] - columnX[3] - 8 - PAD;
    uint16_t sunMinute = UINT16_MAX;
    uint8_t sunIconId = ICON_SUN;
    const char* caption = "ZON";
    if (package.weather.sunriseTodayMinute != UINT16_MAX && minuteOfDay < package.weather.sunriseTodayMinute) {
      sunMinute = package.weather.sunriseTodayMinute;
      caption = "ZON OP";
      sunIconId = ICON_SUNRISE;
    } else if (package.weather.sunsetTodayMinute != UINT16_MAX && minuteOfDay < package.weather.sunsetTodayMinute) {
      sunMinute = package.weather.sunsetTodayMinute;
      caption = "ZON ONDER";
      sunIconId = ICON_SUNSET;
    } else if (package.weather.sunriseTomorrowMinute != UINT16_MAX) {
      sunMinute = package.weather.sunriseTomorrowMinute;
      caption = "MORGEN OP";
      sunIconId = ICON_SUNRISE;
    }
    canvas.icon(sunIconId, {x, rect.y + 14, ICON_SMALL, ICON_SMALL}, false);
    const int textX = x + ICON_SMALL + 4;
    char sunTime[8] = "-";
    if (sunMinute != UINT16_MAX) formatMinute(sunMinute, sunTime);
    label(canvas, textBox(textX, valueY, width - ICON_SMALL - 4, FontRole::Heading), sunTime, FontRole::Heading, true,
          false, TextAlign::Right);
    // "ZON ONDER" is wider than the space beside the icon, so the caption takes
    // the whole column; the icon already distinguishes sunrise from sunset.
    label(canvas, textBox(x, captionY, width, FontRole::Micro), caption, FontRole::Micro, false, false,
          TextAlign::Right);
  }
}

/// Describes the two-hour window with an absolute clock time anchored to the
/// package's rainStartMinute rather than the panel's own clock, which can be a
/// quarter hour ahead of the data. The four outcomes are:
///   - all dry:    "DROOG TOT HH:MM"          (end of the window)
///   - all wet:    "REGEN TOT NA HH:MM"       (rain continues past the window)
///   - dry now:    "HH:MM LICHTE|MATIGE|HEVIGE REGEN" (first wet bucket, peak of
///                 the contiguous wet run that starts there)
///   - wet now:    "HH:MM DROOG"              (first dry bucket)
/// All times are modulo 24 hours.
///
/// `compact` words the same four outcomes for the 8A band's single line:
/// lowercase, because that line also carries the absolute clock and reads as a
/// sentence next to the strip rather than as a caption above it. The decision
/// tree, the buckets it reads and the times it prints are identical either way.
template <size_t Size>
bool describeRain(const std::array<uint8_t, RAIN_BUCKET_COUNT>& buckets, const uint16_t rainStartMinute,
                  char (&out)[Size], const bool compact = false) {
  const auto timeAt = [&](const size_t bucketIndex, char (&time)[8]) {
    const uint16_t minute =
        static_cast<uint16_t>((static_cast<uint32_t>(rainStartMinute) + bucketIndex * RAIN_MINUTES_PER_BUCKET) % 1440);
    formatMinute(minute, time);
  };

  size_t firstWet = 0;
  while (firstWet < RAIN_BUCKET_COUNT && buckets[firstWet] == 0) ++firstWet;

  if (firstWet == RAIN_BUCKET_COUNT) {
    char endTime[8];
    timeAt(RAIN_BUCKET_COUNT, endTime);
    if (compact) {
      std::snprintf(out, sizeof(out), "droog tot %s", endTime);
    } else {
      std::snprintf(out, sizeof(out), "DROOG TOT %s", endTime);
    }
    return true;
  }

  if (firstWet == 0) {
    size_t firstDry = 0;
    while (firstDry < RAIN_BUCKET_COUNT && buckets[firstDry] > 0) ++firstDry;
    if (firstDry == RAIN_BUCKET_COUNT) {
      char endTime[8];
      timeAt(RAIN_BUCKET_COUNT, endTime);
      if (compact) {
        std::snprintf(out, sizeof(out), "regen tot na %s", endTime);
      } else {
        std::snprintf(out, sizeof(out), "REGEN TOT NA %s", endTime);
      }
      return true;
    }
    char dryTime[8];
    timeAt(firstDry, dryTime);
    if (compact) {
      std::snprintf(out, sizeof(out), "%s droog", dryTime);
    } else {
      std::snprintf(out, sizeof(out), "%s DROOG", dryTime);
    }
    return true;
  }

  // Dry now, rain coming: the word follows the peak of the contiguous wet run
  // that begins at firstWet, exactly as RainForecast.outlook does in the app.
  // Anything after the first dry bucket belongs to a later shower and must not
  // influence this headline.
  uint8_t peak = 0;
  size_t cursor = firstWet;
  while (cursor < RAIN_BUCKET_COUNT && buckets[cursor] > 0) {
    peak = std::max(peak, buckets[cursor]);
    ++cursor;
  }
  const char* word = compact ? (peak >= 3 ? "hevige" : peak == 2 ? "matige" : "lichte")
                             : (peak >= 3 ? "HEVIGE" : peak == 2 ? "MATIGE" : "LICHTE");
  char startTime[8];
  timeAt(firstWet, startTime);
  if (compact) {
    std::snprintf(out, sizeof(out), "%s %s regen", startTime, word);
  } else {
    std::snprintf(out, sizeof(out), "%s %s REGEN", startTime, word);
  }
  return true;
}

void renderRain(DashboardV3Canvas& canvas, const Rect rect, const DashboardV3Package& package) {
  // "Every bucket is zero" is a forecast, not an absence: it means two dry
  // hours. Only the wire flag can tell the difference, and reading it wrong is
  // what put "GEEN REGENINFO" over a perfectly good dry morning. The window's
  // clock is rainStartMinute, not the panel's own: the package can be a quarter
  // hour old by the time it is drawn, so the panel clock would shift the axis.
  const bool hasRainData = package.rainKnown && package.rainStartMinute != UINT16_MAX;

  char headline[32] = "GEEN REGENINFO";
  if (hasRainData) describeRain(package.rain, package.rainStartMinute, headline);
  label(canvas, textBox(rect.x + PAD, rect.y + 10, 240, FontRole::Body), headline, FontRole::Body, true);

  // Heating badge, right-aligned against the panel edge.
  {
    const char* heating = !package.heatingKnown ? "VERWARMING -"
                          : package.heatingAllowed ? "STOKEN KAN"
                                                   : "NIET STOKEN";
    // Fixed block anchored to the right edge: icon first, then the text left
    // aligned against it, so the icon always sits next to its label instead of
    // floating wherever a right-aligned string happens to start.
    const int blockWidth = 178;
    const int blockX = rect.x + rect.width - PAD - blockWidth;
    canvas.icon(ICON_FLAME, {blockX, rect.y + 8, ICON_SMALL, ICON_SMALL}, true);
    label(canvas, textBox(blockX + ICON_SMALL + 6, rect.y + 12, blockWidth - ICON_SMALL - 6, FontRole::Micro),
          heating, FontRole::Micro, true);
  }

  // Two-hour strip. Intensity is carried by fill density inside a fixed-height
  // band rather than by bar height: the mock-up's version reads as one calm
  // line and costs 24 px where the chart cost more than twice that.
  const int stripLeft = rect.x + PAD;
  const int stripWidth = rect.width - 2 * PAD;
  const int stripTop = rect.y + RAIN_STRIP_TOP;
  if (!hasRainData) return;  // the headline already said there is nothing to show
  canvas.rect({stripLeft, stripTop, stripWidth, RAIN_STRIP_HEIGHT}, true);
  {
    const int innerTop = stripTop + 1;
    const int innerHeight = RAIN_STRIP_HEIGHT - 2;
    for (int segment = 0; segment < RAIN_SEGMENTS; ++segment) {
      // Each segment is the worst of the buckets it covers. Averaging would let
      // a five-minute downpour disappear into the half hour around it.
      uint8_t peak = 0;
      for (int bucket = 0; bucket < RAIN_BUCKETS_PER_SEGMENT; ++bucket) {
        peak = std::max(peak, package.rain[segment * RAIN_BUCKETS_PER_SEGMENT + bucket]);
      }
      // The nibble is the Buienradar intensity band, not a rescaled byte, so
      // these thresholds are the wire contract itself. The phone maps the same
      // band to 0..3; keeping the two tables separate is what let a drizzle and
      // a downpour collapse onto one half-tone. 4..15 stay valid on the wire
      // and land in Solid; they are just not produced any more.
      const Shade level = peak == 0      ? Shade::None
                          : peak < 2     ? Shade::Quarter
                          : peak < 3     ? Shade::Half
                                         : Shade::Solid;
      if (level == Shade::None) continue;
      const int x1 = stripLeft + 1 + segment * (stripWidth - 2) / RAIN_SEGMENTS;
      const int x2 = stripLeft + 1 + (segment + 1) * (stripWidth - 2) / RAIN_SEGMENTS;
      canvas.shade({x1, innerTop, std::max(1, x2 - x1 - RAIN_SEGMENT_GAP), innerHeight}, level);
    }
  }
  // The left edge marks the first bucket, rainStartMinute: this is the start of
  // the data window, not "now" (the package may already be a quarter hour old).
  canvas.fill({stripLeft + 1, stripTop - 3, 2, RAIN_STRIP_HEIGHT + 6}, true);

  // Absolute labels from the package, so the two-hour window is readable
  // without trusting the panel's own clock.
  const int scaleY = stripTop + RAIN_STRIP_HEIGHT + 8;
  char startLabel[8];
  char endLabel[8];
  formatMinute(package.rainStartMinute, startLabel);
  formatMinute(static_cast<uint16_t>((package.rainStartMinute + RAIN_BUCKET_COUNT * RAIN_MINUTES_PER_BUCKET) % 1440),
               endLabel);
  label(canvas, textBox(stripLeft, scaleY, 120, FontRole::Micro), startLabel, FontRole::Micro);
  label(canvas, textBox(stripLeft + stripWidth - 120, scaleY, 120, FontRole::Micro), endLabel, FontRole::Micro, false,
        true, TextAlign::Right);
}

void renderTraffic(DashboardV3Canvas& canvas, const Rect rect, const DashboardV3Package& package) {
  const int splitX = rect.x + rect.width / 2;
  const int captionY = rect.y + 10;
  const int valueY = captionY + ascenderFor(FontRole::Micro) + 2;

  // Left subject - the commute: destination as caption, travel time as the
  // dominant value.
  {
    const int x = rect.x + PAD;
    canvas.icon(ICON_CAR, {x, rect.y + 20, ICON_LARGE, ICON_LARGE}, true);
    const int textX = x + ICON_LARGE + 8;
    const int textWidth = splitX - textX - 10;
    char destination[MAX_DESTINATION_BYTES + 1] = "REISTIJD";
    if (package.traffic.destinationLength > 0) {
      copyField(package.traffic.destination, package.traffic.destinationLength, destination);
    }
    label(canvas, textBox(textX, captionY, textWidth, FontRole::Micro), destination, FontRole::Micro, true);
    // A missing reading keeps its place but not its weight: at the Hero rung a
    // lone "-" rendered on hardware as a large black block that read like a
    // deliberate graphic rather than absent data.
    if (package.traffic.travelMinutes != UINT16_MAX) {
      char travel[16];
      std::snprintf(travel, sizeof(travel), "%u min", static_cast<unsigned>(package.traffic.travelMinutes));
      label(canvas, textBox(textX, valueY, textWidth, FontRole::Hero), travel, FontRole::Hero, true);
    } else {
      label(canvas, textBox(textX, valueY + 8, textWidth, FontRole::Micro), "-", FontRole::Micro);
    }
  }

  canvas.line(splitX, rect.y + 10, splitX, rect.y + rect.height - 10, true);

  // Right subject - the national jam. The label is fixed, so an absent reading
  // shows a dash rather than borrowing the route's own delay.
  {
    const int textX = splitX + 14;
    const int right = rect.x + rect.width - PAD;
    canvas.icon(ICON_TRAFFIC_CONE, {right - ICON_SMALL, rect.y + 8, ICON_SMALL, ICON_SMALL}, true);
    label(canvas, textBox(textX, captionY, right - textX - ICON_SMALL - 6, FontRole::Micro), "FILES NEDERLAND",
          FontRole::Micro, true);

    // The badge shares the value's line, so the value box gives up its width.
    static const char* const kClassification[3] = {"NORMAAL", "DRUK", "FILE"};
    const bool hasBadge = package.traffic.classification < 3;
    const int badgeWidth = 96;
    const int valueWidth = right - textX - (hasBadge ? badgeWidth + 8 : 0);

    if (package.traffic.nationalCongestionKilometers != UINT16_MAX) {
      char congestion[20];
      std::snprintf(congestion, sizeof(congestion), "%u km",
                    static_cast<unsigned>(package.traffic.nationalCongestionKilometers));
      // A nationwide figure can reach four digits, which no longer fits the
      // Hero rung beside the badge. Step down one rung rather than truncate:
      // "1860 ..." reads as a smaller jam, not as clipped text.
      const FontRole rung = package.traffic.nationalCongestionKilometers >= 1000 ? FontRole::Value : FontRole::Hero;
      const int rungOffset = ascenderFor(FontRole::Hero) - ascenderFor(rung);
      label(canvas, textBox(textX, valueY + rungOffset, valueWidth, rung), congestion, rung, true);
    } else {
      label(canvas, textBox(textX, valueY + 8, valueWidth, FontRole::Micro), "-", FontRole::Micro);
    }

    if (hasBadge) {
      const int badgeHeight = ascenderFor(FontRole::Micro) + 8;
      const int badgeY = valueY + ascenderFor(FontRole::Hero) - badgeHeight;
      canvas.rect({right - badgeWidth, badgeY, badgeWidth, badgeHeight}, true);
      label(canvas, textBox(right - badgeWidth, badgeY + 4, badgeWidth, FontRole::Micro),
            kClassification[package.traffic.classification], FontRole::Micro, true, true, TextAlign::Center);
    }
  }
}

void renderAgenda(DashboardV3Canvas& canvas, const Rect rect, const DashboardV3Package& package,
                  const uint64_t localGenerated) {
  label(canvas, textBox(rect.x + PAD, rect.y + 10, rect.width - 2 * PAD, FontRole::Body), "KOMENDE AFSPRAKEN",
        FontRole::Body, true);
  const int headingRuleY = rect.y + 10 + ascenderFor(FontRole::Body) + 6;
  canvas.line(rect.x + PAD, headingRuleY, rect.x + rect.width - PAD, headingRuleY, true);
  if (package.agendaCount == 0) {
    label(canvas, textBox(rect.x + PAD, headingRuleY + 14, rect.width - 2 * PAD, FontRole::Micro), "GEEN AFSPRAKEN",
          FontRole::Micro);
    return;
  }

  // The agenda column is the narrowest place long Dutch titles have to live, so
  // it runs a tighter margin than the rest of the screen: the time needs 48 px
  // for "08:30" at the Micro rung and every pixel after that is title.
  const int gutter = 10;
  const int timelineX = rect.x + 66;
  const int titleX = rect.x + 74;
  const int titleWidth = rect.x + rect.width - gutter - titleX;
  const int bottom = rect.y + rect.height - 10;

  int y = headingRuleY + 14;
  int timelineTop = y + 8;
  int timelineBottom = timelineTop;
  uint8_t renderedDay = 0;

  for (size_t index = 0; index < package.agendaCount; ++index) {
    const AgendaRow& row = package.agenda[index];
    char title[MAX_AGENDA_TITLE_BYTES + 1];
    copyField(row.title, row.titleLength, title);
    char detail[MAX_AGENDA_DETAIL_BYTES + 1];
    copyField(row.detail, row.detailLength, detail);
    const bool focused = index == 0;
    const bool hasDetail = detail[0] != '\0';

    // A new calendar day gets a separator before its first appointment, so the
    // times below it cannot be read as today's.
    if (row.dayOffset != renderedDay) {
      renderedDay = row.dayOffset;
      if (y + ascenderFor(FontRole::Micro) + 10 > bottom) break;
      char dayLabel[16];
      formatDate(localGenerated, row.dayOffset, dayLabel);
      label(canvas, textBox(titleX, y, titleWidth, FontRole::Micro), dayLabel, FontRole::Micro, true);
      canvas.line(titleX, y + ascenderFor(FontRole::Micro) + 4, rect.x + rect.width - gutter,
                  y + ascenderFor(FontRole::Micro) + 4, true);
      y += ascenderFor(FontRole::Micro) + 14;
    }

    const int rowHeight = (focused ? ascenderFor(FontRole::Body) + 12 : ascenderFor(FontRole::Body) + 4) +
                          (hasDetail ? ascenderFor(FontRole::Micro) + 2 : 0);
    if (y + rowHeight > bottom) break;

    char time[8];
    formatMinute(row.minuteOfDay, time);
    // "00:00" is the widest clock at the Micro rung and measures 48 px, so the
    // 52 px box keeps a margin without eating into the title column.
    label(canvas, textBox(rect.x + gutter, y + 2, timelineX - rect.x - gutter - 4, FontRole::Micro), time,
          FontRole::Micro, focused, true, TextAlign::Right);

    const int dotY = y + 6;
    canvas.fill({timelineX - 3, dotY, 7, 7}, true);
    timelineBottom = dotY + 4;

    if (focused) {
      canvas.rect({titleX - 6, y - 4, titleWidth + 12, rowHeight}, true);
    }
    label(canvas, textBox(titleX, y, titleWidth, FontRole::Body), title, FontRole::Body, focused);
    if (hasDetail) {
      label(canvas, textBox(titleX, y + ascenderFor(FontRole::Body) + 2, titleWidth, FontRole::Micro), detail,
            FontRole::Micro);
    }
    y += rowHeight + 10;
  }

  if (timelineBottom > timelineTop) canvas.line(timelineX, timelineTop, timelineX, timelineBottom, true);
}

void renderStatusColumn(DashboardV3Canvas& canvas, const Rect rect, const DashboardV3Package& package) {
  const int left = rect.x + PAD;
  const int width = rect.width - 2 * PAD;
  const int bottom = rect.y + rect.height - 10;
  int y = rect.y + 10;

  label(canvas, textBox(left, y, width, FontRole::Body), "STATUS", FontRole::Body, true);
  y += ascenderFor(FontRole::Body) + 10;

  // Four rows on one grid. Steps used to be a taller block of its own, which
  // made the column read as two unrelated lists; it is the same kind of thing
  // as the batteries - a value with progress towards a full bar - so it gets
  // the same row.
  char batteryPercent[3][8];
  formatPercent(package.status.x3Battery, batteryPercent[0]);
  formatPercent(package.status.vehicleBattery, batteryPercent[1]);
  formatPercent(package.status.homeBattery, batteryPercent[2]);
  char steps[16];
  formatSteps(package.status.steps, steps);

  // Steps show the count, not the percentage: the goal is context, the count is
  // the thing you look for. The bar still draws progress towards the goal.
  int stepFill = -1;
  if (package.status.steps != UINT16_MAX && package.status.stepGoal != UINT16_MAX && package.status.stepGoal > 0) {
    stepFill = package.status.steps * 100 / package.status.stepGoal;
  }

  struct MeterSpec {
    const char* name;
    uint8_t iconId;
    const char* value;
    int fill;
  };
  const MeterSpec meters[] = {
      {"X3", ICON_BATTERY, batteryPercent[0], meterFillFor(package.status.x3Battery)},
      {"IONIQ 5", ICON_CAR, batteryPercent[1], meterFillFor(package.status.vehicleBattery)},
      {"THUISACCU", ICON_HOUSE, batteryPercent[2], meterFillFor(package.status.homeBattery)},
      {"STAPPEN", ICON_FOOTPRINTS, steps, stepFill},
  };
  const int meterHeight = ascenderFor(FontRole::Micro) + 6 + 8;
  for (const MeterSpec& meter : meters) {
    meterRow(canvas, {left, y, width, meterHeight}, meter.name, meter.iconId, meter.value, meter.fill);
    y += meterHeight + 10;
  }
  y += 8;

  // Markets. Row 0 gets a block of its own: its label as the caption and its
  // change at the Value rung, which is half again the height of the rows below.
  //
  // The renderer does not know what row 0 means. The phone decides which source
  // fills the first slot; this code only knows the first row is the important
  // one, so nothing here has to change when that source does.
  if (package.marketCount > 0 && y + ascenderFor(FontRole::Micro) < bottom) {
    const MarketRow& lead = package.markets[0];
    char leadLabel[MAX_MARKET_LABEL_BYTES + 1];
    copyField(lead.label, lead.labelLength, leadLabel);
    // The caption rung is ALL-CAPS everywhere else on the panel, and this label
    // arrives as a Home Assistant friendly name. Upper-casing belongs here with
    // the panel's typography, not in the phone's configuration.
    for (char* character = leadLabel; *character != '\0'; ++character) {
      if (*character >= 'a' && *character <= 'z') {
        *character = static_cast<char>(*character - 'a' + 'A');
      }
    }
    label(canvas, textBox(left, y, width, FontRole::Micro), leadLabel, FontRole::Micro, true);
    y += ascenderFor(FontRole::Micro) + 4;

    // Heading rather than Value: the status column has no slack, so every pixel
    // this block gains comes straight off the WhatsApp rows below it, which fill
    // whatever is left. Measured on the preview: Value costs two of the five chat
    // rows, Heading costs one and still reads as the largest figure in the column.
    if (y + ascenderFor(FontRole::Heading) <= bottom) {
      if (lead.changeBasisPoints == INT16_MIN) {
        label(canvas, textBox(left, y, width, FontRole::Heading), "-", FontRole::Heading, true);
      } else {
        constexpr int leadMarkSize = 11;
        const int leadMarkY = y + (ascenderFor(FontRole::Heading) - leadMarkSize) / 2;
        triangle(canvas, {left, leadMarkY, leadMarkSize, leadMarkSize}, lead.changeBasisPoints >= 0);
        char change[16];
        formatChange(lead.changeBasisPoints, change);
        label(canvas, textBox(left + leadMarkSize + 8, y, width - leadMarkSize - 8, FontRole::Heading), change,
              FontRole::Heading, true);
      }
      y += ascenderFor(FontRole::Heading) + 10;
    }

    if (package.marketCount > 1 && y + ascenderFor(FontRole::Micro) < bottom) {
      label(canvas, textBox(left, y, width, FontRole::Micro), "MARKTEN \xc2\xb7 DAG", FontRole::Micro, true);
      y += ascenderFor(FontRole::Micro) + 6;
      for (size_t index = 1; index < package.marketCount; ++index) {
        if (y + ascenderFor(FontRole::Small) > bottom) break;
        const MarketRow& row = package.markets[index];
        char market[MAX_MARKET_LABEL_BYTES + 1];
        copyField(row.label, row.labelLength, market);
        label(canvas, textBox(left, y, width - 76, FontRole::Small), market, FontRole::Small);
        if (row.changeBasisPoints == INT16_MIN) {
          label(canvas, textBox(left + width - 76, y, 76, FontRole::Small), "-", FontRole::Small, true, true,
                TextAlign::Right);
        } else {
          char change[16];
          formatChange(row.changeBasisPoints, change);
          label(canvas, textBox(left + width - 62, y, 62, FontRole::Small), change, FontRole::Small, true, true,
                TextAlign::Right);
          constexpr int markSize = 9;
          const int markY = y + (ascenderFor(FontRole::Small) - markSize) / 2;
          triangle(canvas, {left + width - 76, markY, markSize, markSize}, row.changeBasisPoints >= 0);
        }
        y += ascenderFor(FontRole::Small) + 5;
      }
    }
    y += 10;
  }

  // WhatsApp. The section is drawn only as far as the remaining height allows;
  // it never pushes an earlier section around.
  if (package.chatCount == 0 || y + ascenderFor(FontRole::Body) > bottom) return;
  // No message glyph: the heading already says WhatsApp, and the icon only
  // competed with the unread count for the same corner.
  label(canvas, textBox(left, y, width - 44, FontRole::Body), "WHATSAPP", FontRole::Body, true);
  char unread[12];
  std::snprintf(unread, sizeof(unread), "%u", static_cast<unsigned>(package.unreadTotal));
  label(canvas, textBox(left + width - 40, y, 40, FontRole::Small), unread, FontRole::Small, true, true,
        TextAlign::Right);
  y += ascenderFor(FontRole::Body) + 8;

  for (size_t index = 0; index < package.chatCount; ++index) {
    if (y + ascenderFor(FontRole::Small) > bottom) break;
    const ChatRow& row = package.chats[index];
    char name[MAX_CHAT_NAME_BYTES + 1];
    copyField(row.name, row.nameLength, name);
    char time[8];
    formatMinute(row.lastMessageMinuteOfDay, time);
    char count[8];
    std::snprintf(count, sizeof(count), "%u", static_cast<unsigned>(row.unreadCount));
    // Time first, then who, then how many: the same reading order as the phone.
    // The clock gets the same 52 px box as the agenda: at the Micro rung the
    // widest time measures 48 px, and a truncated "19:..." says nothing, where
    // a shortened name is still recognisable.
    label(canvas, textBox(left, y + 1, 52, FontRole::Micro), time, FontRole::Micro);
    label(canvas, textBox(left + 56, y, width - 56 - 36, FontRole::Small), name, FontRole::Small);
    label(canvas, textBox(left + width - 36, y, 36, FontRole::Small), count, FontRole::Small, true, true,
          TextAlign::Right);
    y += ascenderFor(FontRole::Small) + 5;
  }
}

void renderFooter(DashboardV3Canvas& canvas, const Rect rect, const DashboardV3Package& package,
                  const uint64_t localGenerated) {
  const int left = rect.x + PAD;
  const int right = rect.x + rect.width - PAD;
  const Quote* quote = quoteForId(package.quoteId);
  // The quote owns the top line outright; only the author line shares its row
  // with the refresh time, so reserving space on both lines wasted 110 px.
  const int quoteWidth = rect.width - 2 * PAD;
  if (quote != nullptr) {
    // A quote is a whole sentence, so truncating it mid-word is worse than
    // setting it a rung smaller. The renderer cannot measure text, but Lexend
    // averages about 11 px per character at the Body rung on this panel, which
    // puts the 500 px line at roughly 44 characters; past that, Micro (about
    // 9 px per character) still fits the longest entry in the table.
    // This counts UTF-8 bytes rather than characters, so an accented quote is
    // judged slightly long. That errs towards the smaller rung, which is the
    // safe direction: the failure being avoided is a sentence cut mid-word.
    // tools/dashboard-v3-preview prints the measured width of every quote.
    constexpr size_t BODY_QUOTE_BUDGET = 44;
    const FontRole quoteRung = std::strlen(quote->text) > BODY_QUOTE_BUDGET ? FontRole::Micro : FontRole::Body;
    const int quoteTop = rect.y + 8 + (ascenderFor(FontRole::Body) - ascenderFor(quoteRung));
    label(canvas, textBox(left, quoteTop, quoteWidth, quoteRung), quote->text, quoteRung, true);
    char author[64];
    std::snprintf(author, sizeof(author), "- %s", quote->author);
    label(canvas, textBox(left, rect.y + 8 + ascenderFor(FontRole::Body) + 4, quoteWidth - 140, FontRole::Micro),
          author, FontRole::Micro);
  }

  // When the package was built, so a stale dashboard is recognisable as stale.
  if (localGenerated != 0) {
    const auto minuteOfDay = static_cast<uint16_t>((localGenerated % 86400ULL) / 60ULL);
    char time[8];
    formatMinute(minuteOfDay, time);
    char refreshed[24];
    std::snprintf(refreshed, sizeof(refreshed), "ververst %s", time);
    label(canvas, textBox(right - 130, rect.y + 8 + ascenderFor(FontRole::Body) + 4, 130, FontRole::Micro), refreshed,
          FontRole::Micro, false, true, TextAlign::Right);
  }
}

// ---------------------------------------------------------------------------
// 8A: the approved visual design, for format-4 and newer packages.
//
// Everything below is reached from renderDashboardV3() when the package says
// formatVersion >= FORMAT_VERSION_8A. A format-2/3 package sitting in a device's
// cache never gets here, which is what lets the two layouts disagree about
// almost everything without either one having to compromise.
//
// The band geometry lives in DashboardV3Layout.cpp; this file only draws into
// the rects that returns, so a canvas that is not 528x792 still gets bands that
// tile exactly.
//
// The bands are deliberately out of line. Each owns a few hundred bytes of text
// scratch (a day heading, a "+N meer" line, four market blocks), and with one
// call site each the compiler inlines all of them into renderDashboardV3: that
// summed every band's scratch, taking the panel-refresh frame from 1456 to 1600
// bytes. Out of line costs 32 bytes of call overhead and leaves the caller where
// it was, with each band's own frame at most 640 bytes (the agenda, which
// carries the day plan and two summary lines). Measure with:
//   riscv32-esp-elf-objdump -d DashboardV3Renderer.cpp.o | grep 'addi.*sp,sp,-'
// ---------------------------------------------------------------------------

constexpr int PAD8A = DASHBOARD8A_PAD;
constexpr int KPI_8A_ROW_HEIGHT = 32;
// The market line packs four indices plus a mover into one row, so it runs a
// tighter margin than the other bands: at the standard 14 px pad the typical
// "AEX +0,5% S&P +0,3% NDX +0,7% BTC +1,2% | ASML +6,1% +2" measures 518 px in
// 500 and would lose its "+2" for no reason the reader can see.
constexpr int MARKETS_8A_PAD = 10;
constexpr int MARKETS_8A_GAP = 4;

// --- Width measurement -------------------------------------------------------

/// Nominal width of one character at the Micro rung, used only when the canvas
/// cannot measure. Derived from the real Lexend 8 bold face: a digit is 10 px, a
/// capital 12, a comma 4 and a percent sign 18, so "AEX" lands on 36 and
/// "+0,5%" on 51 where the face itself measures 36 and 52.
int microUnits(const char* text) {
  int width = 0;
  for (const uint8_t* cursor = reinterpret_cast<const uint8_t*>(text); *cursor != '\0'; ++cursor) {
    const uint8_t character = *cursor;
    if (character >= 0x80U) {
      // One non-ASCII glyph (a degree sign in practice): the widest thing the
      // faces carry at this rung that is not a percent sign.
      width += 13;
      while ((cursor[1] & 0xC0U) == 0x80U) ++cursor;
    } else if (character >= '0' && character <= '9') {
      width += 10;
    } else if (character >= 'A' && character <= 'Z') {
      width += 12;
    } else if (character >= 'a' && character <= 'z') {
      width += 9;
    } else if (character == '%') {
      width += 18;
    } else if (character == '&') {
      width += 13;
    } else if (character == ' ') {
      width += 4;
    } else if (character == ',' || character == '.' || character == ':' || character == ';') {
      width += 4;
    } else if (character == '-') {
      width += 6;
    } else {
      width += 9;
    }
  }
  return width;
}

/// The same figure at another rung. The ratios are the measured ones: a ten
/// digit run is 99 px at Micro, 111 at Small, 123 at Body, 149 at Heading,
/// 173 at Value and 198 at Hero.
int estimateWidth(const FontRole role, const char* text) {
  const int units = microUnits(text);
  switch (role) {
    case FontRole::Micro: return units;
    case FontRole::Small: return units * 112 / 100;
    case FontRole::Body: return units * 124 / 100;
    case FontRole::Heading: return units * 150 / 100;
    case FontRole::Value: return units * 175 / 100;
    case FontRole::Hero: return units * 200 / 100;
  }
  return units;
}

/// The width of `value` at `spec`'s rung and weight: the canvas's own
/// measurement when it has one, otherwise the estimate above plus a margin. The
/// device and the preview both measure, so the estimate is only what a canvas
/// without font metrics gets; it over-counts on purpose, because the one thing
/// it must never do is hand the renderer a box narrower than the face needs.
/// A column that comes out too wide only looks loose, where one that comes out
/// too narrow clips a number.
int measuredWidth(const DashboardV3Canvas& canvas, const TextSpec& spec, const char* value) {
  const int measured = canvas.measureText(spec, value);
  if (measured >= 0) return measured;
  const int estimate = estimateWidth(spec.font, value);
  return estimate + estimate / 8 + 4;
}

// --- 8A text formatters ------------------------------------------------------

/// A change as a signed one-decimal percentage, rounded to the tenth the panel
/// has room for: 68 bp -> "+0,7%", 610 bp -> "+6,1%" and 96 bp -> "+1,0%".
void formatSignedPercent(const int16_t basisPoints, char (&out)[16]) {
  const int magnitude = std::abs(static_cast<int>(basisPoints));
  const int tenths = (magnitude + 5) / 10;
  std::snprintf(out, sizeof(out), "%c%d,%d%%", basisPoints < 0 ? '-' : '+', tenths / 10, tenths % 10);
}

/// A duration as the shortest honest form: "45m", "1u", "1u30". UINT16_MAX is
/// the wire's "the phone did not say" and returns false so the caller draws
/// nothing rather than guessing an interval.
bool formatDuration(const uint16_t minutes, char (&out)[12]) {
  if (minutes == UINT16_MAX) return false;
  if (minutes < 60) {
    std::snprintf(out, sizeof(out), "%um", static_cast<unsigned>(minutes));
    return true;
  }
  const unsigned hours = minutes / 60;
  const unsigned rest = minutes % 60;
  if (rest == 0) {
    std::snprintf(out, sizeof(out), "%uu", hours);
  } else {
    std::snprintf(out, sizeof(out), "%uu%02u", hours, rest);
  }
  return true;
}

/// How far the first event is from the package's own reference minute. The
/// reference is the local minute of generatedAt, never the panel's clock: the
/// panel can be woken hours after the package was composed, and a countdown
/// that keeps shrinking would end up saying "over -1u" on a stale package.
/// A negative distance (the event has started, or the package is old) is
/// clamped to "nu" instead of counting backwards.
void formatCountdown(const int minutesAway, char (&out)[24]) {
  if (minutesAway <= 0) {
    std::snprintf(out, sizeof(out), "nu");
    return;
  }
  if (minutesAway < 60) {
    std::snprintf(out, sizeof(out), "over %um", minutesAway);
    return;
  }
  if (minutesAway < 24 * 60) {
    const int hours = minutesAway / 60;
    const int minutes = minutesAway % 60;
    if (minutes == 0) {
      std::snprintf(out, sizeof(out), "over %uu", hours);
    } else {
      std::snprintf(out, sizeof(out), "over %uu%02u", hours, minutes);
    }
    return;
  }
  const int days = minutesAway / (24 * 60);
  const int hours = (minutesAway % (24 * 60)) / 60;
  const int minutes = minutesAway % 60;
  // A multi-day countdown still carries its smaller unit: "over 1d 4u45" is a
  // time someone can plan around, "over 1d 4u" is a rounded-off guess.
  if (hours == 0) {
    if (minutes == 0) {
      std::snprintf(out, sizeof(out), "over %ud", days);
    } else {
      std::snprintf(out, sizeof(out), "over %ud %um", days, minutes);
    }
  } else if (minutes == 0) {
    std::snprintf(out, sizeof(out), "over %ud %uu", days, hours);
  } else {
    std::snprintf(out, sizeof(out), "over %ud %uu%02u", days, hours, minutes);
  }
}

/// A short day reference for the focus row's caption: "morgen" for tomorrow and
/// "di 15" beyond that, both derived from the package's own timestamp. Callers
/// only reach this for a future day, so it never has to describe today.
void formatDayContext(const uint64_t localGenerated, const int dayOffset, char (&out)[16]) {
  if (dayOffset <= 0) {
    std::snprintf(out, sizeof(out), "vandaag");
    return;
  }
  if (dayOffset == 1) {
    std::snprintf(out, sizeof(out), "morgen");
    return;
  }
  const int64_t days = static_cast<int64_t>(localGenerated / 86400ULL) + dayOffset;
  const CivilDate date = civilFromDays(days);
  const auto weekday = static_cast<dashboard::Weekday>(((days % 7) + 7 + 3) % 7);
  std::snprintf(out, sizeof(out), "%s %d", dashboard::weekdayAbbreviation(weekday), date.day);
}

/// The overflow line only states how many appointments did not fit. A time or
/// title would put one hidden appointment beside the count and make it look
/// like a visible row.
void formatMoreEvents8A(const int omitted, char (&out)[32]) {
  std::snprintf(out, sizeof(out), "+%d meer", omitted);
}

/// The day heading: date, the day's exact event total, and how many are all-day.
/// Facts from the package only — no "geen werkdag" and no
/// "volle ochtend", because the wire carries no work-calendar and no free time.
void formatDayHeading8A(const uint64_t localGenerated, const int dayOffset, const int eventCount,
                        const int allDayCount, char (&out)[64]) {
  char dateLabel[16];
  formatDate(localGenerated, dayOffset, dateLabel);
  const char* const eventWord = eventCount == 1 ? "AFSPRAAK" : "AFSPRAKEN";
  if (allDayCount > 0) {
    std::snprintf(out, sizeof(out), "%s \xc2\xb7 %d %s \xc2\xb7 %d HELE DAG%s", dateLabel, eventCount, eventWord,
                  allDayCount, allDayCount == 1 ? "" : "EN");
  } else {
    std::snprintf(out, sizeof(out), "%s \xc2\xb7 %d %s", dateLabel, eventCount, eventWord);
  }
}

// --- 8A bands ----------------------------------------------------------------

/// The header: date and refresh time on the left, weather, wind and the next
/// sun event on the right, no divider columns. The date and "ververst" come from
/// the package's own timestamp shifted into local time so they agree with each
/// other; the sun event is picked against the device clock, which is the only
/// clock that knows whether sunrise has passed.
[[gnu::noinline]]
void renderHeader8A(DashboardV3Canvas& canvas, const Rect rect, const DashboardV3Package& package,
                    const uint16_t minuteOfDay, const uint64_t localGenerated) {
  canvas.fill(rect, true);
  const int left = rect.x + PAD8A;
  const int right = rect.x + rect.width - PAD8A;
  const int valueY = rect.y + 10;
  const int captionY = rect.y + 36;
  constexpr int ICON_8A_HEADER = 22;
  constexpr int ICON_VALUE_GAP = 5;
  constexpr int ITEM_GAP = 14;

  char temperature[8] = "-";
  if (package.weather.currentCelsius != INT8_MIN) {
    std::snprintf(temperature, sizeof(temperature), "%d\xc2\xb0", package.weather.currentCelsius);
  }
  char wind[8] = "-";
  if (package.weather.windKilometersPerHour != UINT8_MAX) {
    std::snprintf(wind, sizeof(wind), "%u", static_cast<unsigned>(package.weather.windKilometersPerHour));
  }

  uint16_t sunMinute = UINT16_MAX;
  uint8_t sunIconId = ICON_SUN;
  if (package.weather.sunriseTodayMinute != UINT16_MAX && minuteOfDay < package.weather.sunriseTodayMinute) {
    sunMinute = package.weather.sunriseTodayMinute;
    sunIconId = ICON_SUNRISE;
  } else if (package.weather.sunsetTodayMinute != UINT16_MAX && minuteOfDay < package.weather.sunsetTodayMinute) {
    sunMinute = package.weather.sunsetTodayMinute;
    sunIconId = ICON_SUNSET;
  } else if (package.weather.sunriseTomorrowMinute != UINT16_MAX) {
    sunMinute = package.weather.sunriseTomorrowMinute;
    sunIconId = ICON_SUNRISE;
  }
  char sunTime[8] = "-";
  if (sunMinute != UINT16_MAX) formatMinute(sunMinute, sunTime);

  // Right group, placed from the right edge inwards: the sun clock sits
  // furthest right and the weather reading is placed last, so when the row runs
  // out of width the reading that matters least is the one that gives way.
  struct HeaderItem8A {
    uint8_t iconId;
    const char* value;
  };
  const HeaderItem8A items[] = {
      {package.weather.conditionIconId, temperature},
      {ICON_WIND, wind},
      {sunIconId, sunTime},
  };
  int groupLeft = right;
  int x = right;
  for (int index = static_cast<int>(std::size(items)) - 1; index >= 0; --index) {
    const TextSpec spec = measureSpec(valueY, FontRole::Heading, true);
    const int valueWidth = std::max(8, measuredWidth(canvas, spec, items[index].value));
    const int itemWidth = (items[index].iconId != 0 ? ICON_8A_HEADER + ICON_VALUE_GAP : 0) + valueWidth;
    if (x - itemWidth < left + 96) break;  // keep the date column readable
    x -= itemWidth;
    groupLeft = x;
    int textX = x;
    if (items[index].iconId != 0) {
      canvas.icon(items[index].iconId, {x, rect.y + 12, ICON_8A_HEADER, ICON_8A_HEADER}, false);
      textX += ICON_8A_HEADER + ICON_VALUE_GAP;
    }
    label(canvas, textBox(textX, valueY, valueWidth, FontRole::Heading), items[index].value, FontRole::Heading, true,
          false);
    x -= ITEM_GAP;
  }

  const int leftWidth = groupLeft - 12 - left;
  if (leftWidth <= 0) return;
  char dateLabel[16];
  formatDate(localGenerated, 0, dateLabel);
  label(canvas, textBox(left, valueY, leftWidth, FontRole::Heading), dateLabel, FontRole::Heading, true, false);

  char range[20] = "-";
  const bool hasRange =
      package.weather.minimumCelsius != INT8_MIN && package.weather.maximumCelsius != INT8_MIN;
  if (hasRange) {
    std::snprintf(range, sizeof(range), "%d\xc2\xb0 / %d\xc2\xb0", package.weather.minimumCelsius,
                  package.weather.maximumCelsius);
  }
  char refreshed[20] = "-";
  if (localGenerated != 0) {
    char time[8];
    formatMinute(static_cast<uint16_t>((localGenerated % 86400ULL) / 60ULL), time);
    std::snprintf(refreshed, sizeof(refreshed), "ververst %s", time);
  }
  char caption[48];
  if (hasRange && localGenerated != 0) {
    std::snprintf(caption, sizeof(caption), "%s \xc2\xb7 %s", range, refreshed);
  } else if (hasRange) {
    std::snprintf(caption, sizeof(caption), "%s", range);
  } else {
    std::snprintf(caption, sizeof(caption), "%s", refreshed);
  }
  label(canvas, textBox(left, captionY, leftWidth, FontRole::Micro), caption, FontRole::Micro, false, false);
}

/// A strong diagonal through `bounds`: the "not this" mark the heating verdict
/// wears when the stove may not go on. Drawn as a filled band a few pixels thick
/// rather than a hairline, because a one-pixel line disappears into a 24 px
/// glyph's own strokes on a one-bit panel. Every run stays inside `bounds`.
void strikeDiagonal(DashboardV3Canvas& canvas, const Rect bounds) {
  if (bounds.width <= 0 || bounds.height <= 0) return;
  const int thickness = std::min(3, bounds.width);
  const int span = std::max(1, bounds.height - 1);
  for (int row = 0; row < bounds.height; ++row) {
    const int center = row * (bounds.width - 1) / span;
    const int left = std::clamp(center - thickness / 2, 0, bounds.width - thickness);
    canvas.fill({bounds.x + left, bounds.y + row, thickness, 1}, true);
  }
}

/// The rain band: one line, now drawn directly under the agenda. The rain icon,
/// the compact outlook, the outlined intensity strip and the window's two
/// clocks all share the 40 px band's middle, so the band reads as a sentence
/// about the next two hours with its own labelled axis instead of a caption with
/// a chart under it. Every clock here is absolute (rainStartMinute, or
/// rainStartMinute plus the two-hour window), so the band says the same thing a
/// quarter of an hour after the package was composed.
///
/// The far right carries the stove verdict as exactly one icon and no text: a
/// flame when heating is allowed, the same flame struck through when it is not,
/// and nothing at all when the package does not say. A word beside it would be a
/// second, slower way to read the same fact, and the icon is the one the band
/// already had.
///
/// The widths are measured, not assumed: the right-hand group is placed from the
/// right edge inwards and the strip takes what is left between the outlook's
/// start clock and the end clock. When that is not enough for the full
/// "HH:MM lichte regen" the leading clock is dropped first — the window's own
/// start clock sits right beside the strip, so the severity word is the part
/// worth keeping in the sentence. Nothing is ever clipped.
[[gnu::noinline]]
void renderRain8A(DashboardV3Canvas& canvas, const Rect rect, const DashboardV3Package& package) {
  const int left = rect.x + PAD8A;
  const int right = rect.x + rect.width - PAD8A;
  const bool hasRainData = package.rainKnown && package.rainStartMinute != UINT16_MAX;

  char message[40];
  formatRainLine(package, message);

  // The canvas centres a natural 32px bitmap in this box. Reserve enough
  // horizontal space for its overhang before starting either text label.
  constexpr int ICON_8A_RAIN = 24;
  constexpr int ICON_GAP = 6;
  constexpr int ITEM_GAP = 8;
  constexpr int STRIP_HEIGHT = 10;
  // The outlook text can give up its clock but the strip keeps a readable
  // minimum, so the two never fight over the same pixels. The window's start
  // clock takes its own measured slot between them.
  constexpr int STRIP_MIN_WIDTH = 96;
  const int lineY = rect.y + (rect.height - ascenderFor(FontRole::Micro)) / 2;
  const TextSpec spec{{0, lineY, 0, ascenderFor(FontRole::Micro)}, FontRole::Micro, TextAlign::Left, true, true};

  // Right-hand group, placed from the right edge inwards. The heating badge's
  // 24 px slot is reserved whether or not the package carries a verdict, so the
  // strip and the clocks keep the same place from package to package.
  const int heatingX = right - ICON_8A_RAIN;
  // A short rule in the gap before that slot marks where the rain sentence ends
  // and the verdict begins, so the last clock and the flame cannot run together
  // into one word. Like the slot itself it is drawn whether or not the package
  // carries a verdict, so the band keeps its shape package to package.
  constexpr int STOVE_DIVIDER_INSET = 8;
  if (rect.height > 2 * STOVE_DIVIDER_INSET) {
    const int dividerX = heatingX - ITEM_GAP / 2;
    canvas.line(dividerX, rect.y + STOVE_DIVIDER_INSET, dividerX, rect.y + rect.height - STOVE_DIVIDER_INSET, true);
  }
  char endLabel[8] = "";
  char startLabel[8] = "";
  int endLabelWidth = 0;
  int startLabelWidth = 0;
  if (hasRainData) {
    formatMinute(package.rainStartMinute, startLabel);
    formatMinute(static_cast<uint16_t>((package.rainStartMinute + RAIN_BUCKET_COUNT * RAIN_MINUTES_PER_BUCKET) % 1440),
                 endLabel);
    endLabelWidth = std::max(8, measuredWidth(canvas, spec, endLabel));
    startLabelWidth = std::max(8, measuredWidth(canvas, spec, startLabel));
  }
  // The end clock's right edge, then the strip's right edge one gap to its left.
  const int endTimeRight = heatingX - ITEM_GAP;
  const int endTimeX = endTimeRight - endLabelWidth;
  const int stripRight = endTimeX - ITEM_GAP;

  // The outlook: the full form, or the same sentence without its leading clock
  // when the row cannot hold the start clock, the strip and the end clock at once.
  const int iconX = left;
  const int messageX = iconX + ICON_8A_RAIN + ICON_GAP;
  const int clockSlot = hasRainData ? startLabelWidth + ITEM_GAP : 0;
  const int messageBudget = std::max(0, stripRight - STRIP_MIN_WIDTH - ITEM_GAP - clockSlot - messageX);
  char terse[40] = "";
  if (std::strlen(message) > 6 && message[2] == ':' && message[5] == ' ') {
    std::snprintf(terse, sizeof(terse), "%s", message + 6);
  }
  const int messageWidth = std::max(8, measuredWidth(canvas, spec, message));
  const int terseWidth = terse[0] != '\0' ? std::max(8, measuredWidth(canvas, spec, terse)) : messageWidth;
  const bool useTerse = messageWidth > messageBudget && terse[0] != '\0';
  const char* drawnMessage = useTerse ? terse : message;
  const int drawnWidth = useTerse ? terseWidth : messageWidth;

  // The icon answers "is this about rain or about dry weather?", which is what
  // the message says; it is not a second copy of the intensity strip.
  const bool dryOutlook = std::strncmp(message, "droog", 5) == 0;
  canvas.icon(dryOutlook ? ICON_SUN : ICON_CLOUD_RAIN, {iconX, lineY, ICON_8A_RAIN, ICON_8A_RAIN}, true);
  // The box never comes out narrower than the string it holds; a string wider
  // than the budget pushes the strip to the right instead of being clipped.
  label(canvas, textBox(messageX, lineY, std::max(8, std::max(messageBudget, drawnWidth)), FontRole::Micro),
        drawnMessage, FontRole::Micro, true);

  // The stove verdict: one icon, no words. The strike is the same flame plus a
  // strong diagonal, which is what "not allowed" looks like without a second
  // symbol or a label; an unknown verdict draws nothing at all.
  if (package.heatingKnown) {
    const Rect badge{heatingX, lineY, ICON_8A_RAIN, ICON_8A_RAIN};
    canvas.icon(ICON_FLAME, badge, true);
    if (!package.heatingAllowed) strikeDiagonal(canvas, badge);
  }

  if (!hasRainData) return;  // the outlook already said there is nothing to draw

  // The window's own start clock, then the strip, then its end clock: the three
  // read as one labelled axis whatever the outlook says.
  int cursor = messageX + drawnWidth + ITEM_GAP;
  label(canvas, textBox(cursor, lineY, startLabelWidth, FontRole::Micro), startLabel, FontRole::Micro, true, true);
  cursor += startLabelWidth + ITEM_GAP;
  label(canvas, textBox(endTimeX, lineY, endLabelWidth, FontRole::Micro), endLabel, FontRole::Micro, true, true);

  // 10 px strip, one pixel of outline and one pixel of white between segments so
  // two neighbouring intensities cannot read as one block.
  const int ribbonX = cursor;
  const Rect ribbon{ribbonX, lineY + (ascenderFor(FontRole::Micro) - STRIP_HEIGHT) / 2, stripRight - ribbonX,
                    STRIP_HEIGHT};
  if (ribbon.width <= 2) return;
  canvas.rect(ribbon, true);
  const Rect inner{ribbon.x + 1, ribbon.y + 1, ribbon.width - 2, ribbon.height - 2};
  for (int segment = 0; segment < RAIN_SEGMENTS; ++segment) {
    uint8_t peak = 0;
    for (int bucket = 0; bucket < RAIN_BUCKETS_PER_SEGMENT; ++bucket) {
      peak = std::max(peak, package.rain[segment * RAIN_BUCKETS_PER_SEGMENT + bucket]);
    }
    const Shade level = peak == 0  ? Shade::None
                        : peak < 2 ? Shade::Quarter
                        : peak < 3 ? Shade::Half
                                   : Shade::Solid;
    if (level == Shade::None) continue;
    const int x1 = inner.x + segment * inner.width / RAIN_SEGMENTS;
    const int x2 = inner.x + (segment + 1) * inner.width / RAIN_SEGMENTS;
    canvas.shade({x1, inner.y, std::max(1, x2 - x1 - 1), inner.height}, level);
  }
}

/// The focus row: the first encoded event, its time large on the left and its
/// title and second line to the right of a hairline. The countdown is measured
/// from the package's reference minute, so it is a snapshot of "how long after
/// the phone composed this" rather than a live clock the panel cannot keep.
///
/// It is not a band of its own any more. renderAgenda8A draws it as the first
/// entry of the agenda's first day group, after that day's ribbon: it keeps the
/// 58 px and the Hero time / Body title the standalone hero band had, but it
/// draws on the same white as the rows below it instead of inverting. Only its
/// second line stays secondary, so the countdown and the location beside it
/// never compete with the appointment they belong to. The caller only invokes
/// it for a day that exists, so there is no empty state here.
[[gnu::noinline]]
void renderFocusRow8A(DashboardV3Canvas& canvas, const Rect rect, const DashboardV3Package& package,
                  const uint64_t localGenerated) {
  const int left = rect.x + PAD8A;
  const int right = rect.x + rect.width - PAD8A;

  const AgendaRow& row = package.agenda[0];
  char big[16];
  if (row.isAllDay) {
    std::snprintf(big, sizeof(big), "hele dag");
  } else {
    char time[8];
    formatMinute(row.minuteOfDay, time);
    std::snprintf(big, sizeof(big), "%s", time);
  }
  const TextSpec bigSpec = measureSpec(rect.y + 12, FontRole::Hero, true);
  const int bigWidth = std::min(std::max(8, measuredWidth(canvas, bigSpec, big)), (right - left) / 2);
  label(canvas, textBox(left, rect.y + 12, bigWidth, FontRole::Hero), big, FontRole::Hero, true);

  const int ruleX = left + bigWidth + 14;
  if (ruleX >= right - 60) return;  // no room for a title beside the time
  canvas.line(ruleX, rect.y + 12, ruleX, rect.y + rect.height - 12, true);

  char title[MAX_AGENDA_TITLE_BYTES + 1];
  copyField(row.title, row.titleLength, title);
  const int textX = ruleX + 14;
  const int textWidth = right - textX;
  // Regular weight: the Hero clock is the row's loudest mark, so the title names
  // the appointment without competing with it. The box is bounded by the band's
  // own right pad, so a full-width title is truncated by the canvas rather than
  // spilling past the panel.
  label(canvas, textBox(textX, rect.y + 8, textWidth, FontRole::Body), title, FontRole::Body, false);

  // "Teams · over 7u45" when the wire carried a location, else "hierna · over
  // 7u45" on the day itself. A later day's day reference takes the place of
  // "hierna" so the caption stays one line. Everything goes into fixed buffers:
  // no std::string, no heap, on a path the C3 draws on every wake.
  char detail[MAX_AGENDA_DETAIL_BYTES + 1];
  copyField(row.detail, row.detailLength, detail);
  char fallback[16] = "hierna";
  if (localGenerated != 0 && row.dayOffset > 0) formatDayContext(localGenerated, row.dayOffset, fallback);
  const char* const lead = hasVisibleText(detail) ? detail : fallback;

  char caption[48];
  if (localGenerated == 0) {
    // Without a reference there is no honest countdown to print.
    std::snprintf(caption, sizeof(caption), "%s", lead);
  } else {
    char window[24];
    if (row.isAllDay) {
      std::snprintf(window, sizeof(window), "hele dag");
    } else {
      const int referenceMinute = static_cast<int>((localGenerated % 86400ULL) / 60ULL);
      const int minutesAway = row.dayOffset * 1440 + row.minuteOfDay - referenceMinute;
      formatCountdown(minutesAway, window);
    }
    std::snprintf(caption, sizeof(caption), "%s \xc2\xb7 %s", lead, window);
  }
  label(canvas, textBox(textX, rect.y + 32, textWidth, FontRole::Micro), caption, FontRole::Micro, false, true,
        TextAlign::Left, /*dithered=*/true);
}

/// One calendar day's rows in the package: where they start and how many.
struct AgendaDay8A {
  uint8_t first = 0;
  uint8_t count = 0;
};

/// Draws the 07:00..23:00 ribbon for one day. A timed event becomes an interval
/// whose width is its real duration, clipped to the window; an unknown duration
/// becomes a two-pixel mark instead of an invented interval; a soft block is
/// dithered so it reads as provisional next to the solid timed events.
void drawAgenda8ARibbon(DashboardV3Canvas& canvas, const Agenda8ARects& agenda, const DashboardV3Package& package,
                        const Rect ribbon, const size_t first, const size_t end) {
  canvas.rect(ribbon, true);
  const Rect inner{ribbon.x + 1, ribbon.y + 1, ribbon.width - 2, ribbon.height - 2};
  if (inner.width <= 0 || inner.height <= 0) return;
  const int innerRight = inner.x + inner.width;
  for (size_t index = first; index < end; ++index) {
    const AgendaRow& row = package.agenda[index];
    if (row.isAllDay) continue;  // an all-day event has no place on a clock axis
    int x = agendaRibbonX(inner, row.minuteOfDay, agenda.ribbonStartMinute, agenda.ribbonEndMinute);
    const bool hasInterval = row.durationMinutes != UINT16_MAX && row.durationMinutes > 0;
    if (!hasInterval) {
      if (x >= innerRight) x = innerRight - 2;
      canvas.fill({x, inner.y, 2, inner.height}, true);
      continue;
    }
    int x2 = agendaRibbonX(inner, row.minuteOfDay + row.durationMinutes, agenda.ribbonStartMinute,
                           agenda.ribbonEndMinute);
    int width = std::max(2, x2 - x);
    if (x >= innerRight) {
      x = innerRight - width;
    } else if (x + width > innerRight) {
      width = innerRight - x;
    }
    if (width <= 0) continue;
    if (row.isSoftBlock) {
      canvas.shade({x, inner.y, width, inner.height}, Shade::Half);
    } else {
      canvas.fill({x, inner.y, width, inner.height}, true);
    }
  }

  // Three ticks under the strip, so "half past ten" has something to line up
  // with. Only 08:00, 12:00 and 18:00: more ticks than that turn the 10 px strip
  // into a comb at this size. Each tick carries the hour it stands for, because
  // a bare tick tells a reader the day's shape but not where on the clock an
  // event sits. The label is centred on the tick and sits in the ribbon gap the
  // layout reserves for it.
  static constexpr int kTickHours[] = {8, 12, 18};
  constexpr int kHourLabelWidth = 24;
  const int tickLabelY = ribbon.y + ribbon.height + 4;
  for (const int hour : kTickHours) {
    const int x = agendaRibbonX(inner, hour * 60, agenda.ribbonStartMinute, agenda.ribbonEndMinute);
    canvas.line(x, ribbon.y + ribbon.height, x, ribbon.y + ribbon.height + 3, true);
    char hourLabel[4];
    std::snprintf(hourLabel, sizeof(hourLabel), "%d", hour);
    label(canvas, textBox(x - kHourLabelWidth / 2, tickLabelY, kHourLabelWidth, FontRole::Micro), hourLabel,
          FontRole::Micro, true, true, TextAlign::Center);
  }
}

/// One agenda row: right-aligned clock, the title, the encoded duration on the
/// right. There is no vertical ruler any more: the clock column and the fixed
/// title left edge already separate the two, and a full-width zebra stripe
/// (drawn by renderAgenda8A under the row) carries the eye across it. The clock
/// stays bold and the title regular, so the time reads first. The legacy
/// free-text `detail` is deliberately absent — on the phone it often holds a
/// room or a platform, and beside a duration column it reads like one.
void drawAgenda8ARow(DashboardV3Canvas& canvas, const Agenda8ARects& agenda, const AgendaRow& row, const int y) {
  const int right = agenda.content.x + agenda.content.width;
  const int durationX = right - agenda.durationWidth;
  char title[MAX_AGENDA_TITLE_BYTES + 16];
  char rawTitle[MAX_AGENDA_TITLE_BYTES + 1];
  copyField(row.title, row.titleLength, rawTitle);
  if (row.isAllDay) {
    // No clock cell: an all-day row has no minute, and "00:00" would be a claim
    // the wire never made. The label rides with the title instead.
    std::snprintf(title, sizeof(title), "hele dag \xc2\xb7 %s", rawTitle);
  } else {
    std::snprintf(title, sizeof(title), "%s", rawTitle);
    char time[8];
    formatMinute(row.minuteOfDay, time);
    label(canvas, textBox(agenda.content.x, y, agenda.timeColumnWidth, FontRole::Micro), time, FontRole::Micro, true,
          true, TextAlign::Right);
  }
  // The title box stops a gap short of the duration column, so a long title is
  // bounded (and truncated by the canvas) before it can run under the duration.
  label(canvas, textBox(agenda.titleX, y, durationX - agenda.titleX - 6, FontRole::Body), title, FontRole::Body, false);
  if (!row.isAllDay) {
    char duration[12];
    if (formatDuration(row.durationMinutes, duration)) {
      label(canvas, textBox(durationX, y + 2, agenda.durationWidth, FontRole::Micro), duration, FontRole::Micro, true,
            true, TextAlign::Right);
    }
  }
}

/// The agenda band: one group per calendar day, each with its date and encoded
/// counts, the day ribbon and as many rows as the band still has room for.
///
/// The first group carries the focus row too. Every group heads the day on
/// white and then draws its 07:00..23:00 ribbon; after that the first group
/// draws the 58 px focus row for row 0 (time, title and countdown) in place of a
/// list row, and then the day's remaining rows. Later groups are heading,
/// ribbon and rows with no focus row, nothing draws a black band, and row 0 is
/// never drawn twice.
///
/// The band fills in wire order rather than to a fixed number of rows per day.
/// Every day group that fits is drawn whole; when the next one does not, the
/// rows share what is left beside the single "+N meer" line that has to name
/// everything after them, and the days beyond that line are named by it alone.
/// Nothing is dropped silently, and a day with six events keeps all six when
/// the band has the room, exactly like a day with two.
[[gnu::noinline]]
void renderAgenda8A(DashboardV3Canvas& canvas, const DashboardV3Package& package, const uint64_t localGenerated) {
  // The agenda band's own geometry, from the same canvas all the other bands
  // were laid out against.
  const Agenda8ARects agenda = computeAgenda8ALayout(canvas.width(), canvas.height(), {});
  // Without a first event there is no focus row and no day to head, so an empty
  // agenda draws nothing at all.
  if (agenda.content.width <= 0 || agenda.content.height <= 0 || package.agendaCount == 0) return;

  // Group the encoded rows by day, starting at row 0: the first group's day is
  // the focus row's, and that row stands in for its first list entry.
  AgendaDay8A days[MAX_AGENDA_ROWS];
  uint8_t dayCount = 0;
  for (size_t index = 0; index < package.agendaCount && dayCount < MAX_AGENDA_ROWS; ++index) {
    const bool startsDay = dayCount == 0 || package.agenda[index].dayOffset != package.agenda[index - 1].dayOffset;
    if (startsDay) {
      days[dayCount].first = static_cast<uint8_t>(index);
      days[dayCount].count = 0;
      ++dayCount;
    }
    ++days[dayCount - 1].count;
  }

  // Row 0 of the first group is the focus row, so that group lists one row fewer
  // and carries the focus row's own height. Later groups list every row they have.
  const auto listRows = [&](const uint8_t day) {
    return static_cast<int>(days[day].count) - (day == 0 ? 1 : 0);
  };
  const auto groupHeight = [&](const uint8_t day, const int rows) {
    return agenda8ADayGroupHeight(agenda, rows) + (day == 0 ? agenda.heroHeight : 0);
  };
  // The day's exact total: the phone's count when it sent one, otherwise the
  // rows on the wire. Includes the focus row's own row either way.
  const auto exactTotal = [&](const uint8_t day) {
    const AgendaRow& first = package.agenda[days[day].first];
    return first.dayTotalCount > 0 ? static_cast<int>(first.dayTotalCount) : static_cast<int>(days[day].count);
  };

  // Plan the fill before drawing anything. Exact totals can exceed the encoded
  // rows when the phone's wire ceiling cut a day short, so fitting the encoded
  // prefix is not by itself proof that the whole calendar list fits.
  int listHeight = 0;
  bool hasRowsBeyondWire = false;
  for (uint8_t day = 0; day < dayCount; ++day) {
    listHeight += groupHeight(day, listRows(day));
    if (day + 1 < dayCount) listHeight += agenda.dayGap;
    if (exactTotal(day) > days[day].count) hasRowsBeyondWire = true;
  }
  const bool everythingFits = listHeight <= agenda.content.height && !hasRowsBeyondWire;
  const int sharedCeiling = agenda.content.height - agenda.summaryHeight;

  uint8_t shownRows[MAX_AGENDA_ROWS] = {};
  uint8_t drawnDays = 0;
  int used = 0;
  int summaryOmitted = 0;
  for (uint8_t day = 0; day < dayCount; ++day) {
    const int listCount = listRows(day);
    const int gap = drawnDays > 0 ? agenda.dayGap : 0;
    const int heroCredit = day == 0 ? 1 : 0;
    const int exactListCount = std::max(listCount, exactTotal(day) - heroCredit);
    const bool firstDayFitsAlone = groupHeight(day, exactListCount) <= agenda.content.height;
    const int ceiling = everythingFits || (day == 0 && firstDayFitsAlone) ? agenda.content.height : sharedCeiling;
    const int roomForRows = ceiling - used - gap - groupHeight(day, 0);
    const int fits = std::min(listCount, std::max(0, roomForRows / agenda.rowHeight));
    // The first group is the agenda's anchor: even with no list rows it draws
    // its heading, ribbon and focus row. Later groups only appear when they can
    // hold at least one row, otherwise the summary line covers them.
    if (fits > 0 || day == 0) {
      shownRows[drawnDays] = static_cast<uint8_t>(fits);
      ++drawnDays;
      used += gap + groupHeight(day, fits);
    }
    const int exactRowsOmitted = exactTotal(day) - heroCredit - fits;
    if (fits < listCount || exactRowsOmitted > 0) {
      // Count each known day by its exact total, then subtract the list rows
      // already drawn and the focus row's own row. This includes rows never
      // carried on the wire, and it keeps the focus row counted in the totals
      // it reads.
      int knownTotal = 0;
      for (uint8_t knownDay = 0; knownDay < dayCount; ++knownDay) knownTotal += exactTotal(knownDay);
      int visibleRows = 0;
      for (uint8_t drawn = 0; drawn < drawnDays; ++drawn) visibleRows += shownRows[drawn];
      summaryOmitted = std::max(0, knownTotal - 1 - visibleRows);
      break;
    }
  }

  const int bottom = agenda.content.y + agenda.content.height;
  int y = agenda.content.y;
  for (uint8_t day = 0; day < drawnDays; ++day) {
    const int count = days[day].count;
    const int shown = shownRows[day];
    const int heroCredit = day == 0 ? 1 : 0;

    const AgendaRow& first = package.agenda[days[day].first];
    int eventCount = first.dayTotalCount;
    int allDayCount = first.dayAllDayCount;
    if (eventCount == 0) {
      eventCount = count;
      allDayCount = 0;
      for (size_t index = days[day].first;
           index < static_cast<size_t>(days[day].first) + static_cast<size_t>(count); ++index) {
        if (package.agenda[index].isAllDay) ++allDayCount;
      }
    }
    char heading[64];
    formatDayHeading8A(localGenerated, first.dayOffset, eventCount, allDayCount, heading);
    label(canvas, textBox(agenda.content.x, y, agenda.content.width, FontRole::Micro), heading, FontRole::Micro, true);
    y += agenda.headingHeight;

    // Every day's ribbon comes first, so a day's shape reads before its list:
    // the ticks and their hours sit right under the strip, and only then do the
    // appointments follow.
    drawAgenda8ARibbon(canvas, agenda, package, {agenda.content.x, y, agenda.content.width, agenda.ribbonHeight},
                       days[day].first, days[day].first + count);
    y += agenda.ribbonHeight + agenda.ribbonGap;

    // Only the first group carries the focus row, and it leads that group's
    // list: row 0 drawn at the 58 px the standalone hero band used to hold,
    // rather than as a band of its own above the ribbon.
    if (heroCredit > 0) {
      renderFocusRow8A(canvas, {agenda.band.x, y, agenda.band.width, agenda.heroHeight}, package, localGenerated);
      y += agenda.heroHeight;
    }

    // The focus row already shows row 0, so the list starts at the row after it.
    const size_t firstListRow = static_cast<size_t>(days[day].first) + static_cast<size_t>(heroCredit);
    for (int rowIndex = 0; rowIndex < shown; ++rowIndex) {
      // The zebra, reset at every day heading: the first ordinary row is paper
      // and every second one after it takes the light quarter-tone, so a reader
      // can follow one row across the fixed time/title/duration columns. The
      // focus row, the headings, the ribbon and the "+N meer" line never shade,
      // and the stripe is drawn before the row so the ink stays on top of it.
      if (rowIndex % 2 == 1) {
        canvas.shade({agenda.content.x, y, agenda.content.width, agenda.rowHeight}, Shade::Quarter);
      }
      drawAgenda8ARow(canvas, agenda, package.agenda[firstListRow + static_cast<size_t>(rowIndex)], y);
      y += agenda.rowHeight;
    }
    if (day + 1 < drawnDays) y += agenda.dayGap;
  }

  if (summaryOmitted > 0 && y + agenda.summaryHeight <= bottom) {
    char more[32];
    formatMoreEvents8A(summaryOmitted, more);
    label(canvas, textBox(agenda.content.x, y, agenda.content.width, FontRole::Micro), more, FontRole::Micro, true);
  }
}

/// A battery-style progress bar: outlined track, filled from the left. The fill
/// is the percentage the row prints, so an absent reading (-1) leaves the track
/// empty instead of drawing a confident-looking half bar.
void drawMeterBar8A(DashboardV3Canvas& canvas, const Rect bar, const int fillPercentage) {
  canvas.rect(bar, true);
  if (fillPercentage <= 0) return;
  const int filled = (bar.width - 2) * std::min(fillPercentage, 100) / 100;
  if (filled > 0) canvas.fill({bar.x + 1, bar.y + 1, filled, bar.height - 2}, true);
}

/// The portfolio bar is the one signed meter: zero sits in the middle, the fill
/// grows in the direction of the change and stops at plus or minus eight percent,
/// which is the range a daily portfolio move is worth a bar for. An unknown
/// change leaves the track empty; nothing is drawn that suggests a direction.
void drawSignedBar8A(DashboardV3Canvas& canvas, const Rect bar, const int16_t basisPoints) {
  const int half = (bar.width - 2) / 2;
  if (half <= 0) {
    canvas.rect(bar, true);
    return;
  }
  canvas.rect(bar, true);
  const int centerX = bar.x + 1 + half;
  canvas.fill({centerX, bar.y + 1, 1, bar.height - 2}, true);  // the zero mark
  if (basisPoints == INT16_MIN) return;
  constexpr int RANGE_BASIS_POINTS = 800;  // +/- 8%
  const int magnitude = std::min(std::abs(static_cast<int>(basisPoints)), RANGE_BASIS_POINTS);
  const int filled = half * magnitude / RANGE_BASIS_POINTS;
  if (filled <= 0) return;
  const int x = basisPoints < 0 ? centerX - filled : centerX;
  canvas.fill({x, bar.y + 1, filled, bar.height - 2}, true);
}

/// The KPI band, left column: the three batteries and the portfolio. Each row is
/// an icon, its value and a ten-pixel bar on one line, so the column reads as
/// four gauges rather than as four captions with a bar floating under them. The
/// value boxes are all as wide as the widest value the column has to draw and
/// the bars all start at the same x, so the four bars are a straight column
/// whatever each value measures.
[[gnu::noinline]]
void renderKpiLeft8A(DashboardV3Canvas& canvas, const Rect column, const DashboardV3Package& package) {
  const int left = column.x + PAD8A;
  const int right = column.x + column.width - PAD8A;
  const int rowTop = column.y + (column.height - 4 * KPI_8A_ROW_HEIGHT) / 2;

  char x3[8];
  char vehicle[8];
  char home[8];
  char portfolio[16];
  formatPercent(package.status.x3Battery, x3);
  formatPercent(package.status.vehicleBattery, vehicle);
  formatPercent(package.status.homeBattery, home);
  if (package.portfolioChangeBasisPoints == INT16_MIN) {
    std::snprintf(portfolio, sizeof(portfolio), "-");
  } else {
    formatSignedPercent(package.portfolioChangeBasisPoints, portfolio);
  }

  struct KpiRow8A {
    uint8_t iconId;
    const char* value;
    int fill;
    bool signedBar;
  };
  const KpiRow8A rows[] = {
      {ICON_BATTERY, x3, meterFillFor(package.status.x3Battery), false},
      {ICON_CAR, vehicle, meterFillFor(package.status.vehicleBattery), false},
      {ICON_HOUSE, home, meterFillFor(package.status.homeBattery), false},
      {ICON_TRENDING_UP, portfolio, 0, true},
  };

  // One value column for all four rows: the widest reading reserves the space
  // and every bar starts after it.
  int reservedValueWidth = 0;
  for (size_t index = 0; index < std::size(rows); ++index) {
    const TextSpec valueSpec = measureSpec(rowTop + 1, FontRole::Body, true);
    reservedValueWidth = std::max(reservedValueWidth, measuredWidth(canvas, valueSpec, rows[index].value));
  }
  const int valueX = left + 26;
  int barX = valueX + reservedValueWidth + 8;
  int barWidth = right - barX;
  if (barWidth < 8) {
    // A canvas too narrow to hold that reserve still gets a usable bar, pinned
    // to the right edge instead of running off the panel.
    barWidth = 8;
    barX = right - barWidth;
  }
  const int barY = (KPI_8A_ROW_HEIGHT - 10) / 2;

  for (size_t index = 0; index < std::size(rows); ++index) {
    const KpiRow8A& row = rows[index];
    const int rowY = rowTop + static_cast<int>(index) * KPI_8A_ROW_HEIGHT;
    canvas.icon(row.iconId, {left, rowY + (KPI_8A_ROW_HEIGHT - 20) / 2, 20, 20}, true);
    const TextSpec valueSpec = measureSpec(rowY + 1, FontRole::Body, true);
    // Each value gets its own measured width, capped by the shared reserve, so
    // the widest reading never borrows space from the bar to its right.
    const int valueWidth = std::min(measuredWidth(canvas, valueSpec, row.value), reservedValueWidth);
    label(canvas, textBox(valueX, rowY + 1, std::max(8, valueWidth), FontRole::Body), row.value, FontRole::Body, true,
          true);
    const int rowBarY = rowY + barY;
    if (row.signedBar) {
      // Zero sits in the middle of the track whether or not the change is known,
      // so the portfolio's axis never moves between packages.
      drawSignedBar8A(canvas, {barX, rowBarY, barWidth, 10}, package.portfolioChangeBasisPoints);
    } else {
      drawMeterBar8A(canvas, {barX, rowBarY, barWidth, 10}, row.fill);
    }
  }
}

/// The KPI band, right column: the four readings that are a number plus a word
/// (travel minutes to a destination, national jam with its classification,
/// unread messages with the first chat names, steps). The value is drawn first,
/// in a box measured to fit it, and only the caption may be shortened: a
/// clipped "12.…" is a different number, a clipped destination is still a
/// recognisable word.
[[gnu::noinline]]
void renderKpiRight8A(DashboardV3Canvas& canvas, const Rect column, const DashboardV3Package& package) {
  const int left = column.x + PAD8A;
  const int right = column.x + column.width - PAD8A;
  const int rowTop = column.y + (column.height - 4 * KPI_8A_ROW_HEIGHT) / 2;

  char travel[16] = "-";
  if (package.traffic.travelMinutes != UINT16_MAX) {
    std::snprintf(travel, sizeof(travel), "%u min", static_cast<unsigned>(package.traffic.travelMinutes));
  }
  char destination[MAX_DESTINATION_BYTES + 1] = "REISTIJD";
  if (package.traffic.destinationLength > 0) {
    copyField(package.traffic.destination, package.traffic.destinationLength, destination);
    for (char* character = destination; *character != '\0'; ++character) {
      if (*character >= 'a' && *character <= 'z') *character = static_cast<char>(*character - 'a' + 'A');
    }
  }

  char congestion[16] = "-";
  if (package.traffic.nationalCongestionKilometers != UINT16_MAX) {
    std::snprintf(congestion, sizeof(congestion), "%u km",
                  static_cast<unsigned>(package.traffic.nationalCongestionKilometers));
  }
  static const char* const kClassification[3] = {"NORMAAL", "DRUK", "FILE"};
  const char* classification =
      package.traffic.classification < 3 ? kClassification[package.traffic.classification] : "FILES";

  char unread[8];
  std::snprintf(unread, sizeof(unread), "%u", static_cast<unsigned>(package.unreadTotal));
  char chats[48] = "-";
  if (package.chatCount > 0) {
    char first[MAX_CHAT_NAME_BYTES + 1];
    copyField(package.chats[0].name, package.chats[0].nameLength, first);
    if (package.chatCount > 1) {
      char second[MAX_CHAT_NAME_BYTES + 1];
      copyField(package.chats[1].name, package.chats[1].nameLength, second);
      std::snprintf(chats, sizeof(chats), "%s, %s", first, second);
    } else {
      std::snprintf(chats, sizeof(chats), "%s", first);
    }
  }

  char steps[16];
  formatSteps(package.status.steps, steps);

  struct KpiRow8A {
    const char* caption;
    uint8_t iconId;
    const char* value;
  };
  const KpiRow8A rows[] = {
      {destination, ICON_CAR, travel},
      {classification, ICON_TRAFFIC_CONE, congestion},
      {chats, ICON_MESSAGE, unread},
      {"STAPPEN", ICON_FOOTPRINTS, steps},
  };

  for (size_t index = 0; index < std::size(rows); ++index) {
    const KpiRow8A& row = rows[index];
    const int rowY = rowTop + static_cast<int>(index) * KPI_8A_ROW_HEIGHT;
    canvas.icon(row.iconId, {left, rowY + 4, 20, 20}, true);
    const TextSpec valueSpec = measureSpec(rowY + 1, FontRole::Body, true);
    const int valueX = left + 26;
    const int valueCeiling = std::max(16, right - valueX - 40);
    const int valueWidth = std::clamp(measuredWidth(canvas, valueSpec, row.value), 16, valueCeiling);
    label(canvas, textBox(valueX, rowY + 1, valueWidth, FontRole::Body), row.value, FontRole::Body, true, true);
    const int captionX = valueX + valueWidth + 8;
    if (right - captionX > 0) {
      label(canvas, textBox(captionX, rowY + 5, right - captionX, FontRole::Micro), row.caption, FontRole::Micro,
            false);
    }
  }
}

/// The market band: one line, everything the phone sent about the day's
/// indices. The four primary indices are a fixed row of gauges — AEX, S&P, NDX
/// and BTC in that order — so the band looks the same on every package: a slot
/// whose value the phone did not send shows a dash where its change would be,
/// and a package that carried no indices at all still draws all four. The
/// strongest mover, the thin divider and the "+N" count are added only while
/// they fit, in that order. A mover is drawn only above the three percent the
/// decoder's own contract uses, so a synthetic fixture cannot slip a 3,0%
/// holding in as a "strongest mover".
[[gnu::noinline]]
void renderMarkets8A(DashboardV3Canvas& canvas, const Rect rect, const DashboardV3Package& package) {
  const int left = rect.x + MARKETS_8A_PAD;
  const int right = rect.x + rect.width - MARKETS_8A_PAD;
  const int width = right - left;
  if (width <= 0) return;
  const int y = rect.y + (rect.height - ascenderFor(FontRole::Micro)) / 2;
  const TextSpec spec{{0, y, 0, ascenderFor(FontRole::Micro)}, FontRole::Micro, TextAlign::Left, true, true};

  struct MarketBlock8A {
    char label[MAX_MARKET_LABEL_BYTES + 1];
    char change[16];
    int labelWidth;
    int changeWidth;
  };
  static constexpr const char* kIndexLabels[MAX_MARKETS] = {"AEX", "S&P", "NDX", "BTC"};
  const int indexCount = static_cast<int>(MAX_MARKETS);
  MarketBlock8A indices[MAX_MARKETS];
  for (int index = 0; index < indexCount; ++index) {
    std::snprintf(indices[index].label, sizeof(indices[index].label), "%s", kIndexLabels[index]);
    // Slots past the phone's own market count are missing data, not missing
    // gauges: the label stays and only the change becomes a dash.
    const int16_t changeBasisPoints =
        index < static_cast<int>(package.marketCount) ? package.markets[index].changeBasisPoints : INT16_MIN;
    if (changeBasisPoints == INT16_MIN) {
      std::snprintf(indices[index].change, sizeof(indices[index].change), "-");
    } else {
      formatSignedPercent(changeBasisPoints, indices[index].change);
    }
    indices[index].labelWidth = std::max(0, measuredWidth(canvas, spec, indices[index].label));
    indices[index].changeWidth = std::max(0, measuredWidth(canvas, spec, indices[index].change));
  }

  // Extreme labels can still outsell the band; in that case every index gets an
  // equal block and the label yields to its own value rather than the row
  // crossing the panel edge.
  int used = 0;
  for (int index = 0; index < indexCount; ++index) {
    used += indices[index].labelWidth + 3 + indices[index].changeWidth;
    if (index > 0) used += MARKETS_8A_GAP;
  }
  if (used > width && indexCount > 0) {
    const int block = (width - (indexCount - 1) * MARKETS_8A_GAP) / indexCount;
    for (int index = 0; index < indexCount; ++index) {
      indices[index].changeWidth = std::min(indices[index].changeWidth, std::max(0, block - 3));
      indices[index].labelWidth = std::max(0, block - 3 - indices[index].changeWidth);
    }
    used = 0;
    for (int index = 0; index < indexCount; ++index) {
      used += indices[index].labelWidth + 3 + indices[index].changeWidth;
      if (index > 0) used += MARKETS_8A_GAP;
    }
  }

  const int change = package.strongestMover.changeBasisPoints;
  const bool hasMover = package.moverCount > 0 && package.strongestMover.labelLength > 0 &&
                        change != INT16_MIN && std::abs(static_cast<int>(change)) > 300;
  char moverLabel[MAX_MARKET_LABEL_BYTES + 1] = "";
  char moverChange[16] = "";
  char moverExtra[8] = "";
  int moverLabelWidth = 0;
  int moverChangeWidth = 0;
  int moverExtraWidth = 0;
  int dividerWidth = 0;
  if (hasMover) {
    copyField(package.strongestMover.label, package.strongestMover.labelLength, moverLabel);
    for (char* character = moverLabel; *character != '\0'; ++character) {
      if (*character >= 'a' && *character <= 'z') *character = static_cast<char>(*character - 'a' + 'A');
    }
    formatSignedPercent(static_cast<int16_t>(change), moverChange);
    if (package.moverCount > 1) {
      std::snprintf(moverExtra, sizeof(moverExtra), "+%u", static_cast<unsigned>(package.moverCount - 1));
    }
    moverLabelWidth = std::max(0, measuredWidth(canvas, spec, moverLabel));
    moverChangeWidth = std::max(0, measuredWidth(canvas, spec, moverChange));
    moverExtraWidth = moverExtra[0] != '\0' ? std::max(0, measuredWidth(canvas, spec, moverExtra)) : 0;
    // gap + rule + gap
    dividerWidth = 2 * MARKETS_8A_GAP + 1;
  }
  const int moverWidth = moverLabelWidth + 3 + moverChangeWidth;
  const bool withExtra = hasMover && moverExtraWidth > 0 &&
                         used + dividerWidth + moverWidth + 3 + moverExtraWidth <= width;
  const bool withMover = hasMover && (withExtra || used + dividerWidth + moverWidth <= width);
  if (withMover) used += dividerWidth + moverWidth + (withExtra ? 3 + moverExtraWidth : 0);

  int x = left + std::max(0, (width - used) / 2);
  for (int index = 0; index < indexCount; ++index) {
    const MarketBlock8A& block = indices[index];
    if (block.labelWidth > 6) {
      label(canvas, textBox(x, y, block.labelWidth, FontRole::Micro), block.label, FontRole::Micro, true, true);
    }
    if (block.changeWidth > 0) {
      label(canvas, textBox(x + block.labelWidth + 3, y, block.changeWidth, FontRole::Micro), block.change,
            FontRole::Micro, true, true, TextAlign::Right);
    }
    x += block.labelWidth + 3 + block.changeWidth;
    if (index + 1 < indexCount) x += MARKETS_8A_GAP;
  }
  if (!withMover) return;

  x += MARKETS_8A_GAP;
  canvas.line(x, rect.y + 8, x, rect.y + rect.height - 8, true);
  x += 1 + MARKETS_8A_GAP;
  if (moverLabelWidth > 6) {
    label(canvas, textBox(x, y, moverLabelWidth, FontRole::Micro), moverLabel, FontRole::Micro, true, true);
  }
  label(canvas, textBox(x + moverLabelWidth + 3, y, moverChangeWidth, FontRole::Micro), moverChange, FontRole::Micro,
        true, true, TextAlign::Right);
  x += moverLabelWidth + 3 + moverChangeWidth;
  if (withExtra) {
    // The "+N" sits on the same white band as the rest of the line, so it is
    // drawn in black like its neighbours. It is measured bold, so it is drawn
    // bold too: a regular face would be narrower than the box reserved for it.
    label(canvas, textBox(x + 3, y, moverExtraWidth, FontRole::Micro), moverExtra, FontRole::Micro, true, true);
  }
}

/// The quote band: the day's entry from the shared table and its author. The
/// package's own refresh time lives in the header now, so this band is only the
/// quote. Long entries step down a rung rather than being cut mid-sentence; the
/// preview measures every entry in the table against exactly this box.
[[gnu::noinline]]
void renderQuote8A(DashboardV3Canvas& canvas, const Rect rect, const DashboardV3Package& package) {
  const Quote* quote = quoteForId(package.quoteId);
  if (quote == nullptr) return;
  const int left = rect.x + PAD8A;
  const int width = rect.width - 2 * PAD8A;
  if (width <= 0) return;
  constexpr size_t BODY_QUOTE_BUDGET = 44;
  const FontRole rung = std::strlen(quote->text) > BODY_QUOTE_BUDGET ? FontRole::Micro : FontRole::Body;
  const int quoteTop = rect.y + 8 + (ascenderFor(FontRole::Body) - ascenderFor(rung));
  label(canvas, textBox(left, quoteTop, width, rung), quote->text, rung, true);
  char author[64];
  std::snprintf(author, sizeof(author), "- %s", quote->author);
  label(canvas, textBox(left, rect.y + 8 + ascenderFor(FontRole::Body) + 6, width, FontRole::Micro), author,
        FontRole::Micro);
}

[[gnu::noinline]]
void renderDashboard8A(DashboardV3Canvas& canvas, const DashboardV3Package& package, const uint16_t minuteOfDay,
                       const uint8_t utcOffsetQ) {
  const Dashboard8ARects bands = computeDashboard8ALayout(canvas.width(), canvas.height(), {});
  if (bands.header.width <= 0) return;
  const uint64_t localGenerated = localGeneratedAt(package.generatedAt, utcOffsetQ);
  canvas.fill({0, 0, canvas.width(), canvas.height()}, false);
  renderHeader8A(canvas, bands.header, package, minuteOfDay, localGenerated);
  // No standalone hero call: renderAgenda8A draws the focus row inside its
  // first day group, after that day's ribbon.
  renderAgenda8A(canvas, package, localGenerated);
  // The refinement draws the rain band directly under the agenda, above the KPI
  // band, so the two calls follow the band order the layout hands out.
  renderRain8A(canvas, bands.rain, package);
  renderKpiLeft8A(canvas, bands.kpiLeft, package);
  renderKpiRight8A(canvas, bands.kpiRight, package);
  // A rule on the halfway line separates the gauges from the four readings.
  canvas.line(bands.kpiRight.x, bands.kpi.y + 10, bands.kpiRight.x, bands.kpi.y + bands.kpi.height - 10, true);
  renderMarkets8A(canvas, bands.markets, package);
  renderQuote8A(canvas, bands.quote, package);
  // Hairlines between the light bands: the black header separates itself, the
  // five paper bands would otherwise read as one tall column.
  canvas.line(0, bands.rain.y, canvas.width() - 1, bands.rain.y, true);
  canvas.line(0, bands.kpi.y, canvas.width() - 1, bands.kpi.y, true);
  canvas.line(0, bands.markets.y, canvas.width() - 1, bands.markets.y, true);
  canvas.line(0, bands.quote.y, canvas.width() - 1, bands.quote.y, true);
}

}  // namespace

int DashboardV3Canvas::measureText(const TextSpec&, const char*) const { return -1; }

void formatRainLine(const DashboardV3Package& package, char (&out)[40]) {
  std::snprintf(out, sizeof(out), "regen -");
  if (package.rainKnown && package.rainStartMinute != UINT16_MAX) {
    describeRain(package.rain, package.rainStartMinute, out, true);
  }
}

void applyDashboardV3DeviceBattery(DashboardV3Package& package, const uint16_t percentage) {
  package.status.x3Battery = static_cast<uint8_t>(std::min<uint16_t>(percentage, 100));
}

void renderDashboardV3(DashboardV3Canvas& canvas, const DashboardV3Package& package, const uint16_t minuteOfDay,
                       const uint8_t utcOffsetQ) {
  // The phone switches renderers by bumping the payload format, so this is the
  // one branch that decides which visual design a package gets. Format 2 and 3
  // packages keep the legacy bands they were composed for — including ones still
  // sitting in a device's cache after an update.
  if (package.formatVersion >= FORMAT_VERSION_8A) {
    renderDashboard8A(canvas, package, minuteOfDay, utcOffsetQ);
    return;
  }
  const DashboardV3Rects layout = computeDashboardV3Layout(canvas.width(), canvas.height(), {});
  const uint64_t localGenerated = localGeneratedAt(package.generatedAt, utcOffsetQ);
  canvas.fill({0, 0, canvas.width(), canvas.height()}, false);
  renderHeader(canvas, layout.header, package, minuteOfDay, localGenerated);
  renderRain(canvas, layout.rain, package);
  renderTraffic(canvas, layout.traffic, package);
  canvas.line(0, layout.rain.y + layout.rain.height, canvas.width() - 1, layout.rain.y + layout.rain.height, true);
  canvas.line(0, layout.traffic.y + layout.traffic.height, canvas.width() - 1,
              layout.traffic.y + layout.traffic.height, true);
  canvas.line(layout.bodyRight.x, layout.body.y, layout.bodyRight.x, layout.body.y + layout.body.height - 1, true);
  renderAgenda(canvas, layout.bodyLeft, package, localGenerated);
  renderStatusColumn(canvas, layout.bodyRight, package);
  canvas.line(0, layout.body.y + layout.body.height, canvas.width() - 1, layout.body.y + layout.body.height, true);
  renderFooter(canvas, layout.footer, package, localGenerated);
}

}  // namespace dashboard::v3

#ifdef CROSSINK_BLE_HANDOFF_READER

#include <GfxRenderer.h>

#include "components/icons/dashboardIconTable.h"
#include "fontIds.h"

namespace dashboard::v3 {
namespace {

// Full Lexend faces only. The subsetted "dash" ladder covers ASCII plus the
// degree sign, so the U+2026 that truncatedText() appends would render as a
// replacement box.
int fontId(const FontRole role) {
  switch (role) {
    case FontRole::Micro: return LEXENDDECA_8_FONT_ID;
    case FontRole::Small: return LEXENDDECA_9_FONT_ID;
    case FontRole::Body: return LEXENDDECA_10_FONT_ID;
    case FontRole::Heading: return LEXENDDECA_12_FONT_ID;
    case FontRole::Value: return LEXENDDECA_14_FONT_ID;
    case FontRole::Hero: return LEXENDDECA_16_FONT_ID;
  }
  return LEXENDDECA_10_FONT_ID;
}

void drawNaturalIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const Rect bounds, const bool black) {
  const int stride = (icon.w + 7) / 8;
  const int x = bounds.x + (bounds.width - icon.w) / 2;
  const int y = bounds.y + (bounds.height - icon.h) / 2;
  for (int row = 0; row < icon.h; ++row) {
    const uint8_t* source = icon.bits + row * stride;
    for (int column = 0; column < icon.w; ++column) {
      if ((source[column >> 3] & static_cast<uint8_t>(0x80U >> (column & 7))) == 0) {
        renderer.drawPixel(x + column, y + row, black);
      }
    }
  }
}

class GfxDashboardV3Canvas final : public DashboardV3Canvas {
 public:
  explicit GfxDashboardV3Canvas(GfxRenderer& renderer) : renderer_(renderer) {}

  int width() const override { return renderer_.getScreenWidth(); }
  int height() const override { return renderer_.getScreenHeight(); }
  void fill(const Rect rect, const bool black) override {
    renderer_.fillRect(rect.x, rect.y, rect.width, rect.height, black);
  }
  void line(const int x1, const int y1, const int x2, const int y2, const bool black) override {
    renderer_.drawLine(x1, y1, x2, y2, black);
  }
  void rect(const Rect rect, const bool black) override {
    renderer_.drawRect(rect.x, rect.y, rect.width, rect.height, black);
  }
  void text(const TextSpec& spec, const char* value) override {
    const int id = fontId(spec.font);
    const EpdFontFamily::Style style = spec.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    // The 8A path sizes its boxes from getTextWidth, so most strings already fit
    // and truncatedText() would return them unchanged — after building a
    // std::string, which is a heap allocation per label on the C3. Drawing the
    // fitting ones straight through skips that. The strings that do not fit
    // take exactly the path they always took, so no label changes appearance.
    const int measured = renderer_.getTextWidth(id, value, style);
    std::string bounded;
    const char* drawn = value;
    if (measured > spec.bounds.width) {
      bounded = renderer_.truncatedText(id, value, spec.bounds.width, style);
      drawn = bounded.c_str();
    }
    const int textWidth = drawn == value ? measured : renderer_.getTextWidth(id, drawn, style);
    int x = spec.bounds.x;
    if (spec.align == TextAlign::Center) x += (spec.bounds.width - textWidth) / 2;
    if (spec.align == TextAlign::Right) x += spec.bounds.width - textWidth;
    renderer_.drawText(id, x, spec.bounds.y, drawn, spec.black, style);
    if (spec.dithered) {
      // A one-bit panel has no grey ink, so a secondary line is drawn solid and
      // then broken up: clear the pixels the half-tone pattern does not cover.
      // Only this line's own box is touched, and erasing the paper around it is
      // a no-op, so nothing else on the panel moves.
      const int height = renderer_.getLineHeight(id);
      for (int py = spec.bounds.y; py < spec.bounds.y + height; ++py) {
        for (int px = x; px < x + textWidth; ++px) {
          if (!shadeCoversPixel(Shade::Half, px, py)) renderer_.drawPixel(px, py, false);
        }
      }
    }
  }
  /// The device measures with the same call the legacy renderer trusts, so a
  /// value box sized from this comes out exactly as wide as the face needs and
  /// the 8A path never has to guess.
  int measureText(const TextSpec& spec, const char* value) const override {
    if (value == nullptr) return 0;
    const EpdFontFamily::Style style = spec.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    return renderer_.getTextWidth(fontId(spec.font), value, style);
  }
  void shade(const Rect bounds, const Shade level) override {
    if (level == Shade::None) return;
    for (int y = bounds.y; y < bounds.y + bounds.height; ++y) {
      for (int x = bounds.x; x < bounds.x + bounds.width; ++x) {
        if (shadeCoversPixel(level, x, y)) renderer_.drawPixel(x, y, true);
      }
    }
  }
  void icon(const uint8_t iconId, const Rect bounds, const bool black) override {
    if (iconId == 0 || iconId > DASHBOARD_ICON_COUNT) return;
    const freeink::Icon* selected = bounds.width >= 40 && bounds.height >= 40 ? DASHBOARD_ICONS_48[iconId]
                                                                            : DASHBOARD_ICONS_32[iconId];
    if (selected != nullptr) drawNaturalIcon(renderer_, *selected, bounds, black);
  }

 private:
  GfxRenderer& renderer_;
};

}  // namespace

void renderDashboardV3(GfxRenderer& renderer, const DashboardV3Package& package, const uint16_t minuteOfDay,
                       const uint8_t utcOffsetQ) {
  GfxDashboardV3Canvas canvas(renderer);
  renderDashboardV3(canvas, package, minuteOfDay, utcOffsetQ);
}

}  // namespace dashboard::v3

#endif
