#include "DashboardV3Layout.h"

namespace dashboard::v3 {
namespace {

constexpr int REFERENCE_WIDTH = 528;
constexpr int REFERENCE_HEIGHT = 792;
constexpr int HEADER_HEIGHT = 77;
// The rain band shrank from 117 when its chart became the mock-up's fixed
// 24 px intensity strip. The 21 px it gave up went to the body, which is the
// band that runs out of room first: it is where the agenda rows live.
constexpr int RAIN_HEIGHT = 96;
constexpr int TRAFFIC_HEIGHT = 77;
constexpr int BODY_HEIGHT = 480;
constexpr int LEFT_COLUMN_WIDTH = 270;

int scaled(const int available, const int reference) {
  return available * reference / REFERENCE_HEIGHT;
}

}  // namespace

DashboardV3Rects computeDashboardV3Layout(const int width, const int height, const Insets safe) {
  const int usableWidth = width - safe.left - safe.right;
  const int usableHeight = height - safe.top - safe.bottom;
  if (usableWidth <= 0 || usableHeight <= 0 || safe.left < 0 || safe.top < 0 || safe.right < 0 || safe.bottom < 0) {
    return {};
  }

  const int headerHeight = scaled(usableHeight, HEADER_HEIGHT);
  const int rainHeight = scaled(usableHeight, RAIN_HEIGHT);
  const int trafficHeight = scaled(usableHeight, TRAFFIC_HEIGHT);
  const int bodyHeight = scaled(usableHeight, BODY_HEIGHT);
  const int footerHeight = usableHeight - headerHeight - rainHeight - trafficHeight - bodyHeight;

  DashboardV3Rects result{};
  int y = safe.top;
  result.header = {safe.left, y, usableWidth, headerHeight};
  y += headerHeight;
  result.rain = {safe.left, y, usableWidth, rainHeight};
  y += rainHeight;
  result.traffic = {safe.left, y, usableWidth, trafficHeight};
  y += trafficHeight;
  result.body = {safe.left, y, usableWidth, bodyHeight};
  y += bodyHeight;
  result.footer = {safe.left, y, usableWidth, footerHeight};

  const int leftWidth = usableWidth * LEFT_COLUMN_WIDTH / REFERENCE_WIDTH;
  result.bodyLeft = {safe.left, result.body.y, leftWidth, bodyHeight};
  result.bodyRight = {safe.left + leftWidth, result.body.y, usableWidth - leftWidth, bodyHeight};
  return result;
}

}  // namespace dashboard::v3
