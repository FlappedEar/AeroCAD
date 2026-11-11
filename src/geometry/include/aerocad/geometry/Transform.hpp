// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Align.hpp"
#include "aerocad/core/Math.hpp"
#include "aerocad/geometry/Shape.hpp"

namespace aerocad::geometry {

/// Smallest and largest accepted uniform scale factor. Outside this range the result is not
/// meaningful in millimetres (and very small factors make the kernel reject the transform).
inline constexpr double kMinScaleFactor = 1e-6;
inline constexpr double kMaxScaleFactor = 1e6;

/// Returns a copy of the shape uniformly scaled about `origin`.
/// Throws std::invalid_argument for a factor outside [kMinScaleFactor, kMaxScaleFactor] or a
/// non-finite origin, and KernelError if the kernel fails.
[[nodiscard]] Shape scaled(const Shape& shape, double factor, const core::Vec3& origin);

/// Returns a copy of the shape scaled by a different factor along each world axis about
/// `origin` (Fusion: Scale, Non-Uniform). Curved faces become B-splines. Same limits and
/// exceptions as scaled().
[[nodiscard]] Shape scaledNonUniform(const Shape& shape, double fx, double fy, double fz, const core::Vec3& origin);

/// Returns a copy of the shape translated by `offset`.
[[nodiscard]] Shape translated(const Shape& shape, const core::Vec3& offset);

/// Returns a copy of the shape moved by a rigid transform (rotation + translation).
/// Throws std::invalid_argument if `transform.linear` is not a proper rotation.
[[nodiscard]] Shape transformed(const Shape& shape, const core::Affine& transform);

} // namespace aerocad::geometry
