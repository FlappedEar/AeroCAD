// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/Align.hpp"
#include "aerocad/core/Measure.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
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

std::optional<Affine> affineBetween(const TriangleMesh& from, const TriangleMesh& to)
{
    const auto& a = from.vertices();
    const auto& b = to.vertices();
    if (a.empty() || a.size() != b.size() || from.triangleCount() != to.triangleCount())
        return std::nullopt;

    // Winding: unchanged, or every triangle reversed the way transformMesh does it.
    bool mirrored = false;
    if (from.triangles() != to.triangles()) {
        const auto& ta = from.triangles();
        const auto& tb = to.triangles();
        for (std::size_t i = 0; i < ta.size(); ++i) {
            if (ta[i][0] != tb[i][0] || ta[i][1] != tb[i][2] || ta[i][2] != tb[i][1])
                return std::nullopt;
        }
        mirrored = true;
    }

    // Four well spread source points (searched on a sample, enough for a stable solve).
    const std::size_t step = std::max<std::size_t>(1, a.size() / 4096);
    const std::size_t i0 = 0;
    std::size_t i1 = i0;
    std::size_t i2 = i0;
    std::size_t i3 = i0;
    double best = 0.0;
    for (std::size_t i = 0; i < a.size(); i += step) {
        const double d = (a[i] - a[i0]).dot(a[i] - a[i0]);
        if (d > best) {
            best = d;
            i1 = i;
        }
    }
    if (best <= 0.0)
        return std::nullopt;
    const Vec3 e1 = a[i1] - a[i0];
    best = 0.0;
    for (std::size_t i = 0; i < a.size(); i += step) {
        const Vec3 c = (a[i] - a[i0]).cross(e1);
        const double d = c.dot(c);
        if (d > best) {
            best = d;
            i2 = i;
        }
    }
    // Relative to |e1|^4: points this close to the line give no usable second direction.
    if (best <= 1e-18 * e1.dot(e1) * e1.dot(e1))
        return std::nullopt;
    const Vec3 e2 = a[i2] - a[i0];
    const Vec3 n = e1.cross(e2);
    best = 0.0;
    for (std::size_t i = 0; i < a.size(); i += step) {
        const double d = std::abs((a[i] - a[i0]).dot(n));
        if (d > best) {
            best = d;
            i3 = i;
        }
    }

    const Vec3 f1 = b[i1] - b[i0];
    const Vec3 f2 = b[i2] - b[i0];
    Vec3 e3;
    Vec3 f3;
    if (best > 1e-9 * n.length() * e1.length()) {
        e3 = a[i3] - a[i0];
        f3 = b[i3] - b[i0];
    } else {
        // Flat mesh: map the plane normal like a similarity would.
        const double s = f1.length() / e1.length();
        if (!(s > 0.0) || !std::isfinite(s))
            return std::nullopt;
        e3 = n;
        f3 = f1.cross(f2) * (1.0 / s);
        if (mirrored)
            f3 = f3 * -1.0;
    }

    // L = F * E^-1, with E and F the matrices whose columns are e1..e3 and f1..f3.
    const double det = e1.dot(e2.cross(e3));
    if (det == 0.0 || !std::isfinite(det))
        return std::nullopt;
    const Vec3 r1 = e2.cross(e3) * (1.0 / det);
    const Vec3 r2 = e3.cross(e1) * (1.0 / det);
    const Vec3 r3 = e1.cross(e2) * (1.0 / det);
    Mat3 linear;
    const Vec3 fs[3] = {f1, f2, f3};
    const Vec3 rs[3] = {r1, r2, r3};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) {
                const double fi = i == 0 ? fs[k].x : i == 1 ? fs[k].y : fs[k].z;
                const double rj = j == 0 ? rs[k].x : j == 1 ? rs[k].y : rs[k].z;
                sum += fi * rj;
            }
            linear.m[static_cast<std::size_t>(i * 3 + j)] = sum;
        }
    }
    if ((linear.determinant() < 0.0) != mirrored)
        return std::nullopt;
    const Affine tf{linear, b[i0] - linear * a[i0]};

    // Every vertex must follow, within rounding relative to the size of the result.
    const BoundingBox box = to.bounds();
    const double magnitude = std::max({std::abs(box.min.x), std::abs(box.min.y), std::abs(box.min.z),
                                       std::abs(box.max.x), std::abs(box.max.y), std::abs(box.max.z)});
    const double tol = 1e-9 * (magnitude + (box.max - box.min).length()) + 1e-300;
    const double tol2 = tol * tol;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const Vec3 d = tf.apply(a[i]) - b[i];
        if (!(d.dot(d) <= tol2))
            return std::nullopt;
    }
    return tf;
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

AxisAngle axisAngle(const Mat3& r)
{
    const double c = std::clamp(((r(0, 0) + r(1, 1) + r(2, 2)) - 1.0) / 2.0, -1.0, 1.0);
    const double angle = std::acos(c);
    if (angle < 1e-12)
        return {};
    Vec3 axis{r(2, 1) - r(1, 2), r(0, 2) - r(2, 0), r(1, 0) - r(0, 1)};
    if (angle > std::numbers::pi - 1e-6 || axis.length() < 1e-9) {
        // Near half a turn: R = 2 a a^T - I, take the largest diagonal entry.
        const double xx = std::max(0.0, (r(0, 0) + 1.0) / 2.0);
        const double yy = std::max(0.0, (r(1, 1) + 1.0) / 2.0);
        const double zz = std::max(0.0, (r(2, 2) + 1.0) / 2.0);
        if (xx >= yy && xx >= zz) {
            const double x = std::sqrt(xx);
            axis = {x, (r(0, 1) + r(1, 0)) / (4.0 * x), (r(0, 2) + r(2, 0)) / (4.0 * x)};
        } else if (yy >= zz) {
            const double y = std::sqrt(yy);
            axis = {(r(0, 1) + r(1, 0)) / (4.0 * y), y, (r(1, 2) + r(2, 1)) / (4.0 * y)};
        } else {
            const double z = std::sqrt(zz);
            axis = {(r(0, 2) + r(2, 0)) / (4.0 * z), (r(1, 2) + r(2, 1)) / (4.0 * z), z};
        }
    }
    return {axis.normalized(), angle};
}

Vec3 patchNormal(const TriangleMesh& mesh, const Vec3& point, std::size_t triangle, double radius)
{
    if (triangle >= mesh.triangleCount())
        return {};
    const Vec3 hit = mesh.faceNormal(triangle);
    const double reach = 2.0 * radius;
    const double disc = std::numbers::pi * radius * radius;
    const auto& v = mesh.vertices();
    Vec3 sum{};
    for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
        const auto& tri = mesh.triangles()[t];
        const Vec3 a = v[tri[0]], b = v[tri[1]], c = v[tri[2]];
        const Vec3 n2 = (b - a).cross(c - a); // 2 * area * normal
        const double area = n2.length() / 2.0;
        if (!(area > 0.0))
            continue;
        const Vec3 n = n2.normalized();
        const double facing = n.dot(hit);
        if (facing <= 0.0)
            continue;
        if (t != triangle && (closestPointOnTriangle(point, a, b, c) - point).length() > radius)
            continue;
        // Roughly the part of the triangle inside the sampling disc: a long or tall
        // triangle only reaches in with a strip of its size.
        const double longest = std::max({(b - a).length(), (c - b).length(), (a - c).length()});
        const double height = 2.0 * area / longest;
        const double inside =
            std::min(disc, area * std::min(1.0, reach / longest) * std::min(1.0, reach / height));
        // Surfaces turned away from the clicked one (a bevel, the next face of an edge)
        // count far less than scan noise around it (cos^8: 15 deg -> 0.76, 45 deg -> 0.06).
        sum = sum + n * (inside * std::pow(facing, 8));
    }
    return sum.length() > 0.0 ? sum.normalized() : hit;
}

Mat3 faceTurn(const Vec3& normal, FaceTarget target)
{
    const Vec3 n = normal.normalized();
    if (target == FaceTarget::Down)
        return rotationBetween(n, Vec3{0, 0, -1});
    if (std::hypot(n.x, n.y) > 0.2)
        return Mat3::rotation(Vec3{0, 0, 1}, std::atan2(-n.x, -n.y));
    return rotationBetween(n, Vec3{0, -1, 0});
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
