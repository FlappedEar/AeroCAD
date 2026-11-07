// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/ImportStage.hpp"

#include <utility>

namespace aerocad::commands {

void ImportStage::put(const std::string& path, io::ImportResult result)
{
    const std::lock_guard lock(m_mutex);
    m_results.insert_or_assign(path, std::move(result));
}

std::optional<io::ImportResult> ImportStage::take(const std::string& path)
{
    const std::lock_guard lock(m_mutex);
    auto it = m_results.find(path);
    if (it == m_results.end())
        return std::nullopt;
    auto node = m_results.extract(it);
    return std::make_optional(std::move(node.mapped()));
}

void ImportStage::clear()
{
    const std::lock_guard lock(m_mutex);
    m_results.clear();
}

} // namespace aerocad::commands
