// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <nlohmann/json.hpp>

#include <QVariant>

namespace aerocad::app {

/// Conversions between QML values (QVariant) and command-layer JSON.
nlohmann::json toJson(const QVariant& value);
QVariant toVariant(const nlohmann::json& value);

} // namespace aerocad::app
