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
    fn(*p);
    p->id = id; // the id is not modifiable
    ++p->revision;
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
    // Copy so observers may (un)register during notification.
    const auto observers = m_observers;
    for (const auto& [_, observer] : observers)
        observer(change, id);
}

} // namespace aerocad::model
