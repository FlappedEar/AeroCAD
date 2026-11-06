// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace aerocad::core {

/// Length units a file may have been written in. The document always works in
/// millimetres; importing in another unit scales the geometry by toMillimetres().
enum class LengthUnit { Micrometre, Millimetre, Centimetre, Metre, Inch, Foot };

/// Size of one unit in millimetres.
[[nodiscard]] constexpr double toMillimetres(LengthUnit unit)
{
    switch (unit) {
    case LengthUnit::Micrometre:
        return 0.001;
    case LengthUnit::Millimetre:
        return 1.0;
    case LengthUnit::Centimetre:
        return 10.0;
    case LengthUnit::Metre:
        return 1000.0;
    case LengthUnit::Inch:
        return 25.4;
    case LengthUnit::Foot:
        return 304.8;
    }
    return 1.0;
}

/// Short symbol used in commands and the UI: "um", "mm", "cm", "m", "in", "ft".
[[nodiscard]] std::string_view unitSymbol(LengthUnit unit);

/// Parses a unit symbol or name ("mm", "millimetre", "inch", "\"", "µm", ...; case-insensitive).
[[nodiscard]] std::optional<LengthUnit> parseLengthUnit(std::string_view text);

/// All units, in the order shown in the UI.
[[nodiscard]] std::vector<LengthUnit> allLengthUnits();

} // namespace aerocad::core
