// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/model/UndoStack.hpp"

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

void UndoStack::clear()
{
    m_entries.clear();
    m_index = 0;
}

} // namespace aerocad::model
