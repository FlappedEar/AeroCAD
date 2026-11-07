// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/Align.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace aerocad::core {

Mat3 Mat3::rotation(const Vec3& axis, double angle)
{
    const Vec3 a = axis.normalized();
    if (a == Vec3{})
        return {};
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const double t = 1.0 - c;
    return {{t * a.x * a.x + c, t * a.x * a.y - s * a.z, t * a.x * a.z + s * a.y, //
             t * a.x * a.y + s * a.z, t * a.y * a.y + c, t * a.y * a.z - s * a.x, //
             t * a.x * a.z - s * a.y, t * a.y * a.z + s * a.x, t * a.z * a.z + c}};
}

Mat3 Mat3::fromRows(const Vec3& r0, const Vec3& r1, const Vec3& r2)
{
    return {{r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, r2.x, r2.y, r2.z}};
}

Mat3 Mat3::operator*(const Mat3& o) const
{
    Mat3 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k)
                sum += (*this)(i, k) * o(k, j);
            r.m[static_cast<std::size_t>(i * 3 + j)] = sum;
        }
    }
    return r;
}

Mat3 Mat3::transposed() const
{
    return {{m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]}};
}

double Mat3::determinant() const
{
    return row(0).dot(row(1).cross(row(2)));
}

Affine Affine::after(const Affine& first) const
{
    return {linear * first.linear, linear * first.translation + translation};
}

void transformMesh(TriangleMesh& mesh, const Affine& transform)
{
    std::vector<Vec3> vertices = mesh.vertices();
    for (auto& v : vertices)
        v = transform.apply(v);
    std::vector<TriangleMesh::Triangle> triangles = mesh.triangles();
    if (transform.linear.determinant() < 0.0) {
        for (auto& t : triangles)
            std::swap(t[1], t[2]);
    }
    mesh = TriangleMesh(std::move(vertices), std::move(triangles));
}

namespace {

/// Eigen decomposition of a symmetric 3x3 matrix (cyclic Jacobi). Returns eigenvalues
/// and the matching eigenvectors (columns of v).
void symmetricEigen(double a[3][3], double values[3], double v[3][3])
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            v[i][j] = i == j ? 1.0 : 0.0;
    for (int sweep = 0; sweep < 50; ++sweep) {
        const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        const double scale = a[0][0] * a[0][0] + a[1][1] * a[1][1] + a[2][2] * a[2][2];
        if (off <= scale * 1e-30 || off < 1e-300)
            break;
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::abs(a[p][q]) < 1e-300)
                    continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (int k = 0; k < 3; ++k) {
                    const double akp = a[k][p];
                    const double akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double apk = a[p][k];
                    const double aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double vkp = v[k][p];
                    const double vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
    for (int i = 0; i < 3; ++i)
        values[i] = a[i][i];
}

} // namespace

namespace {

/// Plane from the (weighted) covariance of a point distribution about its centroid.
std::optional<Plane> planeFromCovariance(double cov[3][3], const Vec3& centroid, double weight)
{
    double values[3];
    double vectors[3][3];
    symmetricEigen(cov, values, vectors);
    int smallest = 0;
    int largest = 0;
    for (int i = 1; i < 3; ++i) {
        if (values[i] < values[smallest])
            smallest = i;
        if (values[i] > values[largest])
            largest = i;
    }
    int middle = 3 - smallest - largest;
    if (smallest == largest)
        middle = (smallest + 1) % 3;
    // Collinear (or coincident) points: the second largest spread is ~0.
    if (values[largest] <= 0.0 || values[middle] <= values[largest] * 1e-12)
        return std::nullopt;

    Plane plane;
    plane.origin = centroid;
    plane.normal = Vec3{vectors[0][smallest], vectors[1][smallest], vectors[2][smallest]}.normalized();
    plane.rms = std::sqrt(std::max(0.0, values[smallest]) / weight);
    plane.major = Vec3{vectors[0][largest], vectors[1][largest], vectors[2][largest]}.normalized();
    plane.elongation = 1.0 - std::max(0.0, values[middle]) / values[largest];
    return plane;
}

} // namespace

std::optional<Plane> fitPlane(const std::vector<Vec3>& points)
{
    if (points.size() < 3)
        return std::nullopt;
    Vec3 c;
    for (const auto& p : points)
        c = c + p;
    c = c * (1.0 / static_cast<double>(points.size()));

    double cov[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (const auto& p : points) {
        const double d[3] = {p.x - c.x, p.y - c.y, p.z - c.z};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                cov[i][j] += d[i] * d[j];
    }
    return planeFromCovariance(cov, c, static_cast<double>(points.size()));
}

std::optional<Plane> fitPlaneToTriangles(const TriangleMesh& mesh, const std::vector<std::size_t>& triangles)
{
    // Area-weighted, so the result does not depend on how finely each part is meshed.
    double total = 0.0;
    Vec3 c;
    const auto& v = mesh.vertices();
    for (const std::size_t t : triangles) {
        if (t >= mesh.triangleCount())
            continue;
        const auto& tri = mesh.triangles()[t];
        const double a = 0.5 * (v[tri[1]] - v[tri[0]]).cross(v[tri[2]] - v[tri[0]]).length();
        total += a;
        c = c + (v[tri[0]] + v[tri[1]] + v[tri[2]]) * (a / 3.0);
    }
    if (!(total > 0.0))
        return std::nullopt;
    c = c * (1.0 / total);
    // Exact second moment of each triangle about c: A/12 (sum d_i d_i^T + 9 g g^T),
    // with d_i the corners and g the triangle's centroid relative to c.
    double cov[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (const std::size_t t : triangles) {
        if (t >= mesh.triangleCount())
            continue;
        const auto& tri = mesh.triangles()[t];
        const Vec3 d[3] = {v[tri[0]] - c, v[tri[1]] - c, v[tri[2]] - c};
        const double a = 0.5 * (d[1] - d[0]).cross(d[2] - d[0]).length();
        const Vec3 g = (d[0] + d[1] + d[2]) * (1.0 / 3.0);
        auto add = [&](const Vec3& p, double w) {
            const double q[3] = {p.x, p.y, p.z};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    cov[i][j] += w * q[i] * q[j];
        };
        for (const auto& p : d)
            add(p, a / 12.0);
        add(g, 9.0 * a / 12.0);
    }
    return planeFromCovariance(cov, c, total);
}

std::vector<Vec3> triangleVertices(const TriangleMesh& mesh, const std::vector<std::size_t>& triangles)
{
    std::vector<std::uint32_t> ids;
    ids.reserve(triangles.size() * 3);
    for (const std::size_t t : triangles) {
        if (t < mesh.triangleCount())
            ids.insert(ids.end(), mesh.triangles()[t].begin(), mesh.triangles()[t].end());
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::vector<Vec3> out;
    out.reserve(ids.size());
    for (const auto id : ids)
        out.push_back(mesh.vertices()[id]);
    return out;
}

Mat3 rotationBetween(const Vec3& from, const Vec3& to)
{
    const Vec3 f = from.normalized();
    const Vec3 t = to.normalized();
    const double c = std::clamp(f.dot(t), -1.0, 1.0);
    Vec3 axis = f.cross(t);
    if (axis.length() < 1e-12) {
        if (c > 0.0)
            return {};
        // Opposite: rotate 180 degrees about any axis perpendicular to `from`.
        axis = std::abs(f.x) < 0.9 ? f.cross(Vec3{1, 0, 0}) : f.cross(Vec3{0, 1, 0});
    }
    return Mat3::rotation(axis, std::acos(c));
}

std::optional<Affine> frameToWorld(const Vec3& origin, const Vec3& zAxis, const std::optional<Vec3>& xHint)
{
    const Vec3 z = zAxis.normalized();
    if (z == Vec3{})
        return std::nullopt;
    Vec3 x;
    if (xHint) {
        x = (*xHint - z * xHint->dot(z)).normalized();
    }
    if (x == Vec3{}) {
        // Keep the rotation minimal: X is the world X axis turned with the normal.
        x = rotationBetween(Vec3{0, 0, 1}, z) * Vec3{1, 0, 0};
        x = (x - z * x.dot(z)).normalized();
    }
    const Vec3 y = z.cross(x);
    const Mat3 r = Mat3::fromRows(x, y, z); // maps x->X, y->Y, z->Z
    return Affine{r, (r * origin) * -1.0};
}

} // namespace aerocad::core
