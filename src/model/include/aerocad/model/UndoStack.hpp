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
    /// Number of applied entries (entries after it can be redone).
    [[nodiscard]] std::size_t position() const { return m_index; }

    void clear();

    /// The stack's entries and position, without applying anything. Used to make a
    /// temporary change (a dialog preview) and then put the history back exactly as it was,
    /// redo entries included.
    struct Snapshot {
        std::vector<UndoEntry> entries;
        std::size_t index = 0;
    };
    [[nodiscard]] Snapshot snapshot() const { return {m_entries, m_index}; }
    /// Replaces entries and position; the document must already be in the matching state.
    void restore(Snapshot snapshot)
    {
        m_entries = std::move(snapshot.entries);
        m_index = snapshot.index;
    }

private:
    std::vector<UndoEntry> m_entries;
    std::size_t m_index = 0; // entries [0, m_index) are applied
    std::size_t m_limit;
};

} // namespace aerocad::model
