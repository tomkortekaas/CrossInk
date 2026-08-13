#include "DashboardBootSwitch.h"

#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <spi_flash_mmap.h>

#include <cstdint>
#include <cstring>

namespace dashboard_boot {
namespace {

struct __attribute__((packed)) SelectEntry {
  uint32_t otaSeq;
  uint8_t label[20];
  uint32_t state;
  uint32_t crc;
};
static_assert(sizeof(SelectEntry) == 32);

uint32_t sequenceCrc(const uint32_t sequence) {
  return esp_rom_crc32_le(UINT32_MAX, reinterpret_cast<const uint8_t*>(&sequence), sizeof(sequence));
}

bool switchToSubtype(const esp_partition_subtype_t subtype) {
  const esp_partition_t* destination = esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, nullptr);
  uint8_t magic = 0;
  if (destination == nullptr || esp_partition_read(destination, 0, &magic, sizeof(magic)) != ESP_OK || magic != 0xE9) {
    return false;
  }
  const esp_partition_t* otadata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (otadata == nullptr || otadata->size < 2 * SPI_FLASH_SEC_SIZE) return false;

  SelectEntry entries[2]{};
  if (esp_partition_read(otadata, 0, &entries[0], sizeof(entries[0])) != ESP_OK ||
      esp_partition_read(otadata, SPI_FLASH_SEC_SIZE, &entries[1], sizeof(entries[1])) != ESP_OK) {
    return false;
  }
  int active = -1;
  uint32_t activeSequence = 0;
  for (int index = 0; index < 2; ++index) {
    if (entries[index].otaSeq == UINT32_MAX || entries[index].crc != sequenceCrc(entries[index].otaSeq) ||
        entries[index].state == 3 || entries[index].state == 4) {
      continue;
    }
    if (active < 0 || entries[index].otaSeq > activeSequence) {
      active = index;
      activeSequence = entries[index].otaSeq;
    }
  }
  const uint32_t destinationIndex =
      static_cast<uint32_t>(subtype) - static_cast<uint32_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  uint32_t nextSequence = activeSequence + 1;
  while (((nextSequence - 1U) % 2U) != destinationIndex) ++nextSequence;

  SelectEntry next{};
  next.otaSeq = nextSequence;
  std::memset(next.label, 0xFF, sizeof(next.label));
  next.state = 0;
  next.crc = sequenceCrc(nextSequence);
  const size_t targetOffset = static_cast<size_t>(active == 0 ? 1 : 0) * SPI_FLASH_SEC_SIZE;
  return esp_partition_erase_range(otadata, targetOffset, SPI_FLASH_SEC_SIZE) == ESP_OK &&
         esp_partition_write(otadata, targetOffset, &next, sizeof(next)) == ESP_OK;
}

}  // namespace

bool switchToReader() { return switchToSubtype(ESP_PARTITION_SUBTYPE_APP_OTA_0); }
bool switchToReceiver() { return switchToSubtype(ESP_PARTITION_SUBTYPE_APP_OTA_1); }

}  // namespace dashboard_boot
