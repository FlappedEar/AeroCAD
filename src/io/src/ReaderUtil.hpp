// SPDX-License-Identifier: GPL-3.0-or-later
// Private helpers shared by the format readers.
#pragma once

#include "aerocad/core/Path.hpp"
#include "aerocad/io/Import.hpp"

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aerocad::io::detail {

/// The OCCT data exchange readers and writers share global state (Interface_Static
/// parameters, static controllers), so every use is serialized with this mutex. The first
/// call also routes OCCT's messages from stdout to stderr.
[[nodiscard]] std::mutex& occtMutex();

/// UTF-8 representation of a path (for messages and for APIs taking UTF-8 char*).
using core::pathToUtf8;

/// Reads the whole file in binary mode. Throws ImportError on failure.
[[nodiscard]] std::string readFileBytes(const std::filesystem::path& path);

/// Parses a floating point number independent of the global C/C++ locale.
/// The whole string must be consumed (a leading '+' is accepted).
[[nodiscard]] bool parseDouble(std::string_view text, double& out);

/// Parses a decimal integer (optional sign). The whole string must be consumed.
[[nodiscard]] bool parseInt(std::string_view text, long long& out);

/// ASCII case-insensitive comparison.
[[nodiscard]] bool iequals(std::string_view a, std::string_view b);

[[nodiscard]] constexpr bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

[[nodiscard]] std::string_view trim(std::string_view s);

/// Splits off the next whitespace-delimited token; returns an empty view at the end.
[[nodiscard]] std::string_view nextToken(std::string_view& s);

/// Collects warnings, capping repeated messages so huge broken files do not
/// produce millions of entries.
class WarningSink {
public:
    explicit WarningSink(std::vector<std::string>& out, std::size_t limit = 20)
        : m_out(out), m_limit(limit)
    {
    }
    WarningSink(const WarningSink&) = delete;
    WarningSink& operator=(const WarningSink&) = delete;
    ~WarningSink();

    void add(std::string message);

private:
    std::vector<std::string>& m_out;
    std::size_t m_limit;
    std::size_t m_count = 0;
};

} // namespace aerocad::io::detail
