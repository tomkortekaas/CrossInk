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

}  // namespace v2
}  // namespace dashboard
