// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Align.hpp"
#include "aerocad/core/Math.hpp"
#include "aerocad/geometry/Shape.hpp"

namespace aerocad::geometry {

/// Returns a copy of the shape uniformly scaled about `origin`.
[[nodiscard]] Shape scaled(const Shape& shape, double factor, const core::Vec3& origin);

/// Returns a copy of the shape translated by `offset`.
[[nodiscard]] Shape translated(const Shape& shape, const core::Vec3& offset);

/// Returns a copy of the shape moved by a rigid transform (rotation + translation).
/// Throws std::invalid_argument if `transform.linear` is not a proper rotation.
[[nodiscard]] Shape transformed(const Shape& shape, const core::Affine& transform);

} // namespace aerocad::geometry
