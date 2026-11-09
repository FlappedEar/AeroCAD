// SPDX-License-Identifier: GPL-3.0-or-later
// Rigid transforms and helpers to orient scanned meshes (best-fit planes, frames).
#pragma once

#include "aerocad/core/Math.hpp"
#include "aerocad/core/Mesh.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace aerocad::core {

/// 3x3 matrix, row-major.
struct Mat3 {
    std::array<double, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};

    [[nodiscard]] static Mat3 identity() { return {}; }
    /// Rotation by `angle` radians about `axis` (need not be normalised; zero axis -> identity).
    [[nodiscard]] static Mat3 rotation(const Vec3& axis, double angle);
    /// Matrix whose rows are r0, r1, r2.
    [[nodiscard]] static Mat3 fromRows(const Vec3& r0, const Vec3& r1, const Vec3& r2);

    [[nodiscard]] double operator()(int row, int col) const { return m[static_cast<std::size_t>(row * 3 + col)]; }
    [[nodiscard]] Vec3 row(int r) const { return {(*this)(r, 0), (*this)(r, 1), (*this)(r, 2)}; }
    [[nodiscard]] Vec3 operator*(const Vec3& v) const { return {row(0).dot(v), row(1).dot(v), row(2).dot(v)}; }
    [[nodiscard]] Mat3 operator*(const Mat3& o) const;
    [[nodiscard]] Mat3 transposed() const;
    [[nodiscard]] double determinant() const;
};

/// p' = linear * p + translation. Used for rigid motions and uniform scaling.
struct Affine {
    Mat3 linear;
    Vec3 translation;

    [[nodiscard]] Vec3 apply(const Vec3& p) const { return linear * p + translation; }
    /// (*this) after `first`: result.apply(p) == apply(first.apply(p)).
    [[nodiscard]] Affine after(const Affine& first) const;
    [[nodiscard]] static Affine translationBy(const Vec3& t) { return {Mat3{}, t}; }
};

/// Applies the transform to all vertices. If the transform mirrors (negative
/// determinant) the triangle winding is reversed so normals keep pointing outwards.
void transformMesh(TriangleMesh& mesh, const Affine& transform);

/// Best-fit plane (least squares, through the centroid).
struct Plane {
    Vec3 origin; ///< Centroid of the points.
    Vec3 normal; ///< Unit normal.
    /// Root mean square distance of the points from the plane (fit quality, mm).
    double rms = 0.0;
    /// Unit direction of the largest spread of the points, in the plane (the long side
    /// of a patch), e.g. the X axis of a sketch drawn on it.
    Vec3 major{1, 0, 0};
    /// How clearly `major` stands out: 1 - (second largest spread / largest spread). Near
    /// 0 for round or square patches, where the major direction is arbitrary.
    double elongation = 0.0;
};

/// Fits a plane to at least three non-collinear points. Returns std::nullopt otherwise.
[[nodiscard]] std::optional<Plane> fitPlane(const std::vector<Vec3>& points);

/// Area-weighted best-fit plane of the given triangles (a surface patch, independent of
/// how finely it is meshed). rms is the area-weighted RMS distance. std::nullopt when
/// the triangles have no area or lie along a line.
[[nodiscard]] std::optional<Plane> fitPlaneToTriangles(const TriangleMesh& mesh,
                                                       const std::vector<std::size_t>& triangles);

/// Vertices of the given triangles (each vertex once), e.g. for fitting a plane to a
/// selected region. Out-of-range triangle indices are ignored.
[[nodiscard]] std::vector<Vec3> triangleVertices(const TriangleMesh& mesh, const std::vector<std::size_t>& triangles);

/// Rigid transform that maps a local frame to the world axes: `origin` goes to (0,0,0),
/// `zAxis` to +Z and (the part of) `xHint` (orthogonal to zAxis) to +X. Without a usable
/// hint the X axis is chosen automatically. Returns std::nullopt for a zero zAxis.
[[nodiscard]] std::optional<Affine> frameToWorld(const Vec3& origin, const Vec3& zAxis,
                                                 const std::optional<Vec3>& xHint = std::nullopt);

/// Rotation (about the origin) that turns unit vector `from` onto unit vector `to`.
[[nodiscard]] Mat3 rotationBetween(const Vec3& from, const Vec3& to);

/// Axis (unit) and angle (radians, 0..pi) of a rotation matrix. Identity gives angle 0
/// and axis +Z.
struct AxisAngle {
    Vec3 axis{0, 0, 1};
    double angle = 0.0;
};
[[nodiscard]] AxisAngle axisAngle(const Mat3& rotation);

/// Normal of the surface of `mesh` around `point` on triangle `triangle`: the normals of
/// that triangle and of the triangles reaching within `radius` of the point, each weighted
/// by about the part of it inside that disc and by how closely it faces the same way
/// (cos^8, so a bevel or the next face of an edge hardly counts, scan noise does). On a
/// noisy scan this is the normal of the patch, not of one triangle. Zero vector for an
/// invalid triangle.
[[nodiscard]] Vec3 patchNormal(const TriangleMesh& mesh, const Vec3& point, std::size_t triangle, double radius);

/// How to turn a part by one of its faces (world normal `normal`):
enum class FaceTarget {
    Down,  ///< the face points down (-Z), so the part can lie on it;
    Front, ///< about Z only, so the face looks at the Front view (-Y); a face pointing
           ///< nearly straight up or down is turned onto -Y directly.
};
/// Rotation (about the origin) for `target`.
[[nodiscard]] Mat3 faceTurn(const Vec3& normal, FaceTarget target);

} // namespace aerocad::core
