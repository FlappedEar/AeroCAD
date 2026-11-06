// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/Units.hpp"

#include <cctype>
#include <string>

namespace aerocad::core {

std::string_view unitSymbol(LengthUnit unit)
{
    switch (unit) {
    case LengthUnit::Micrometre:
        return "um";
    case LengthUnit::Millimetre:
        return "mm";
    case LengthUnit::Centimetre:
        return "cm";
    case LengthUnit::Metre:
        return "m";
    case LengthUnit::Inch:
        return "in";
    case LengthUnit::Foot:
        return "ft";
    }
    return "mm";
}

std::optional<LengthUnit> parseLengthUnit(std::string_view text)
{
    std::string s;
    s.reserve(text.size());
    for (const char c : text) {
        if (c != ' ')
            s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (s == "um" || s == "\xc2\xb5m" || s == "micron" || s == "microns" || s == "micrometre" || s == "micrometer" ||
        s == "micrometres" || s == "micrometers")
        return LengthUnit::Micrometre;
    if (s == "mm" || s == "millimetre" || s == "millimeter" || s == "millimetres" || s == "millimeters")
        return LengthUnit::Millimetre;
    if (s == "cm" || s == "centimetre" || s == "centimeter" || s == "centimetres" || s == "centimeters")
        return LengthUnit::Centimetre;
    if (s == "m" || s == "metre" || s == "meter" || s == "metres" || s == "meters")
        return LengthUnit::Metre;
    if (s == "in" || s == "inch" || s == "inches" || s == "\"")
        return LengthUnit::Inch;
    if (s == "ft" || s == "foot" || s == "feet" || s == "'")
        return LengthUnit::Foot;
    return std::nullopt;
}

std::vector<LengthUnit> allLengthUnits()
{
    return {LengthUnit::Micrometre, LengthUnit::Millimetre, LengthUnit::Centimetre,
            LengthUnit::Metre,      LengthUnit::Inch,       LengthUnit::Foot};
}

} // namespace aerocad::core
