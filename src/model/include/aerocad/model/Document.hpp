// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/model/Part.hpp"

#include <cstdint>
#include <algorithm>
#include <functional>
#include <optional>
#include <vector>

namespace aerocad::model {

/// The open project. Owns parts and notifies observers about changes.
/// Not thread-safe: mutate from one thread (the UI thread) only.
class Document {
public:
    /// AboutToAdd / AboutToRemove are sent before the part list changes (so list models
    /// can call beginInsertRows / beginRemoveRows), Added / Removed / Modified after.
    /// New parts are always appended at the end.
    enum class Change { AboutToAdd, Added, AboutToRemove, Removed, Modified };
    using Observer = std::function<void(Change, PartId)>;
    using ObserverId = std::size_t;

    /// Adds a part. If part.id is 0 a new id is assigned; otherwise the id is kept
    /// (used by undo to restore a removed part). Returns the part id.
    PartId addPart(Part part);

    /// Removes a part and returns it, or std::nullopt when the id is unknown.
    std::optional<Part> removePart(PartId id);

    [[nodiscard]] const Part* part(PartId id) const;
    [[nodiscard]] const std::vector<Part>& parts() const { return m_parts; }
    [[nodiscard]] std::size_t partCount() const { return m_parts.size(); }

    /// Applies a modification to a part, bumps its revision and notifies observers.
    /// The new revision is always previous + 1, even if `fn` overwrites the part with an
    /// older snapshot.
    /// Returns false when the id is unknown.
    bool modifyPart(PartId id, const std::function<void(Part&)>& fn);

    /// Makes sure ids up to `id` are never assigned to new parts (ids held elsewhere, e.g.
    /// parts of suppressed timeline features in a loaded project).
    void reserveIds(PartId id) { m_nextId = std::max(m_nextId, id + 1); }

    /// Bounding box of all visible parts.
    [[nodiscard]] core::BoundingBox bounds() const;

    /// Document revision: incremented after every change (add, remove, modify). Lets
    /// clients such as an AI agent detect that the document changed since they read it.
    [[nodiscard]] std::uint64_t revision() const { return m_revision; }

    /// Imported source geometry revision. Changes only when a source is added, removed,
    /// or its immutable mesh / shape is replaced; metadata and timeline mirrors do not
    /// invalidate features. Never restored by undo, so clients can detect source changes.
    [[nodiscard]] std::uint64_t importedGeometryRevision() const { return m_importedGeometryRevision; }

    ObserverId addObserver(Observer observer);
    void removeObserver(ObserverId id);

    void clear();

private:
    void notify(Change change, PartId id);
    Part* findPart(PartId id);

    std::vector<Part> m_parts;
    PartId m_nextId = 1;
    std::vector<std::pair<ObserverId, Observer>> m_observers;
    ObserverId m_nextObserverId = 1;
    std::uint64_t m_revision = 0;
    std::uint64_t m_importedGeometryRevision = 0;
};

} // namespace aerocad::model
