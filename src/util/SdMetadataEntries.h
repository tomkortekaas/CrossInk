#pragma once

#include <string_view>

// Files and folders that operating systems leave on SD cards and that no browser should show.
namespace SdMetadataEntries {
bool isMacOS(std::string_view filename);
bool isWindows(std::string_view filename);
}  // namespace SdMetadataEntries
