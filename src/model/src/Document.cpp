// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/model/Document.hpp"

#include <algorithm>
#include <utility>

namespace aerocad::model {

PartId Document::addPart(Part part)
{
    if (part.id == 0 || findPart(part.id) != nullptr)
        part.id = m_nextId;
    m_nextId = std::max(m_nextId, part.id + 1);
    const PartId id = part.id;
    notify(Change::AboutToAdd, id);
    m_parts.push_back(std::move(part));
    if (m_parts.back().kind == PartKind::Imported)
        ++m_importedGeometryRevision;
    notify(Change::Added, id);
    return id;
}

std::optional<Part> Document::removePart(PartId id)
{
    auto it = std::find_if(m_parts.begin(), m_parts.end(), [id](const Part& p) { return p.id == id; });
    if (it == m_parts.end())
        return std::nullopt;
    notify(Change::AboutToRemove, id);
    it = std::find_if(m_parts.begin(), m_parts.end(), [id](const Part& p) { return p.id == id; });
    Part removed = std::move(*it);
    m_parts.erase(it);
    if (removed.kind == PartKind::Imported)
        ++m_importedGeometryRevision;
    notify(Change::Removed, id);
    return removed;
}

const Part* Document::part(PartId id) const
{
    auto it = std::find_if(m_parts.begin(), m_parts.end(), [id](const Part& p) { return p.id == id; });
    return it == m_parts.end() ? nullptr : &*it;
}

Part* Document::findPart(PartId id)
{
    return const_cast<Part*>(std::as_const(*this).part(id));
}

bool Document::modifyPart(PartId id, const std::function<void(Part&)>& fn)
{
    Part* p = findPart(id);
    if (p == nullptr)
        return false;
    const std::uint64_t revision = p->revision;
    const PartKind kind = p->kind;
    const auto mesh = p->mesh;
    const auto shape = p->shape;
    fn(*p);
    p->id = id; // the id is not modifiable
    // Derived from the state before the callback, so restoring an older snapshot
    // (undo) still yields a new, never seen revision.
    p->revision = revision + 1;
    if ((kind == PartKind::Imported || p->kind == PartKind::Imported) &&
        (kind != p->kind || mesh != p->mesh ||
         (shape ? shape->impl() : nullptr) != (p->shape ? p->shape->impl() : nullptr)))
        ++m_importedGeometryRevision;
    notify(Change::Modified, id);
    return true;
}

core::BoundingBox Document::bounds() const
{
    core::BoundingBox box;
    for (const auto& p : m_parts) {
        if (!p.visible)
            continue;
        if (p.mesh)
            box.merge(p.mesh->bounds());
        else if (p.shape)
            box.merge(p.shape->bounds());
    }
    return box;
}

Document::ObserverId Document::addObserver(Observer observer)
{
    const ObserverId id = m_nextObserverId++;
    m_observers.emplace_back(id, std::move(observer));
    return id;
}

void Document::removeObserver(ObserverId id)
{
    std::erase_if(m_observers, [id](const auto& entry) { return entry.first == id; });
}

void Document::clear()
{
    while (!m_parts.empty())
        removePart(m_parts.back().id);
}

void Document::notify(Change change, PartId id)
{
    if (change == Change::Added || change == Change::Removed || change == Change::Modified)
        ++m_revision;
    // Copy so observers may (un)register during notification.
    const auto observers = m_observers;
    for (const auto& [_, observer] : observers)
        observer(change, id);
}

} // namespace aerocad::model
