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
constexpr uint8_t ICON_WIND = 6;
constexpr uint8_t ICON_SUNRISE = 11;
constexpr uint8_t ICON_SUNSET = 12;
constexpr uint8_t ICON_BATTERY = 17;
constexpr uint8_t ICON_FLAME = 14;
constexpr uint8_t ICON_HOUSE = 20;
constexpr uint8_t ICON_CAR = 25;
constexpr uint8_t ICON_TRAFFIC_CONE = 27;
constexpr uint8_t ICON_FOOTPRINTS = 47;

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
           const bool bold = false, const bool black = true, const TextAlign align = TextAlign::Left) {
  canvas.text({bounds, font, align, bold, black}, value);
}

/// A text box that starts at (x, y) and is exactly tall enough for its rung, so
/// callers reason about rows in real pixels instead of guessing a height.
Rect textBox(const int x, const int y, const int width, const FontRole font) {
  return {x, y, width, ascenderFor(font)};
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
bool describeRain(const std::array<uint8_t, RAIN_BUCKET_COUNT>& buckets, const uint16_t rainStartMinute,
                  char (&out)[32]) {
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
    std::snprintf(out, sizeof(out), "DROOG TOT %s", endTime);
    return true;
  }

  if (firstWet == 0) {
    size_t firstDry = 0;
    while (firstDry < RAIN_BUCKET_COUNT && buckets[firstDry] > 0) ++firstDry;
    if (firstDry == RAIN_BUCKET_COUNT) {
      char endTime[8];
      timeAt(RAIN_BUCKET_COUNT, endTime);
      std::snprintf(out, sizeof(out), "REGEN TOT NA %s", endTime);
      return true;
    }
    char dryTime[8];
    timeAt(firstDry, dryTime);
    std::snprintf(out, sizeof(out), "%s DROOG", dryTime);
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
  const char* word = peak >= 3 ? "HEVIGE" : peak == 2 ? "MATIGE" : "LICHTE";
  char startTime[8];
  timeAt(firstWet, startTime);
  std::snprintf(out, sizeof(out), "%s %s REGEN", startTime, word);
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

    if (y + ascenderFor(FontRole::Value) <= bottom) {
      if (lead.changeBasisPoints == INT16_MIN) {
        label(canvas, textBox(left, y, width, FontRole::Value), "-", FontRole::Value, true);
      } else {
        constexpr int leadMarkSize = 13;
        const int leadMarkY = y + (ascenderFor(FontRole::Value) - leadMarkSize) / 2;
        triangle(canvas, {left, leadMarkY, leadMarkSize, leadMarkSize}, lead.changeBasisPoints >= 0);
        char change[16];
        formatChange(lead.changeBasisPoints, change);
        label(canvas, textBox(left + leadMarkSize + 8, y, width - leadMarkSize - 8, FontRole::Value), change,
              FontRole::Value, true);
      }
      y += ascenderFor(FontRole::Value) + 10;
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

}  // namespace

void applyDashboardV3DeviceBattery(DashboardV3Package& package, const uint16_t percentage) {
  package.status.x3Battery = static_cast<uint8_t>(std::min<uint16_t>(percentage, 100));
}

void renderDashboardV3(DashboardV3Canvas& canvas, const DashboardV3Package& package, const uint16_t minuteOfDay,
                       const uint8_t utcOffsetQ) {
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
    const std::string bounded = renderer_.truncatedText(id, value, spec.bounds.width, style);
    const int textWidth = renderer_.getTextWidth(id, bounded.c_str(), style);
    int x = spec.bounds.x;
    if (spec.align == TextAlign::Center) x += (spec.bounds.width - textWidth) / 2;
    if (spec.align == TextAlign::Right) x += spec.bounds.width - textWidth;
    renderer_.drawText(id, x, spec.bounds.y, bounded.c_str(), spec.black, style);
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
