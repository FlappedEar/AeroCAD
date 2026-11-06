// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cad/Variants.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace aerocad::cad {

VariantSet::VariantSet()
{
    m_variants.push_back({1, "A", {}});
}

const Variant* VariantSet::find(VariantId id) const
{
    auto it = std::find_if(m_variants.begin(), m_variants.end(), [id](const Variant& v) { return v.id == id; });
    return it == m_variants.end() ? nullptr : &*it;
}

const Variant* VariantSet::findByName(std::string_view name) const
{
    auto it = std::find_if(m_variants.begin(), m_variants.end(), [name](const Variant& v) { return v.name == name; });
    return it == m_variants.end() ? nullptr : &*it;
}

Variant& VariantSet::require(VariantId id)
{
    auto it = std::find_if(m_variants.begin(), m_variants.end(), [id](const Variant& v) { return v.id == id; });
    if (it == m_variants.end())
        throw DesignError("No variant with id " + std::to_string(id));
    return *it;
}

std::string VariantSet::nextName() const
{
    // A..Z, then A2..Z2, ...
    for (int round = 1;; ++round) {
        for (char c = 'A'; c <= 'Z'; ++c) {
            std::string name(1, c);
            if (round > 1)
                name += std::to_string(round);
            if (findByName(name) == nullptr)
                return name;
        }
    }
}

VariantId VariantSet::add(std::string name, Design::State state)
{
    if (name.empty())
        name = nextName();
    if (findByName(name) != nullptr)
        throw DesignError("A variant named '" + name + "' already exists");
    const VariantId id = m_nextId++;
    m_variants.push_back({id, std::move(name), std::move(state)});
    return id;
}

Design::State VariantSet::activate(VariantId id, Design::State current)
{
    Variant& target = require(id);
    if (id == m_active)
        return current;
    // Feature ids stay unique across variants: features created independently in two
    // variants must not share an id (part bookkeeping and variant.compare key on it).
    FeatureId nextId = current.nextId;
    for (const auto& v : m_variants)
        nextId = std::max(nextId, v.state.nextId);
    require(m_active).state = std::move(current);
    m_previous = m_active;
    m_active = id;
    target.state.nextId = nextId;
    return target.state;
}

Design::State VariantSet::stateOf(VariantId id, const Design::State& current) const
{
    if (id == m_active)
        return current;
    const Variant* v = find(id);
    if (v == nullptr)
        throw DesignError("No variant with id " + std::to_string(id));
    return v->state;
}

void VariantSet::rename(VariantId id, std::string name)
{
    if (name.empty())
        throw DesignError("Variant name must not be empty");
    const Variant* other = findByName(name);
    if (other != nullptr && other->id != id)
        throw DesignError("A variant named '" + name + "' already exists");
    require(id).name = std::move(name);
}

void VariantSet::remove(VariantId id)
{
    require(id);
    if (id == m_active)
        throw DesignError("Cannot delete the active variant; activate another one first");
    std::erase_if(m_variants, [id](const Variant& v) { return v.id == id; });
    if (m_previous == id)
        m_previous = 0;
}

void VariantSet::assign(std::vector<Variant> variants, VariantId active, VariantId previous)
{
    std::set<VariantId> ids;
    std::set<std::string> names;
    VariantId maxId = 0;
    for (const auto& v : variants) {
        if (v.id == 0 || !ids.insert(v.id).second)
            throw DesignError("Invalid or duplicate variant id " + std::to_string(v.id));
        if (v.name.empty() || !names.insert(v.name).second)
            throw DesignError("Invalid or duplicate variant name '" + v.name + "'");
        maxId = std::max(maxId, v.id);
    }
    if (!ids.contains(active))
        throw DesignError("Active variant " + std::to_string(active) + " does not exist");
    m_variants = std::move(variants);
    m_active = active;
    m_previous = ids.contains(previous) && previous != active ? previous : 0;
    m_nextId = maxId + 1;
}

} // namespace aerocad::cad
