#pragma once

#include <cstddef>
#include <cstdint>

namespace dashboard::v3 {

// Append-only quote table, mirroring Sources/DashboardCore/DashboardV3Quotes.swift
// in the iOS repository entry for entry. That Swift table is the source of
// truth for the ordering.
//
// The BLE package carries a quote id, not the text, so the two tables must stay
// byte-for-byte ordered forever: inserting or reordering an entry silently
// changes what an already-sent package renders. New quotes go at the end of
// both tables.
//
// Ids are **zero based**, because the phone computes them as
// `dayNumber % quotes.count`. There is deliberately no "no quote" sentinel: id 0
// is the first quote, and a package that never carried a real id still renders a
// valid one rather than an empty footer.

struct Quote {
  const char* text;
  const char* author;
};

static constexpr Quote kQuotes[] = {
    {"Verbeelding is belangrijker dan kennis.", "Albert Einstein"},
    {"Eenvoud is de ultieme verfijning.", "Leonardo da Vinci"},
    {"Minder is meer.", "Ludwig Mies van der Rohe"},
    {"Pluk de dag.", "Horatius"},
    {"Geluk is waar voorbereiding en kans elkaar ontmoeten.", "Seneca"},
    {"Een reis van duizend mijl begint met \xc3\xa9\xc3\xa9n stap.", "Laozi"},
    {"Wie niet waagt, die niet wint.", "Nederlands spreekwoord"},
    {"Kennis spreekt, maar wijsheid luistert.", "Jimi Hendrix"},
};

static constexpr size_t QUOTE_COUNT = sizeof(kQuotes) / sizeof(kQuotes[0]);

/// Resolves a wire id to its quote, or nullptr when the id points past the table
/// this firmware knows. A newer phone may send an id from a longer table;
/// drawing nothing is better than drawing the wrong quote.
inline const Quote* quoteForId(const uint8_t quoteId) {
  if (quoteId >= QUOTE_COUNT) return nullptr;
  return &kQuotes[quoteId];
}

}  // namespace dashboard::v3
