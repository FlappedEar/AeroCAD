// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace aerocad::model {

/// One undoable step. `redo` is not called on push: the change is already applied.
struct UndoEntry {
    std::string label;
    std::function<void()> undo;
    std::function<void()> redo;
};

class UndoStack {
public:
    explicit UndoStack(std::size_t limit = 200) : m_limit(limit) {}

    /// Records an already applied change and drops the redo history.
    void push(UndoEntry entry);

    bool undo();
    bool redo();

    [[nodiscard]] bool canUndo() const { return m_index > 0; }
    [[nodiscard]] bool canRedo() const { return m_index < m_entries.size(); }
    [[nodiscard]] std::string undoLabel() const;
    [[nodiscard]] std::string redoLabel() const;
    [[nodiscard]] std::size_t size() const { return m_entries.size(); }

    void clear();

private:
    std::vector<UndoEntry> m_entries;
    std::size_t m_index = 0; // entries [0, m_index) are applied
    std::size_t m_limit;
};

} // namespace aerocad::model
