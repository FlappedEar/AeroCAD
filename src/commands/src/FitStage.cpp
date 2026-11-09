// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/FitStage.hpp"

#include <utility>

namespace aerocad::commands {

void FitStage::put(const std::string& key, Result result)
{
    const std::lock_guard lock(m_mutex);
    m_results.insert_or_assign(key, std::move(result));
}

std::optional<FitStage::Result> FitStage::take(const std::string& key)
{
    const std::lock_guard lock(m_mutex);
    auto it = m_results.find(key);
    if (it == m_results.end())
        return std::nullopt;
    auto node = m_results.extract(it);
    return std::make_optional(std::move(node.mapped()));
}

void FitStage::clear()
{
    const std::lock_guard lock(m_mutex);
    m_results.clear();
}

} // namespace aerocad::commands
