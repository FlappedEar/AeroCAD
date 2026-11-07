// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/io/Import.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace aerocad::commands {

/// Files parsed ahead of time, e.g. on a worker thread while the UI shows a progress
/// bar. document.import takes a staged result for its path instead of reading the file
/// again, so big scans load without freezing the UI and the document still changes only
/// through the command (same undo, same MCP-visible behaviour). Thread-safe.
class ImportStage {
public:
    /// Stores a parsed file under its path (UTF-8, as passed to document.import).
    void put(const std::string& path, io::ImportResult result);
    /// Removes and returns the staged result for `path`, if any.
    [[nodiscard]] std::optional<io::ImportResult> take(const std::string& path);
    void clear();

private:
    std::mutex m_mutex;
    std::map<std::string, io::ImportResult> m_results;
};

} // namespace aerocad::commands
