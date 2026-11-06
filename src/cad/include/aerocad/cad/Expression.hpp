// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aerocad::cad {

class ExpressionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Looks up a named parameter; std::nullopt when unknown.
using ParameterLookup = std::function<std::optional<double>(std::string_view name)>;

/// Evaluates an arithmetic expression such as "width / 2 + 3 mm" or "2 * pi * r".
///
/// Grammar: numbers, parameter names, + - * / ^ (power, right-associative), unary
/// minus, parentheses, functions sqrt sin cos tan abs min max (angles in degrees), the
/// constant pi, and unit suffixes mm, cm, m, in, deg (lengths convert to millimetres,
/// angles stay in degrees). Throws ExpressionError on syntax errors and unknown names.
[[nodiscard]] double evaluateExpression(std::string_view expression, const ParameterLookup& lookup = {});

} // namespace aerocad::cad
