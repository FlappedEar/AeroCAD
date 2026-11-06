// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/model/Part.hpp"

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

    /// Bounding box of all visible parts.
    [[nodiscard]] core::BoundingBox bounds() const;

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
};

} // namespace aerocad::model
