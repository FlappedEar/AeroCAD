// SPDX-License-Identifier: GPL-3.0-or-later
#include "ParamsValidation.hpp"

#include <algorithm>

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

} // namespace

std::optional<std::string> validateParams(const nlohmann::json& schema, const nlohmann::json& value, const std::string& path)
{
    if (!schema.is_object())
        return std::nullopt;

    if (auto it = schema.find("type"); it != schema.end() && it->is_string()) {
        if (!matchesType(it->get<std::string>(), value))
            return path + ": expected " + it->get<std::string>();
    }

    if (auto it = schema.find("enum"); it != schema.end() && it->is_array()) {
        if (std::find(it->begin(), it->end(), value) == it->end())
            return path + ": value not allowed";
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
        if (auto it = schema.find("required"); it != schema.end() && it->is_array()) {
            for (const auto& key : *it) {
                if (!value.contains(key.get<std::string>()))
                    return path + "." + key.get<std::string>() + ": required";
            }
        }
        for (const auto& [key, item] : value.items()) {
            if (props != schema.end() && props->contains(key)) {
                if (auto err = validateParams((*props)[key], item, path + "." + key))
                    return err;
            } else if (auto ap = schema.find("additionalProperties"); ap != schema.end() && ap->is_boolean() && !ap->get<bool>()) {
                return path + "." + key + ": unknown parameter";
            }
        }
    }
    return std::nullopt;
}

} // namespace aerocad::commands
