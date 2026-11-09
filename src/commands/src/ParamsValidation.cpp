// SPDX-License-Identifier: GPL-3.0-or-later
#include "ParamsValidation.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace aerocad::commands {

namespace {

bool matchesType(const std::string& type, const nlohmann::json& v)
{
    if (type == "object")
        return v.is_object();
    if (type == "array")
        return v.is_array();
    if (type == "string")
        return v.is_string();
    if (type == "number")
        return v.is_number();
    if (type == "integer")
        return v.is_number_integer() || (v.is_number_float() && v.get<double>() == static_cast<double>(static_cast<long long>(v.get<double>())));
    if (type == "boolean")
        return v.is_boolean();
    if (type == "null")
        return v.is_null();
    return true; // unknown types are not enforced
}

const char* typeName(const nlohmann::json& v)
{
    return v.type_name();
}

/// "(accepted: a, b, c)" with a "did you mean" for a near miss (id for ids, ...).
std::string acceptedKeys(const nlohmann::json& props, const std::string& key)
{
    if (!props.is_object() || props.empty())
        return " (this takes no parameters)";
    std::string list, guess;
    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    // Words agents use for what AeroCAD calls otherwise (feature_hole center -> point).
    static const std::pair<const char*, const char*> synonyms[] = {
        {"center", "point"},    {"centre", "point"},     {"position", "point"}, {"location", "point"},
        {"length", "distance"}, {"depth", "distance"},   {"amount", "distance"}, {"offset", "distance"},
        {"part", "id"},         {"part_id", "id"},       {"partid", "id"},       {"body_id", "body"},
        {"sketch_id", "sketch"}, {"feature", "id"},      {"file", "path"},       {"filename", "path"},
        {"dir", "folder"},      {"directory", "folder"}, {"r", "radius"},        {"d", "diameter"},
        {"axis", "direction"},  {"vector", "direction"}, {"title", "name"},      {"label", "name"}};
    for (const auto& [from, to] : synonyms)
        if (lower(key) == from && props.contains(to)) {
            guess = to;
            break;
        }
    for (auto it = props.begin(); it != props.end(); ++it) {
        list += (list.empty() ? "" : ", ") + it.key();
        const std::string a = lower(it.key()), b = lower(key);
        // id -> ids, r -> radius, centre -> center: plural, prefix, or a long shared stem.
        if (guess.empty() && (a == b + "s" || b == a + "s" || a.rfind(b, 0) == 0 ||
                              (b.size() >= 4 && a.size() >= 4 && a.compare(0, 4, b, 0, 4) == 0)))
            guess = it.key();
    }
    return (guess.empty() ? std::string() : " - did you mean '" + guess + "'?") + " (accepted: " + list + ")";
}

} // namespace

std::optional<std::string> validateParams(const nlohmann::json& schema, const nlohmann::json& value, const std::string& path)
{
    if (!schema.is_object())
        return std::nullopt;

    if (auto it = schema.find("type"); it != schema.end() && it->is_string()) {
        if (!matchesType(it->get<std::string>(), value))
            return path + ": expected " + it->get<std::string>() + ", got " + typeName(value);
    } else if (it != schema.end() && it->is_array()) {
        // Union type, e.g. ["number", "string"] for a number or an expression.
        const bool any = std::any_of(it->begin(), it->end(), [&](const nlohmann::json& t) {
            return t.is_string() && matchesType(t.get<std::string>(), value);
        });
        if (!any)
            return path + ": expected " + it->dump() + ", got " + typeName(value);
    }

    if (auto it = schema.find("enum"); it != schema.end() && it->is_array()) {
        if (std::find(it->begin(), it->end(), value) == it->end())
            return path + ": value " + value.dump() + " not allowed (one of " + it->dump() + ")";
    }

    if (value.is_number()) {
        const double x = value.get<double>();
        if (auto it = schema.find("minimum"); it != schema.end() && x < it->get<double>())
            return path + ": must be >= " + it->dump();
        if (auto it = schema.find("maximum"); it != schema.end() && x > it->get<double>())
            return path + ": must be <= " + it->dump();
        if (auto it = schema.find("exclusiveMinimum"); it != schema.end() && x <= it->get<double>())
            return path + ": must be > " + it->dump();
        if (auto it = schema.find("exclusiveMaximum"); it != schema.end() && x >= it->get<double>())
            return path + ": must be < " + it->dump();
    }

    if (value.is_string()) {
        if (auto it = schema.find("minLength"); it != schema.end() && value.get<std::string>().size() < it->get<std::size_t>())
            return path + ": too short";
    }

    if (value.is_array()) {
        if (auto it = schema.find("minItems"); it != schema.end() && value.size() < it->get<std::size_t>())
            return path + ": too few items";
        if (auto it = schema.find("maxItems"); it != schema.end() && value.size() > it->get<std::size_t>())
            return path + ": too many items";
        if (auto it = schema.find("items"); it != schema.end()) {
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (auto err = validateParams(*it, value[i], path + "[" + std::to_string(i) + "]"))
                    return err;
            }
        }
    }

    if (value.is_object()) {
        const auto props = schema.find("properties");
        for (const auto& [key, item] : value.items()) {
            if (props != schema.end() && props->contains(key)) {
                if (auto err = validateParams((*props)[key], item, path + "." + key))
                    return err;
            } else if (auto ap = schema.find("additionalProperties"); ap != schema.end() && ap->is_boolean() && !ap->get<bool>()) {
                return path + "." + key + ": unknown parameter" +
                       acceptedKeys(props != schema.end() ? *props : nlohmann::json::object(), key);
            }
        }
        // After the unknown keys: a misspelt key usually is the missing required one, and
        // its message names the right key.
        if (auto it = schema.find("required"); it != schema.end() && it->is_array()) {
            for (const auto& key : *it) {
                if (!value.contains(key.get<std::string>()))
                    return path + "." + key.get<std::string>() + ": required" +
                           (props != schema.end() && props->contains(key.get<std::string>()) &&
                                    (*props)[key.get<std::string>()].contains("description")
                                ? " (" + (*props)[key.get<std::string>()]["description"].get<std::string>() + ")"
                                : std::string());
            }
        }
    }
    return std::nullopt;
}

} // namespace aerocad::commands
