// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace aerocad::core {

/// UTF-8 std::string -> path (portable; std::filesystem::u8path is deprecated in C++20).
inline std::filesystem::path pathFromUtf8(std::string_view utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

/// path -> UTF-8 std::string (path::u8string() returns std::u8string in C++20).
inline std::string pathToUtf8(const std::filesystem::path& path)
{
    const std::u8string s = path.u8string();
    return std::string(s.begin(), s.end());
}

} // namespace aerocad::core
