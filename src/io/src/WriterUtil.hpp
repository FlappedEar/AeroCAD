// SPDX-License-Identifier: GPL-3.0-or-later
// Private helpers shared by the format writers.
#pragma once

#include "aerocad/io/Export.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace aerocad::io::detail {

/// Name of the temporary file a writer fills before it is renamed to `target`.
[[nodiscard]] std::filesystem::path temporarySibling(const std::filesystem::path& target);

/// Renames `temporary` to `target` (replacing it). Removes `temporary` and throws
/// ExportError on failure.
void commitTemporary(const std::filesystem::path& temporary, const std::filesystem::path& target);

/// Writes `bytes` to `target` through a temporary file. Throws ExportError.
void writeFileAtomic(const std::filesystem::path& target, std::string_view bytes);

/// Appends the shortest round-trip decimal representation of `value`, independent of
/// the global locale.
void appendNumber(std::string& out, double value);

/// Appends `text` with XML special characters escaped (for attribute values).
void appendXmlEscaped(std::string& out, std::string_view text);

/// Throws ExportError unless every item has a valid, non-empty mesh.
void requireMeshes(const std::vector<ExportItem>& items, std::string_view formatName);

} // namespace aerocad::io::detail
