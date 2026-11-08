// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aerocad::model {

/// One undoable step. `redo` is not called on push: the change is already applied.
struct UndoEntry {
    std::string label;
    std::function<void()> undo;
    std::function<void()> redo;
    /// Stable token set by UndoStack::push / mergeLast, unique for the stack's lifetime
    /// (also across clear()). Lets a caller recognise its own steps later.
    std::uint64_t id = 0;
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

    /// Token of the last applied entry, 0 when nothing can be undone.
    [[nodiscard]] std::uint64_t topId() const { return m_index > 0 ? m_entries[m_index - 1].id : 0; }
    /// Tokens of the last `count` applied entries, oldest first (fewer when fewer are applied).
    [[nodiscard]] std::vector<std::uint64_t> topIds(std::size_t count) const;
    /// Whether the entry with token `id` is applied (in the history and not undone).
    [[nodiscard]] bool isApplied(std::uint64_t id) const;
    /// Whether the entry with token `id` left the history while applied: dropped as the
    /// oldest one past the limit, or by clear(). Its change stays done for good, unlike an
    /// entry that was undone (and maybe discarded by a later push) or merged into another.
    [[nodiscard]] bool isExpired(std::uint64_t id) const;

    /// Joins the last `count` applied entries into one step called `label` (a dialog that
    /// ran a helper command on the way, such as making a solid editable before a fillet).
    /// Redo entries are dropped. Returns false (and changes nothing) when fewer than `count`
    /// entries are applied or count < 2.
    bool mergeLast(std::size_t count, std::string label);
    /// Like mergeLast, but only when the last applied entries are exactly `ids` (oldest
    /// first): a dialog joins its own steps and never someone else's. Returns false and
    /// changes nothing otherwise.
    bool mergeIds(const std::vector<std::uint64_t>& ids, std::string label);

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
    std::uint64_t m_nextId = 1;
    std::vector<std::uint64_t> m_expired; // ascending (see isExpired)
};

} // namespace aerocad::model
