#pragma once

#include <FreeInkUI.h>
#include <I18n.h>

#include <cstdint>

namespace keyboard_layouts {

struct LayoutInfo {
  freeink::ui::KeyboardLayoutId id;
  Language language;
};

// Table position is the persisted bit assignment. Append future layouts so
// SDK enum changes cannot reinterpret an existing settings file.
inline constexpr LayoutInfo ALL[] = {
    {freeink::ui::KeyboardLayoutId::QwertyEn, Language::EN},
#if defined(CROSSINK_I18N_HAS_LANGUAGE_FR)
    {freeink::ui::KeyboardLayoutId::AzertyFr, Language::FR},
#else
    {freeink::ui::KeyboardLayoutId::AzertyFr, Language::EN},
#endif
#if defined(CROSSINK_I18N_HAS_LANGUAGE_DE)
    {freeink::ui::KeyboardLayoutId::QwertzDe, Language::DE},
#else
    {freeink::ui::KeyboardLayoutId::QwertzDe, Language::EN},
#endif
#if defined(CROSSINK_I18N_HAS_LANGUAGE_ES)
    {freeink::ui::KeyboardLayoutId::SpanishEs, Language::ES},
#else
    {freeink::ui::KeyboardLayoutId::SpanishEs, Language::EN},
#endif
#if defined(CROSSINK_I18N_HAS_LANGUAGE_RU)
    {freeink::ui::KeyboardLayoutId::CyrillicRu, Language::RU},
#else
    {freeink::ui::KeyboardLayoutId::CyrillicRu, Language::EN},
#endif
#if defined(CROSSINK_I18N_HAS_LANGUAGE_UK)
    {freeink::ui::KeyboardLayoutId::CyrillicUk, Language::UK},
#else
    {freeink::ui::KeyboardLayoutId::CyrillicUk, Language::EN},
#endif
#if defined(CROSSINK_I18N_HAS_LANGUAGE_BE)
    {freeink::ui::KeyboardLayoutId::CyrillicBe, Language::BE},
#else
    {freeink::ui::KeyboardLayoutId::CyrillicBe, Language::EN},
#endif
#if defined(CROSSINK_I18N_HAS_LANGUAGE_KK)
    {freeink::ui::KeyboardLayoutId::CyrillicKk, Language::KK},
#else
    {freeink::ui::KeyboardLayoutId::CyrillicKk, Language::EN},
#endif
#if defined(CROSSINK_I18N_HAS_LANGUAGE_HE)
    {freeink::ui::KeyboardLayoutId::HebrewIl, Language::HE},
#else
    {freeink::ui::KeyboardLayoutId::HebrewIl, Language::EN},
#endif
};
inline constexpr uint8_t COUNT = sizeof(ALL) / sizeof(ALL[0]);
static_assert(COUNT <= 16, "keyboard layout mask is uint16_t");

inline constexpr uint16_t bitAt(const uint8_t i) { return static_cast<uint16_t>(1u << i); }
inline constexpr uint16_t LATIN_BITS = bitAt(0) | bitAt(1) | bitAt(2) | bitAt(3);

uint16_t enabled();
freeink::ui::KeyboardLayoutId startingLayout();
freeink::ui::KeyboardLayoutId next(freeink::ui::KeyboardLayoutId current);

}  // namespace keyboard_layouts
