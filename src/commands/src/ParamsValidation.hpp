// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace aerocad::commands {

/// Validates `value` against a JSON Schema subset: type (object, array, string, number,
/// integer, boolean, null), properties, required, additionalProperties (bool), items,
/// enum, minimum, maximum, exclusiveMinimum, exclusiveMaximum, minItems, maxItems,
/// minLength. Returns an error message or std::nullopt when valid.
[[nodiscard]] std::optional<std::string> validateParams(const nlohmann::json& schema, const nlohmann::json& value,
                                                        const std::string& path = "params");

} // namespace aerocad::commands
