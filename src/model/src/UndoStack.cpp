// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/model/UndoStack.hpp"

#include <iterator>
#include <memory>
#include <utility>

namespace aerocad::model {

void UndoStack::push(UndoEntry entry)
{
    m_entries.resize(m_index);
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

void UndoStack::clear()
{
    m_entries.clear();
    m_index = 0;
}

} // namespace aerocad::model
