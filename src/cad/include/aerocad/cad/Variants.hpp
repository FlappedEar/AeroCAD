// SPDX-License-Identifier: GPL-3.0-or-later
// Design variants (A/B comparison): alternative versions of the design timeline in one
// project. Exactly one variant is active; its state lives in the cad::Design, the others
// are stored here. Imported reference parts are shared by all variants.
#pragma once

#include "aerocad/cad/Design.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aerocad::cad {

using VariantId = std::uint64_t;

struct Variant {
    VariantId id = 0;
    std::string name;
    /// Design state of an inactive variant. Stale for the active variant (see Design).
    Design::State state;
};

class VariantSet {
public:
    /// One variant "A" (id 1), active.
    VariantSet();

    [[nodiscard]] const std::vector<Variant>& variants() const { return m_variants; }
    [[nodiscard]] VariantId active() const { return m_active; }
    /// Variant that was active before the current one (0 if none): the A/B toggle target.
    [[nodiscard]] VariantId previous() const { return m_previous; }
    [[nodiscard]] const Variant* find(VariantId id) const;
    [[nodiscard]] const Variant* findByName(std::string_view name) const;
    [[nodiscard]] std::size_t size() const { return m_variants.size(); }

    /// Adds a variant holding `state`. An empty name gets the next free letter (B, C, ...).
    /// Throws DesignError when the name is taken.
    VariantId add(std::string name, Design::State state);

    /// Makes `id` active: stores `current` (the live design state) in the active variant
    /// and returns the state of `id` for Design::restore(). Throws DesignError for unknown ids.
    Design::State activate(VariantId id, Design::State current);

    /// Stored state of a variant; for the active one, `current` is returned.
    [[nodiscard]] Design::State stateOf(VariantId id, const Design::State& current) const;

    void rename(VariantId id, std::string name);
    /// Removes an inactive variant. Throws DesignError for the active or an unknown one.
    void remove(VariantId id);

    /// Name the next add() without a name would use.
    [[nodiscard]] std::string nextName() const;

    /// Rebuilds the set (project load). Throws DesignError if `active` is not in `variants`,
    /// ids are not unique or a name is empty / duplicated.
    void assign(std::vector<Variant> variants, VariantId active, VariantId previous);

private:
    Variant& require(VariantId id);

    std::vector<Variant> m_variants;
    VariantId m_active = 1;
    VariantId m_previous = 0;
    VariantId m_nextId = 2;
};

} // namespace aerocad::cad
