#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "BleHandoffRecord.h"

// Template 4: het herontworpen dashboard. Staat naast TEMPLATE_WIDGET_GRID (3)
// in plaats van dat te vervangen, zodat de telefoon kan kiezen welke hij stuurt
// en een terugweg een schakelaar is in plaats van een flash. Template 3 wordt
// door dit bestand niet aangeraakt.
//
// De eerste 28 bytes (SHARED_PREFIX_SIZE) zijn gedeeld met elke andere
// template; alles daarna vult template 4 zelf in.
namespace dashboard {
namespace v2 {

constexpr uint8_t TEMPLATE_WIDGET_GRID_V2 = 4;

// Twaalf is deelbaar door 2, 3, 4 en 6 in beide richtingen, dus halven, derden,
// kwarten en zesden vallen zonder rest. De echte winst zit in de hoogte: rijen
// van 64 px maken een kop- en voetbalk mogelijk, wat met rijen van 128 niet kon.
constexpr uint8_t GRID_COLUMNS = 12;
constexpr uint8_t MAX_ROW_SPAN = 12;

// NIET GRID_COLUMNS * MAX_ROW_SPAN: dat zou 144 zijn. Zestien tegels is ruim
// meer dan iemand op 528x792 componeert (het ontwerp gebruikt er acht) en houdt
// de widget-array betaalbaar. Zie de static_assert in het .cpp-bestand.
constexpr size_t MAX_WIDGETS = 16;

// Widgettypes 1-3 houden dezelfde nummers als in template 3, zodat het lezen
// van beide formaten naast elkaar geen mentale vertaalslag kost. Group is nieuw.
enum class WidgetType : uint8_t { Kpi = 1, List = 2, Date = 3, Group = 4 };

// Welke veldwaarde van vandaag een Date-widget toont. Identiek aan template 3:
// de datum reist nooit mee, de X3 leest zijn eigen RTC.
enum class DateField : uint8_t { Auto = 0, Day = 1, Weekday = 2, Month = 3, Year = 4, WeekNumber = 5 };
constexpr uint8_t MAX_DATE_FIELD = 5;

// Vormen van een Group. Een enkele meter is itemCount 1 zonder kop; een cluster
// is hetzelfde widget met meer items. Daarom is dit één type en niet drie.
constexpr uint8_t GROUP_SHAPE_ARC = 0;    // 270-graden boog, vulling 0-100
constexpr uint8_t GROUP_SHAPE_BAR = 1;    // horizontale balk met streepje op de grens
constexpr uint8_t GROUP_SHAPE_STRIP = 2;  // label/waarde-paren zonder meter
constexpr uint8_t MAX_GROUP_SHAPE = 2;

// Vulling in procenten. FILL_NONE moet buiten 0..100 vallen: "geen meter"
// (een strip-item) is iets anders dan "meter op nul" (een lege accu), en die
// twee mogen niet hetzelfde tekenen.
constexpr uint8_t MAX_FILL = 100;
constexpr uint8_t FILL_NONE = 255;

// Zes is waar de bogen ophouden leesbaar te zijn: op 504 px is een zevende item
// smaller dan ~84 px en past het getal niet meer in de boog. Voorbij zes moet de
// renderer naar gestapelde balken, niet het formaat naar meer items.
constexpr size_t MAX_GROUP_ITEMS = 6;
// Vijf groepen, want het ontwerp gebruikt er precies vijf: het accu-cluster,
// de kopbalk, de voetbalk, en stappen en reistijd als groepen van een item.
// Zie de static_assert in het .cpp-bestand voor wat dit aan DRAM kost.
constexpr size_t MAX_GROUP_WIDGETS = 5;

constexpr size_t MAX_GROUP_HEADING_SIZE = 24;
constexpr size_t MAX_GROUP_LABEL_SIZE = 16;
constexpr size_t MAX_GROUP_VALUE_SIZE = 16;
constexpr size_t MAX_GROUP_DETAIL_SIZE = 16;

// Label 32 in plaats van 16: "naar werk via de A9" hoeft niet afgekort te
// worden. Alleen voor Kpi-widgets; Group-items hebben hun eigen, krappere
// budget omdat er zes van in een tegel passen.
constexpr size_t MAX_KPI_LABEL_SIZE = 32;
constexpr size_t MAX_KPI_VALUE_SIZE = 16;

constexpr size_t MAX_LIST_HEADING_SIZE = 32;
constexpr size_t MAX_LIST_ROWS = 8;
constexpr size_t MAX_LIST_ROW_TIME_SIZE = 16;
constexpr size_t MAX_LIST_ROW_LABEL_SIZE = 40;
constexpr size_t MAX_LIST_WIDGETS = 3;

struct GroupItem {
  std::array<uint8_t, MAX_GROUP_LABEL_SIZE> labelBytes{};
  uint8_t labelLength = 0;
  std::array<uint8_t, MAX_GROUP_VALUE_SIZE> valueBytes{};
  uint8_t valueLength = 0;
  std::array<uint8_t, MAX_GROUP_DETAIL_SIZE> detailBytes{};
  uint8_t detailLength = 0;
  // 0..MAX_FILL, of FILL_NONE als dit item geen meter heeft.
  uint8_t fill = FILL_NONE;
};

struct GroupContent {
  std::array<uint8_t, MAX_GROUP_HEADING_SIZE> headingBytes{};
  uint8_t headingLength = 0;
  uint8_t shape = GROUP_SHAPE_ARC;
  std::array<GroupItem, MAX_GROUP_ITEMS> items{};
  uint8_t itemCount = 0;
};

struct KpiContentV2 {
  std::array<uint8_t, MAX_KPI_LABEL_SIZE> labelBytes{};
  uint8_t labelLength = 0;
  std::array<uint8_t, MAX_KPI_VALUE_SIZE> valueBytes{};
  uint8_t valueLength = 0;
};

struct ListRowV2 {
  std::array<uint8_t, MAX_LIST_ROW_TIME_SIZE> timeBytes{};
  uint8_t timeLength = 0;
  std::array<uint8_t, MAX_LIST_ROW_LABEL_SIZE> labelBytes{};
  uint8_t labelLength = 0;
};

struct ListContentV2 {
  std::array<uint8_t, MAX_LIST_HEADING_SIZE> headingBytes{};
  uint8_t headingLength = 0;
  std::array<ListRowV2, MAX_LIST_ROWS> rows{};
  uint8_t rowCount = 0;
};

struct WidgetV2 {
  WidgetType type = WidgetType::Kpi;
  uint8_t column = 0;
  uint8_t row = 0;
  uint8_t columnSpan = 1;
  uint8_t rowSpan = 1;
  // Eén byte waarvan de betekenis het type volgt: het lijstnummer, het
  // datumveld, of het groepsnummer. Een widget is nooit twee van de drie, en de
  // byte delen is geen micro-optimalisatie: elk apart veld groeit ×MAX_WIDGETS.
  union {
    uint8_t listIndex = 0;
    DateField dateField;
    uint8_t groupIndex;
  };
  uint16_t style = 0;
  KpiContentV2 kpi{};
};

struct WidgetGridPackageV2 {
  uint8_t schema = SCHEMA_V2;
  uint8_t templateId = TEMPLATE_WIDGET_GRID_V2;
  uint32_t packageId = 0;
  uint64_t generatedAt = 0;
  uint64_t validUntil = 0;
  uint8_t style = 0;
  std::array<WidgetV2, MAX_WIDGETS> widgets{};
  std::array<ListContentV2, MAX_LIST_WIDGETS> lists{};
  std::array<GroupContent, MAX_GROUP_WIDGETS> groups{};
  uint8_t widgetCount = 0;
  uint8_t listCount = 0;
  uint8_t groupCount = 0;
  uint32_t crc = 0;
};

// De inhoud waar `widget` naar wijst, of nullptr als het geen widget van dat
// type is of het nummer buiten bereik valt. Renderen en meten moet hier
// doorheen, niet rechtstreeks in `lists`/`groups` indexeren.
const ListContentV2* listContentFor(const WidgetGridPackageV2& package, const WidgetV2& widget);
const GroupContent* groupContentFor(const WidgetGridPackageV2& package, const WidgetV2& widget);

// Publiek zodat de tests één widget los kunnen keuren; encode en decode roepen
// dit voor elk widget aan.
Status validateWidgetV2(const WidgetGridPackageV2& package, const WidgetV2& widget);

Status encodeWidgetGridPackageV2(const WidgetGridPackageV2& package, PackageBytes& output, size_t& outputLength);
Status decodeWidgetGridPackageV2(const uint8_t* bytes, size_t size, WidgetGridPackageV2& output);

}  // namespace v2
}  // namespace dashboard
