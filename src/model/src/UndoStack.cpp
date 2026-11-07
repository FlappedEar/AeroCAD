// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/model/UndoStack.hpp"

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>

namespace aerocad::model {

void UndoStack::push(UndoEntry entry)
{
    m_entries.resize(m_index);
    entry.id = m_nextId++;
    m_entries.push_back(std::move(entry));
    if (m_limit > 0 && m_entries.size() > m_limit)
        m_entries.erase(m_entries.begin());
    m_index = m_entries.size();
}

bool UndoStack::undo()
{
    if (!canUndo())
        return false;
    --m_index;
    if (m_entries[m_index].undo)
        m_entries[m_index].undo();
    return true;
}

bool UndoStack::redo()
{
    if (!canRedo())
        return false;
    if (m_entries[m_index].redo)
        m_entries[m_index].redo();
    ++m_index;
    return true;
}

std::string UndoStack::undoLabel() const
{
    return canUndo() ? m_entries[m_index - 1].label : std::string{};
}

std::string UndoStack::redoLabel() const
{
    return canRedo() ? m_entries[m_index].label : std::string{};
}

bool UndoStack::mergeLast(std::size_t count, std::string label)
{
    if (count < 2 || count > m_index)
        return false;
    m_entries.resize(m_index);
    std::vector<UndoEntry> parts(std::make_move_iterator(m_entries.end() - static_cast<std::ptrdiff_t>(count)),
                                 std::make_move_iterator(m_entries.end()));
    m_entries.erase(m_entries.end() - static_cast<std::ptrdiff_t>(count), m_entries.end());
    auto shared = std::make_shared<std::vector<UndoEntry>>(std::move(parts));
    UndoEntry merged;
    merged.label = std::move(label);
    merged.id = m_nextId++;
    merged.undo = [shared]() {
        for (auto it = shared->rbegin(); it != shared->rend(); ++it)
            if (it->undo)
                it->undo();
    };
    merged.redo = [shared]() {
        for (auto& e : *shared)
            if (e.redo)
                e.redo();
    };
    m_entries.push_back(std::move(merged));
    m_index = m_entries.size();
    return true;
}

std::vector<std::uint64_t> UndoStack::topIds(std::size_t count) const
{
    count = std::min(count, m_index);
    std::vector<std::uint64_t> out;
    out.reserve(count);
    for (std::size_t i = m_index - count; i < m_index; ++i)
        out.push_back(m_entries[i].id);
    return out;
}

bool UndoStack::isApplied(std::uint64_t id) const
{
    return id != 0 && std::any_of(m_entries.begin(), m_entries.begin() + static_cast<std::ptrdiff_t>(m_index),
                                  [id](const UndoEntry& e) { return e.id == id; });
}

bool UndoStack::mergeIds(const std::vector<std::uint64_t>& ids, std::string label)
{
    if (ids.size() < 2 || topIds(ids.size()) != ids)
        return false;
    return mergeLast(ids.size(), std::move(label));
}

void UndoStack::clear()
{
    m_entries.clear();
    m_index = 0;
}

} // namespace aerocad::model
