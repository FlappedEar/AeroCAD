// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Math.hpp"

#include <cstddef>
#include <memory>

namespace aerocad::geometry {

/// Immutable handle to a B-rep shape (Open CASCADE TopoDS_Shape behind a pimpl).
/// Copying is cheap: copies share the same underlying shape.
class Shape {
public:
    struct Impl;

    Shape();
    explicit Shape(std::shared_ptr<const Impl> impl);

    [[nodiscard]] bool isNull() const;
    [[nodiscard]] core::BoundingBox bounds() const;
    [[nodiscard]] std::size_t faceCount() const;
    [[nodiscard]] std::size_t solidCount() const;

    /// Volume in mm^3 (0 for shapes without solids).
    [[nodiscard]] double volume() const;

    [[nodiscard]] const Impl* impl() const { return m_impl.get(); }

private:
    std::shared_ptr<const Impl> m_impl;
};

} // namespace aerocad::geometry
