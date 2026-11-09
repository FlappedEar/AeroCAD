// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/SurfaceFit.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRep_Tool.hxx>
#include <Extrema_GenLocateExtPS.hxx>
#include <Extrema_POnSurf.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <GeomAPI_PointsToBSplineSurface.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomAdaptor_Surface.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Poly_Triangulation.hxx>
#include <ShapeFix_Face.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt2d.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopAbs_State.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pnt2d.hxx>
#include <math_Jacobi.hxx>
#include <math_Matrix.hxx>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace aerocad::geometry {

namespace {

using core::Vec3;

struct PositionHash {
    std::size_t operator()(const Vec3& v) const noexcept
    {
        const std::hash<double> h;
        std::size_t seed = h(v.x);
        seed ^= h(v.y) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        seed ^= h(v.z) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        return seed;
    }
};

/// Local frame of the best-fit plane: w is the height along `z`.
struct Frame {
    Vec3 origin, x, y, z;
    [[nodiscard]] std::array<double, 3> toLocal(const Vec3& p) const
    {
        const Vec3 d = p - origin;
        return {d.dot(x), d.dot(y), d.dot(z)};
    }
    [[nodiscard]] Vec3 toWorld(double u, double v, double w) const { return origin + x * u + y * v + z * w; }
};

/// PCA plane of the points; z is oriented along `preferredNormal`.
Frame pcaFrame(const std::vector<Vec3>& pts, const Vec3& preferredNormal)
{
    Vec3 c;
    for (const auto& p : pts)
        c = c + p;
    c = c * (1.0 / static_cast<double>(pts.size()));
    math_Matrix cov(1, 3, 1, 3, 0.0);
    for (const auto& p : pts) {
        const Vec3 d = p - c;
        const double a[3] = {d.x, d.y, d.z};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                cov(i + 1, j + 1) += a[i] * a[j];
    }
    math_Jacobi jac(cov);
    int imin = 1, imax = 1;
    for (int i = 2; i <= 3; ++i) {
        if (jac.Value(i) < jac.Value(imin))
            imin = i;
        if (jac.Value(i) > jac.Value(imax))
            imax = i;
    }
    if (imin == imax)
        imax = imin == 1 ? 2 : 1;
    math_Vector vn(1, 3), vx(1, 3);
    jac.Vector(imin, vn);
    jac.Vector(imax, vx);
    Frame f;
    f.origin = c;
    f.z = Vec3{vn(1), vn(2), vn(3)}.normalized();
    if (f.z.dot(preferredNormal) < 0.0)
        f.z = f.z * -1.0;
    f.x = Vec3{vx(1), vx(2), vx(3)}.normalized();
    f.y = f.z.cross(f.x).normalized();
    f.x = f.y.cross(f.z);
    return f;
}

/// Uniform bucket grid over (u, v) for neighbour queries.
class Buckets {
public:
    Buckets(const std::vector<std::array<double, 3>>& uvw, double u0, double u1, double v0, double v1, int n)
        : m_uvw(uvw), m_u0(u0), m_v0(v0), m_n(n), m_cellU((u1 - u0) / n), m_cellV((v1 - v0) / n),
          m_cells(static_cast<std::size_t>(n) * static_cast<std::size_t>(n))
    {
        for (std::size_t i = 0; i < uvw.size(); ++i)
            m_cells[cell(iu(uvw[i][0]), iv(uvw[i][1]))].push_back(static_cast<std::uint32_t>(i));
    }

    [[nodiscard]] int iu(double u) const { return std::clamp(static_cast<int>((u - m_u0) / m_cellU), 0, m_n - 1); }
    [[nodiscard]] int iv(double v) const { return std::clamp(static_cast<int>((v - m_v0) / m_cellV), 0, m_n - 1); }

    /// Calls fn(index) for points in buckets overlapping the square [u-r, u+r] x [v-r, v+r].
    template <typename Fn> void visit(double u, double v, double r, Fn&& fn) const
    {
        for (int j = iv(v - r); j <= iv(v + r); ++j)
            for (int i = iu(u - r); i <= iu(u + r); ++i)
                for (const auto k : m_cells[cell(i, j)])
                    fn(k);
    }

    /// Exact nearest point in 3D (local coordinates): rings of buckets outward until no
    /// unvisited bucket can hold a closer point. Index of the nearest point; `best` receives its distance.
    std::uint32_t nearest(double u, double v, double w, double& best) const
    {
        best = std::numeric_limits<double>::max();
        std::uint32_t bestIndex = 0;
        const int cu = iu(u), cv = iv(v);
        const double ring = std::min(m_cellU, m_cellV);
        for (int r = 0; r < m_n; ++r) {
            for (int j = cv - r; j <= cv + r; ++j) {
                for (int i = cu - r; i <= cu + r; ++i) {
                    if (std::max(std::abs(j - cv), std::abs(i - cu)) != r || i < 0 || j < 0 || i >= m_n || j >= m_n)
                        continue;
                    for (const auto k : m_cells[cell(i, j)]) {
                        const auto& q = m_uvw[k];
                        const double d =
                            std::sqrt((q[0] - u) * (q[0] - u) + (q[1] - v) * (q[1] - v) + (q[2] - w) * (q[2] - w));
                        if (d < best) {
                            best = d;
                            bestIndex = k;
                        }
                    }
                }
            }
            if (best <= r * ring)
                break;
        }
        return bestIndex;
    }

private:
    [[nodiscard]] std::size_t cell(int i, int j) const
    {
        return static_cast<std::size_t>(j) * static_cast<std::size_t>(m_n) + static_cast<std::size_t>(i);
    }

    const std::vector<std::array<double, 3>>& m_uvw;
    double m_u0, m_v0;
    int m_n;
    double m_cellU, m_cellV;
    std::vector<std::vector<std::uint32_t>> m_cells;
};

/// Solves the leading n x n block of m x = rhs (n <= 6) by Gaussian elimination with partial pivoting; std::nullopt
/// when the system is (nearly) singular, e.g. points on a line.
std::optional<std::array<double, 6>> solveSymmetric6(std::array<std::array<double, 6>, 6> m, std::array<double, 6> rhs,
                                                     std::size_t n = 6)
{
    double scale = 0.0;
    for (const auto& row : m)
        for (const double x : row)
            scale = std::max(scale, std::abs(x));
    for (std::size_t c = 0; c < n; ++c) {
        std::size_t pivot = c;
        for (std::size_t r = c + 1; r < n; ++r)
            if (std::abs(m[r][c]) > std::abs(m[pivot][c]))
                pivot = r;
        if (std::abs(m[pivot][c]) <= 1e-10 * scale)
            return std::nullopt;
        std::swap(m[c], m[pivot]);
        std::swap(rhs[c], rhs[pivot]);
        for (std::size_t r = c + 1; r < n; ++r) {
            const double f = m[r][c] / m[c][c];
            for (std::size_t k = c; k < n; ++k)
                m[r][k] -= f * m[c][k];
            rhs[r] -= f * rhs[c];
        }
    }
    std::array<double, 6> x{};
    for (std::size_t c = n; c-- > 0;) {
        double sum = rhs[c];
        for (std::size_t k = c + 1; k < n; ++k)
            sum -= m[c][k] * x[k];
        x[c] = sum / m[c][c];
    }
    return x;
}

/// Closed outlines of the selection (border edges chained into loops), longest first.
std::vector<std::vector<Vec3>> selectionOutlines(const core::TriangleMesh& mesh, const core::MeshAdjacency& adjacency,
                                                 const core::TriangleSet& selection)
{
    std::vector<char> selected(mesh.triangleCount(), 0);
    for (const auto t : selection)
        selected[t] = 1;

    std::unordered_map<Vec3, std::uint32_t, PositionHash> ids;
    std::vector<Vec3> positions;
    auto idOf = [&](const Vec3& p) {
        auto [it, inserted] = ids.try_emplace(p, static_cast<std::uint32_t>(positions.size()));
        if (inserted)
            positions.push_back(p);
        return it->second;
    };
    std::vector<std::array<std::uint32_t, 2>> edges;
    for (const auto t : selection) {
        const auto& tri = mesh.triangles()[t];
        for (std::size_t s = 0; s < 3; ++s) {
            const auto nb = adjacency.neighbours(t)[s];
            if (nb != core::MeshAdjacency::none && selected[nb] != 0)
                continue;
            const auto a = idOf(mesh.vertices()[tri[s]]);
            const auto b = idOf(mesh.vertices()[tri[(s + 1) % 3]]);
            if (a != b)
                edges.push_back({a, b});
        }
    }

    std::vector<std::vector<std::uint32_t>> edgesAt(positions.size());
    for (std::uint32_t e = 0; e < edges.size(); ++e) {
        edgesAt[edges[e][0]].push_back(e);
        edgesAt[edges[e][1]].push_back(e);
    }
    std::vector<char> used(edges.size(), 0);
    std::vector<std::vector<Vec3>> loops;
    for (std::uint32_t start = 0; start < edges.size(); ++start) {
        if (used[start] != 0)
            continue;
        used[start] = 1;
        std::vector<Vec3> loop{positions[edges[start][0]]};
        const std::uint32_t first = edges[start][0];
        std::uint32_t current = edges[start][1];
        while (current != first) {
            loop.push_back(positions[current]);
            std::uint32_t next = UINT32_MAX;
            for (const auto e : edgesAt[current]) {
                if (used[e] == 0) {
                    next = e;
                    break;
                }
            }
            if (next == UINT32_MAX)
                break; // open chain (non-manifold border); keep what we have
            used[next] = 1;
            current = edges[next][0] == current ? edges[next][1] : edges[next][0];
        }
        if (loop.size() >= 3)
            loops.push_back(std::move(loop));
    }
    auto perimeter = [](const std::vector<Vec3>& l) {
        double len = 0.0;
        for (std::size_t i = 0; i < l.size(); ++i)
            len += (l[(i + 1) % l.size()] - l[i]).length();
        return len;
    };
    std::sort(loops.begin(), loops.end(), [&](const auto& a, const auto& b) { return perimeter(a) > perimeter(b); });
    return loops;
}

/// Resamples a closed loop to `count` points evenly spaced along its length.
std::vector<Vec3> resampleLoop(const std::vector<Vec3>& loop, std::size_t count)
{
    std::vector<double> acc{0.0};
    for (std::size_t i = 0; i < loop.size(); ++i)
        acc.push_back(acc.back() + (loop[(i + 1) % loop.size()] - loop[i]).length());
    const double total = acc.back();
    std::vector<Vec3> out;
    std::size_t seg = 0;
    for (std::size_t k = 0; k < count; ++k) {
        const double s = total * static_cast<double>(k) / static_cast<double>(count);
        while (seg + 1 < acc.size() - 1 && acc[seg + 1] < s)
            ++seg;
        const double len = acc[seg + 1] - acc[seg];
        const double t = len > 0.0 ? (s - acc[seg]) / len : 0.0;
        out.push_back(loop[seg] + (loop[(seg + 1) % loop.size()] - loop[seg]) * t);
    }
    return out;
}

/// Edge-connected patches of a sorted selection, largest first, each sorted.
std::vector<core::TriangleSet> selectionComponents(const core::MeshAdjacency& adjacency,
                                                   const core::TriangleSet& selection)
{
    std::unordered_map<std::uint32_t, char> visited;
    visited.reserve(selection.size());
    for (const auto t : selection)
        visited.emplace(t, 0);
    std::vector<core::TriangleSet> parts;
    std::vector<std::uint32_t> stack;
    for (const auto start : selection) {
        if (visited[start] != 0)
            continue;
        parts.emplace_back();
        visited[start] = 1;
        stack.push_back(start);
        while (!stack.empty()) {
            const auto t = stack.back();
            stack.pop_back();
            parts.back().push_back(t);
            for (const auto nb : adjacency.neighbours(t)) {
                const auto it = visited.find(nb);
                if (it != visited.end() && it->second == 0) {
                    it->second = 1;
                    stack.push_back(nb);
                }
            }
        }
        core::normalizeSelection(parts.back());
    }
    std::sort(parts.begin(), parts.end(), [](const auto& x, const auto& y) { return x.size() > y.size(); });
    return parts;
}

/// Fills the unknown values of an nu x nv grid smoothly from the known ones: harmonic
/// relaxation, then biharmonic (thin plate) so slopes continue across the border of the data.
void fillUnknown(std::vector<double>& values, const std::vector<char>& known, int nu, int nv)
{
    auto at = [nu](int i, int j) {
        return static_cast<std::size_t>(j) * static_cast<std::size_t>(nu) + static_cast<std::size_t>(i);
    };
    auto value = [&](int i, int j) { return values[at(std::clamp(i, 0, nu - 1), std::clamp(j, 0, nv - 1))]; };
    for (int iter = 0; iter < 2000; ++iter) {
        const bool biharmonic = iter >= 500;
        for (int j = 0; j < nv; ++j) {
            for (int i = 0; i < nu; ++i) {
                if (known[at(i, j)] != 0)
                    continue;
                const double n4 = value(i - 1, j) + value(i + 1, j) + value(i, j - 1) + value(i, j + 1);
                if (!biharmonic || i < 2 || j < 2 || i >= nu - 2 || j >= nv - 2) {
                    values[at(i, j)] = 0.25 * n4;
                    continue;
                }
                const double diag =
                    value(i - 1, j - 1) + value(i + 1, j - 1) + value(i - 1, j + 1) + value(i + 1, j + 1);
                const double far = value(i - 2, j) + value(i + 2, j) + value(i, j - 2) + value(i, j + 2);
                values[at(i, j)] = (8.0 * n4 - 2.0 * diag - far) / 20.0;
            }
        }
    }
}

/// A selected patch flattened into the plane by a least squares conformal map, in mm, on its
/// principal axes and inside [0, length] x [0, width]. The border is free, so no corners
/// have to be guessed. Works for any amount of wrap, as long as the patch is a topological
/// disc (holes allowed).
struct Unfolded {
    std::vector<Vec3> positions;
    std::vector<std::array<double, 2>> uv;
    std::vector<std::array<std::uint32_t, 3>> triangles; ///< Local vertex ids.
    std::vector<std::uint32_t> outer;                    ///< Outer border loop (local ids).
    std::vector<std::vector<std::uint32_t>> holes;       ///< Inner border loops (local ids).
    double length = 0.0;
    double width = 0.0;
};

/// Border loops of a set of triangles given by local vertex ids, longest (by length) first.
std::vector<std::vector<std::uint32_t>> borderLoops(const std::vector<std::array<std::uint32_t, 3>>& tris,
                                                    const std::vector<Vec3>& pos)
{
    std::unordered_map<std::uint64_t, int> count;
    auto key = [](std::uint32_t a, std::uint32_t b) {
        return (static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
    };
    for (const auto& t : tris)
        for (int s = 0; s < 3; ++s)
            ++count[key(t[s], t[(s + 1) % 3])];
    // Directed border edges keep the triangles' orientation, so loops are consistently oriented.
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> next;
    for (const auto& t : tris)
        for (int s = 0; s < 3; ++s)
            if (count[key(t[s], t[(s + 1) % 3])] == 1)
                next[t[s]].push_back(t[(s + 1) % 3]);
    std::vector<std::vector<std::uint32_t>> loops;
    while (!next.empty()) {
        auto it = next.begin();
        const std::uint32_t first = it->first;
        std::vector<std::uint32_t> loop{first};
        std::uint32_t current = first;
        while (true) {
            auto n = next.find(current);
            if (n == next.end() || n->second.empty())
                break;
            const std::uint32_t to = n->second.back();
            n->second.pop_back();
            if (n->second.empty())
                next.erase(n);
            if (to == first)
                break;
            loop.push_back(to);
            current = to;
        }
        if (loop.size() >= 3)
            loops.push_back(std::move(loop));
    }
    auto len = [&](const std::vector<std::uint32_t>& l) {
        double s = 0.0;
        for (std::size_t i = 0; i < l.size(); ++i)
            s += (pos[l[(i + 1) % l.size()]] - pos[l[i]]).length();
        return s;
    };
    std::sort(loops.begin(), loops.end(), [&](const auto& a, const auto& b) { return len(a) > len(b); });
    return loops;
}

/// Flips triangles so that neighbours run along their shared edge in opposite
/// directions (STL files are often wound inconsistently). Throws for a non-orientable
/// patch. Each edge-connected part keeps the winding of its first triangle.
void orientConsistently(std::vector<std::array<std::uint32_t, 3>>& tris)
{
    auto key = [](std::uint32_t a, std::uint32_t b) {
        return (static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
    };
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> byEdge;
    for (std::uint32_t t = 0; t < tris.size(); ++t)
        for (int s = 0; s < 3; ++s)
            byEdge[key(tris[t][static_cast<std::size_t>(s)], tris[t][static_cast<std::size_t>((s + 1) % 3)])].push_back(
                t);
    // True when triangle t runs from a to b.
    auto runs = [&](std::uint32_t t, std::uint32_t a, std::uint32_t b) {
        for (int s = 0; s < 3; ++s)
            if (tris[t][static_cast<std::size_t>(s)] == a && tris[t][static_cast<std::size_t>((s + 1) % 3)] == b)
                return true;
        return false;
    };
    std::vector<char> done(tris.size(), 0);
    std::vector<std::uint32_t> stack;
    for (std::uint32_t start = 0; start < tris.size(); ++start) {
        if (done[start] != 0)
            continue;
        done[start] = 1;
        stack.push_back(start);
        while (!stack.empty()) {
            const auto t = stack.back();
            stack.pop_back();
            for (int s = 0; s < 3; ++s) {
                const auto a = tris[t][static_cast<std::size_t>(s)], b = tris[t][static_cast<std::size_t>((s + 1) % 3)];
                const auto& sharing = byEdge[key(a, b)];
                if (sharing.size() != 2)
                    continue; // border or non-manifold (reported by sheetLoops)
                for (const auto nb : sharing) {
                    if (nb == t)
                        continue;
                    const bool same = runs(nb, a, b); // same direction: inconsistent
                    if (done[nb] != 0) {
                        if (same)
                            throw SurfaceFitError("The selection is not orientable (like a Moebius strip)");
                        continue;
                    }
                    if (same)
                        std::swap(tris[nb][1], tris[nb][2]);
                    done[nb] = 1;
                    stack.push_back(nb);
                }
            }
        }
    }
}

/// Border loops of a welded patch, or an error when it is not a simple sheet.
std::vector<std::vector<std::uint32_t>> sheetLoops(const std::vector<std::array<std::uint32_t, 3>>& tris,
                                                   const std::vector<Vec3>& pos)
{
    auto loops = borderLoops(tris, pos);
    if (loops.empty())
        throw SurfaceFitError("The selection is closed (no border); select an open region");
    // Topological disc with holes: V - E + F = 2 - 2g - b with genus 0.
    std::unordered_map<std::uint64_t, char> edges;
    std::vector<char> used(pos.size(), 0);
    for (const auto& t : tris)
        for (int s = 0; s < 3; ++s) {
            const auto a = t[static_cast<std::size_t>(s)], b = t[static_cast<std::size_t>((s + 1) % 3)];
            edges.emplace((static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b), 0);
            used[a] = 1;
        }
    long vertices = 0;
    for (const char c : used)
        vertices += c;
    const long euler = vertices - static_cast<long>(edges.size()) + static_cast<long>(tris.size());
    if (euler != 2 - static_cast<long>(loops.size()))
        throw SurfaceFitError("The selection is not a simple sheet (it has handles or non-manifold edges)");
    return loops;
}

/// Vertex clustering on a cubic grid of `cell` mm: vertices in one cell merge (at their
/// mean), collapsed and repeated triangles are dropped. Vertices flagged in `keep` are not
/// merged.
std::vector<std::int64_t> clusterVertices(std::vector<Vec3>& pos, std::vector<std::array<std::uint32_t, 3>>& tris,
                                          double cell, const std::vector<char>& keep)
{
    const std::size_t original = pos.size();
    std::unordered_map<std::uint64_t, std::uint32_t> clusterOf;
    std::vector<std::uint32_t> remap(pos.size());
    std::vector<Vec3> sum;
    std::vector<int> count;
    for (std::size_t i = 0; i < pos.size(); ++i) {
        auto c = [&](double x) {
            return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(x / cell)) + (1 << 20)) & 0x1FFFFF;
        };
        // Kept vertices (the border) stay exactly where they are, each its own cluster.
        const std::uint64_t key = keep[i] != 0 ? (1ull << 63) | i : c(pos[i].x) << 42 | c(pos[i].y) << 21 | c(pos[i].z);
        const auto [it, inserted] = clusterOf.try_emplace(key, static_cast<std::uint32_t>(sum.size()));
        if (inserted) {
            sum.push_back({0.0, 0.0, 0.0});
            count.push_back(0);
        }
        remap[i] = it->second;
        sum[it->second] = sum[it->second] + pos[i];
        ++count[it->second];
    }
    pos.resize(sum.size());
    for (std::size_t k = 0; k < sum.size(); ++k)
        pos[k] = sum[k] * (1.0 / count[k]);
    std::unordered_map<std::uint64_t, char> seen;
    std::vector<std::array<std::uint32_t, 3>> out;
    for (const auto& t : tris) {
        const std::array<std::uint32_t, 3> r{remap[t[0]], remap[t[1]], remap[t[2]]};
        if (r[0] == r[1] || r[1] == r[2] || r[0] == r[2])
            continue;
        std::array<std::uint32_t, 3> sorted = r;
        std::sort(sorted.begin(), sorted.end());
        const std::uint64_t key = static_cast<std::uint64_t>(sorted[0]) * 0x9E3779B97F4A7C15ull ^
                                  (static_cast<std::uint64_t>(sorted[1]) << 32 | sorted[2]);
        if (seen.emplace(key, 0).second)
            out.push_back(r);
    }
    // Drop clusters left without triangles.
    std::vector<std::int64_t> index(pos.size(), -1);
    std::vector<Vec3> kept;
    for (auto& t : out)
        for (auto& v : t) {
            if (index[v] < 0) {
                index[v] = static_cast<std::int64_t>(kept.size());
                kept.push_back(pos[v]);
            }
            v = static_cast<std::uint32_t>(index[v]);
        }
    pos = std::move(kept);
    tris = std::move(out);
    std::vector<std::int64_t> oldToNew(original, -1);
    for (std::size_t i = 0; i < original; ++i)
        oldToNew[i] = index[remap[i]];
    return oldToNew;
}

/// `trimTo` (a subset of `selection`, or empty): the outline and holes are taken from
/// that subset, while the whole selection is unfolded and fit (data beyond the trim, for
/// smooth joins with neighbouring patches).
Unfolded unfoldPatch(const core::TriangleMesh& mesh, const core::TriangleSet& selection,
                     const core::TriangleSet& trimTo, const std::function<void()>& checkCancel)
{
    Unfolded u;
    std::vector<std::array<std::uint32_t, 3>> trimTriangles;
    std::vector<char> inTrim;
    std::unordered_map<Vec3, std::uint32_t, PositionHash> ids;
    for (const auto t : selection) {
        std::array<std::uint32_t, 3> local{};
        for (int k = 0; k < 3; ++k) {
            const Vec3& p = mesh.vertices()[mesh.triangles()[t][static_cast<std::size_t>(k)]];
            const auto [it, inserted] = ids.try_emplace(p, static_cast<std::uint32_t>(u.positions.size()));
            if (inserted)
                u.positions.push_back(p);
            local[static_cast<std::size_t>(k)] = it->second;
        }
        if (local[0] == local[1] || local[1] == local[2] || local[0] == local[2])
            continue; // degenerate
        u.triangles.push_back(local);
        if (!trimTo.empty())
            inTrim.push_back(std::binary_search(trimTo.begin(), trimTo.end(), t) ? 1 : 0);
    }
    orientConsistently(u.triangles);
    auto loops = sheetLoops(u.triangles, u.positions);
    std::vector<std::vector<std::uint32_t>> trimLoops;
    if (!trimTo.empty()) {
        for (std::size_t k = 0; k < u.triangles.size(); ++k)
            if (inTrim[k] != 0)
                trimTriangles.push_back(u.triangles[k]);
        trimLoops = borderLoops(trimTriangles, u.positions);
        if (trimLoops.empty())
            throw SurfaceFitError("The trimmed part of the selection has no border");
    }

    // Large selections are unfolded on a coarser copy (the fit needs far fewer points
    // than a dense scan has); when coarsening changes the topology, the full mesh is used.
    constexpr std::size_t maxVertices = 12000;
    if (u.positions.size() > maxVertices) {
        double area = 0.0;
        for (const auto& t : u.triangles)
            area += 0.5 * (u.positions[t[1]] - u.positions[t[0]]).cross(u.positions[t[2]] - u.positions[t[0]]).length();
        auto pos = u.positions;
        auto tris = u.triangles;
        // The border stays exact: neighbouring patches share it when sewn.
        std::vector<char> border(pos.size(), 0);
        for (const auto* set : {&loops, &trimLoops})
            for (const auto& loop : *set)
                for (const auto v : loop)
                    border[v] = 1;
        const auto oldToNew = clusterVertices(pos, tris, std::sqrt(area / static_cast<double>(maxVertices)), border);
        try {
            auto coarseLoops = sheetLoops(tris, pos);
            bool kept = coarseLoops.size() == loops.size();
            auto coarseTrim = trimLoops;
            for (auto& loop : coarseTrim)
                for (auto& v : loop) {
                    kept = kept && oldToNew[v] >= 0;
                    v = kept ? static_cast<std::uint32_t>(oldToNew[v]) : 0;
                }
            if (kept) {
                u.positions = std::move(pos);
                u.triangles = std::move(tris);
                loops = std::move(coarseLoops);
                trimLoops = std::move(coarseTrim);
            }
        } catch (const SurfaceFitError&) {
            // keep the full mesh
        }
    }
    const std::size_t n = u.positions.size();
    const std::vector<std::uint32_t> outer = loops.front(); // for the pins
    const auto& trim = trimLoops.empty() ? loops : trimLoops;
    u.outer = trim.front();
    u.holes.assign(trim.begin() + 1, trim.end());

    // Least squares conformal map (Levy et al. 2002) with a free border: every triangle
    // should map to a similar copy of itself. Two far-apart vertices are pinned.
    std::size_t pinA = 0, pinB = 0;
    {
        auto farthest = [&](std::size_t from) {
            std::size_t best = from;
            double bestD = -1.0;
            for (const auto i : outer) {
                const double d = (u.positions[i] - u.positions[from]).length();
                if (d > bestD) {
                    bestD = d;
                    best = i;
                }
            }
            return best;
        };
        pinA = farthest(outer.front());
        pinB = farthest(pinA);
    }
    // Normal matrix of the energy over (u0, v0, u1, v1, ...), area weighted.
    const std::size_t dim = 2 * n;
    std::vector<std::unordered_map<std::uint32_t, double>> mat(dim);
    double area = 0.0;
    for (const auto& t : u.triangles) {
        const Vec3 e1 = u.positions[t[1]] - u.positions[t[0]];
        const Vec3 e2 = u.positions[t[2]] - u.positions[t[0]];
        const double l1 = e1.length();
        const double twiceArea = e1.cross(e2).length();
        if (l1 < 1e-300 || twiceArea < 1e-300)
            continue;
        area += 0.5 * twiceArea;
        // Triangle in its own frame: z0 = 0, z1 = (l1, 0), z2 = (x2, y2) with y2 > 0.
        const double x2 = e1.dot(e2) / l1, y2 = twiceArea / l1;
        const std::array<double, 3> zx{0.0, l1, x2}, zy{0.0, 0.0, y2};
        const double scale = 1.0 / std::sqrt(twiceArea);
        // W_j = z_{j+2} - z_{j+1}; sum_j W_j (u_j + i v_j) = 0 for a similarity.
        std::array<std::uint32_t, 6> idx{};
        std::array<double, 6> re{}, im{};
        for (int j = 0; j < 3; ++j) {
            const auto sj = static_cast<std::size_t>(j);
            const auto k = static_cast<std::size_t>((j + 2) % 3), l = static_cast<std::size_t>((j + 1) % 3);
            const double a = (zx[k] - zx[l]) * scale, b = (zy[k] - zy[l]) * scale;
            idx[2 * sj] = 2 * t[sj];
            idx[2 * sj + 1] = 2 * t[sj] + 1;
            re[2 * sj] = a; // real part: a u - b v
            re[2 * sj + 1] = -b;
            im[2 * sj] = b; // imaginary part: b u + a v
            im[2 * sj + 1] = a;
        }
        for (std::size_t p = 0; p < 6; ++p)
            for (std::size_t q = 0; q < 6; ++q)
                mat[idx[p]][idx[q]] += re[p] * re[q] + im[p] * im[q];
    }
    if (area <= 0.0)
        throw SurfaceFitError("The selection has no area");

    std::vector<double> x(dim, 0.0);
    std::vector<char> pinned(dim, 0);
    const double pinDistance = (u.positions[pinB] - u.positions[pinA]).length();
    pinned[2 * pinA] = pinned[2 * pinA + 1] = pinned[2 * pinB] = pinned[2 * pinB + 1] = 1;
    x[2 * pinB] = pinDistance;

    // Conjugate gradients with a Jacobi preconditioner on the free unknowns.
    std::vector<std::uint32_t> freeIds;
    std::vector<std::int64_t> slot(dim, -1);
    for (std::uint32_t i = 0; i < dim; ++i)
        if (pinned[i] == 0) {
            slot[i] = static_cast<std::int64_t>(freeIds.size());
            freeIds.push_back(i);
        }
    const std::size_t m = freeIds.size();
    // Free rows in compressed sparse row form, for a fast product.
    std::vector<double> diag(m, 1.0), rhs(m, 0.0), sol(m, 0.0);
    std::vector<std::size_t> rowStart{0};
    std::vector<std::uint32_t> cols;
    std::vector<double> vals;
    for (std::size_t r = 0; r < m; ++r) {
        for (const auto& [j, v] : mat[freeIds[r]]) {
            if (j == freeIds[r])
                diag[r] = v > 1e-300 ? v : 1.0;
            if (pinned[j] != 0) {
                rhs[r] -= v * x[j];
            } else {
                cols.push_back(static_cast<std::uint32_t>(slot[j]));
                vals.push_back(v);
            }
        }
        rowStart.push_back(cols.size());
    }
    mat.clear();
    auto apply = [&](const std::vector<double>& in, std::vector<double>& out) {
        for (std::size_t r = 0; r < m; ++r) {
            double s = 0.0;
            for (std::size_t k = rowStart[r]; k < rowStart[r + 1]; ++k)
                s += vals[k] * in[cols[k]];
            out[r] = s;
        }
    };
    // Symmetric successive over-relaxation as the preconditioner: far fewer iterations
    // than Jacobi on these Laplacian-like systems.
    constexpr double omega = 1.6;
    auto precondition = [&](const std::vector<double>& in, std::vector<double>& out) {
        for (std::size_t r = 0; r < m; ++r) { // (D / omega + L) y = in
            double s = in[r];
            for (std::size_t k = rowStart[r]; k < rowStart[r + 1]; ++k)
                if (cols[k] < r)
                    s -= vals[k] * out[cols[k]];
            out[r] = s * omega / diag[r];
        }
        for (std::size_t r = 0; r < m; ++r) // y <- D / omega y
            out[r] *= diag[r] / omega;
        for (std::size_t r = m; r-- > 0;) { // (D / omega + U) z = y
            double s = out[r];
            for (std::size_t k = rowStart[r]; k < rowStart[r + 1]; ++k)
                if (cols[k] > r)
                    s -= vals[k] * out[cols[k]];
            out[r] = s * omega / diag[r];
        }
    };
    {
        std::vector<double> res = rhs, z(m), p(m), ap(m);
        double rz = 0.0, norm0 = 0.0;
        precondition(res, z);
        for (std::size_t r = 0; r < m; ++r) {
            rz += res[r] * z[r];
            norm0 += rhs[r] * rhs[r];
        }
        p = z;
        const auto maxIter =
            std::max<std::size_t>(5000, static_cast<std::size_t>(50.0 * std::sqrt(static_cast<double>(m))));
        for (std::size_t iter = 0; iter < maxIter && m > 0; ++iter) {
            if (iter % 16 == 0)
                checkCancel();
            apply(p, ap);
            double pap = 0.0;
            for (std::size_t r = 0; r < m; ++r)
                pap += p[r] * ap[r];
            if (pap <= 0.0)
                break;
            const double alpha = rz / pap;
            double rr = 0.0;
            for (std::size_t r = 0; r < m; ++r) {
                sol[r] += alpha * p[r];
                res[r] -= alpha * ap[r];
                rr += res[r] * res[r];
            }
            if (rr <= 1e-16 * std::max(norm0, 1e-300))
                break;
            precondition(res, z);
            double rzNew = 0.0;
            for (std::size_t r = 0; r < m; ++r)
                rzNew += res[r] * z[r];
            const double beta = rzNew / rz;
            rz = rzNew;
            for (std::size_t r = 0; r < m; ++r)
                p[r] = z[r] + beta * p[r];
        }
    }
    for (std::size_t r = 0; r < m; ++r)
        x[freeIds[r]] = sol[r];

    // Principal axes of the unfolded patch, scaled to the 3D area, moved to the origin.
    u.uv.assign(n, {0.0, 0.0});
    double mu = 0.0, mv = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        mu += x[2 * i];
        mv += x[2 * i + 1];
    }
    mu /= static_cast<double>(n);
    mv /= static_cast<double>(n);
    double suu = 0.0, suv = 0.0, svv = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double du = x[2 * i] - mu, dv = x[2 * i + 1] - mv;
        suu += du * du;
        suv += du * dv;
        svv += dv * dv;
    }
    const double angle = 0.5 * std::atan2(2.0 * suv, suu - svv);
    const double ca = std::cos(angle), sa = std::sin(angle);
    double uvArea = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double du = x[2 * i] - mu, dv = x[2 * i + 1] - mv;
        u.uv[i] = {ca * du + sa * dv, -sa * du + ca * dv};
    }
    for (const auto& t : u.triangles) {
        const auto& p0 = u.uv[t[0]];
        const auto& p1 = u.uv[t[1]];
        const auto& p2 = u.uv[t[2]];
        uvArea += 0.5 * std::abs((p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1]));
    }
    if (!(uvArea > 0.0) || !std::isfinite(uvArea))
        throw SurfaceFitError("Could not unfold the selection");
    // Keep the triangles counter-clockwise in (u, v), so the surface normal (du x dv)
    // points like the mesh normals.
    double signedArea = 0.0;
    for (const auto& t : u.triangles) {
        const auto& p0 = u.uv[t[0]];
        const auto& p1 = u.uv[t[1]];
        const auto& p2 = u.uv[t[2]];
        signedArea += (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1]);
    }
    if (signedArea < 0.0)
        for (auto& p : u.uv)
            p[1] = -p[1];
    const double k = std::sqrt(area / uvArea);
    double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
    for (auto& p : u.uv) {
        p = {p[0] * k, p[1] * k};
        u0 = std::min(u0, p[0]);
        u1 = std::max(u1, p[0]);
        v0 = std::min(v0, p[1]);
        v1 = std::max(v1, p[1]);
    }
    for (auto& p : u.uv)
        p = {p[0] - u0, p[1] - v0};
    u.length = u1 - u0;
    u.width = v1 - v0;

    // The outer border encloses the largest area in the plane (a wiggly hole can be the
    // longest loop, so the length used above does not decide it).
    {
        auto enclosed = [&](const std::vector<std::uint32_t>& loop) {
            double a = 0.0;
            for (std::size_t i = 0; i < loop.size(); ++i) {
                const auto& p = u.uv[loop[i]];
                const auto& q = u.uv[loop[(i + 1) % loop.size()]];
                a += p[0] * q[1] - q[0] * p[1];
            }
            return std::abs(a);
        };
        std::size_t best = u.holes.size();
        double bestArea = enclosed(u.outer);
        for (std::size_t h = 0; h < u.holes.size(); ++h) {
            const double a = enclosed(u.holes[h]);
            if (a > bestArea) {
                bestArea = a;
                best = h;
            }
        }
        if (best < u.holes.size())
            std::swap(u.outer, u.holes[best]);
    }
    return u;
}

/// Cubic B-spline basis on a clamped uniform knot vector over [a, a + spans * h]:
/// returns the first control index and the 4 basis values at t.
std::pair<int, std::array<double, 4>> cubicBasis(double t, double a, double h, int spans)
{
    // Knots: a (x4), a + h, ..., a + (spans - 1) h, a + spans h (x4).
    auto knot = [&](int k) { return a + h * std::clamp(k - 3, 0, spans); };
    int s = std::clamp(static_cast<int>(std::floor((t - a) / h)), 0, spans - 1) + 3; // span index
    std::array<double, 4> n{1.0, 0.0, 0.0, 0.0};
    std::array<double, 4> left{}, right{};
    for (int j = 1; j <= 3; ++j) {
        left[static_cast<std::size_t>(j)] = t - knot(s + 1 - j);
        right[static_cast<std::size_t>(j)] = knot(s + j) - t;
        double saved = 0.0;
        for (int r = 0; r < j; ++r) {
            const double den = right[static_cast<std::size_t>(r + 1)] + left[static_cast<std::size_t>(j - r)];
            const double tmp = den != 0.0 ? n[static_cast<std::size_t>(r)] / den : 0.0;
            n[static_cast<std::size_t>(r)] = saved + right[static_cast<std::size_t>(r + 1)] * tmp;
            saved = left[static_cast<std::size_t>(j - r)] * tmp;
        }
        n[static_cast<std::size_t>(j)] = saved;
    }
    return {s - 3, n};
}

/// Bicubic B-spline surface with the unfolded (u, v) as its parameters, fit to the patch
/// vertices by penalized least squares (P-spline): data term plus a thin plate penalty
/// on the control net. Where there is no data (holes, the margin past the outline,
/// corners of the bounding rectangle) the penalty continues the surface linearly.
Handle(Geom_BSplineSurface) fitUnfolded(const Unfolded& u, double spacing, int marginSpans, double smoothing)
{
    const double a0 = -marginSpans * spacing, b0 = -marginSpans * spacing;
    const int spansU = std::max(1, static_cast<int>(std::ceil(u.length / spacing))) + 2 * marginSpans;
    const int spansV = std::max(1, static_cast<int>(std::ceil(u.width / spacing))) + 2 * marginSpans;
    const int ku = spansU + 3, kv = spansV + 3;
    // Unknown order: the shorter direction inside, so the matrix is banded and narrow.
    const bool uInner = ku <= kv;
    const int inner = uInner ? ku : kv;
    auto index = [&](int i, int j) { return uInner ? j * ku + i : i * kv + j; };
    const int count = ku * kv;
    const int band = 3 * inner + 3;
    // Lower band of the symmetric matrix: m[r * (band + 1) + d] = A(r, r - d).
    std::vector<double> m(static_cast<std::size_t>(count) * static_cast<std::size_t>(band + 1), 0.0);
    std::vector<std::array<double, 3>> rhs(static_cast<std::size_t>(count), {0.0, 0.0, 0.0});
    auto add = [&](int r, int c, double v) {
        if (r < c)
            std::swap(r, c);
        m[static_cast<std::size_t>(r) * static_cast<std::size_t>(band + 1) + static_cast<std::size_t>(r - c)] += v;
    };
    for (std::size_t p = 0; p < u.positions.size(); ++p) {
        const auto [iu, nu] = cubicBasis(u.uv[p][0], a0, spacing, spansU);
        const auto [iv, nv] = cubicBasis(u.uv[p][1], b0, spacing, spansV);
        std::array<int, 16> ids{};
        std::array<double, 16> w{};
        for (int a = 0; a < 4; ++a)
            for (int b = 0; b < 4; ++b) {
                ids[static_cast<std::size_t>(a * 4 + b)] = index(iu + a, iv + b);
                w[static_cast<std::size_t>(a * 4 + b)] =
                    nu[static_cast<std::size_t>(a)] * nv[static_cast<std::size_t>(b)];
            }
        for (std::size_t x = 0; x < 16; ++x) {
            const auto r = static_cast<std::size_t>(ids[x]);
            rhs[r][0] += w[x] * u.positions[p].x;
            rhs[r][1] += w[x] * u.positions[p].y;
            rhs[r][2] += w[x] * u.positions[p].z;
            for (std::size_t y = 0; y <= x; ++y)
                add(ids[x], ids[y], w[x] * w[y]); // each pair once: the lower half
        }
    }
    // Penalty scale relative to the mean data weight per control point.
    double trace = 0.0;
    for (int r = 0; r < count; ++r)
        trace += m[static_cast<std::size_t>(r) * static_cast<std::size_t>(band + 1)];
    const double lambda = smoothing * std::max(trace / count, 1e-12);
    auto stencil = [&](const std::vector<std::pair<int, double>>& terms, double weight) {
        for (const auto& [r, x] : terms)
            for (const auto& [c, y] : terms)
                if (r >= c)
                    add(r, c, weight * x * y);
    };
    for (int i = 0; i < ku; ++i)
        for (int j = 0; j < kv; ++j) {
            if (i + 2 < ku)
                stencil({{index(i, j), 1.0}, {index(i + 1, j), -2.0}, {index(i + 2, j), 1.0}}, lambda);
            if (j + 2 < kv)
                stencil({{index(i, j), 1.0}, {index(i, j + 1), -2.0}, {index(i, j + 2), 1.0}}, lambda);
            if (i + 1 < ku && j + 1 < kv)
                stencil(
                    {{index(i, j), 1.0}, {index(i + 1, j), -1.0}, {index(i, j + 1), -1.0}, {index(i + 1, j + 1), 1.0}},
                    2.0 * lambda);
        }
    for (int r = 0; r < count; ++r)
        m[static_cast<std::size_t>(r) * static_cast<std::size_t>(band + 1)] += 1e-12 * lambda + 1e-300;

    // Banded Cholesky, A = L L^T, in place.
    auto L = [&](int r, int c) -> double& {
        return m[static_cast<std::size_t>(r) * static_cast<std::size_t>(band + 1) + static_cast<std::size_t>(r - c)];
    };
    for (int r = 0; r < count; ++r) {
        const int c0 = std::max(0, r - band);
        for (int c = c0; c <= r; ++c) {
            double s = L(r, c);
            for (int k = std::max(c0, c - band); k < c; ++k)
                s -= L(r, k) * L(c, k);
            if (c == r) {
                if (s <= 0.0)
                    throw SurfaceFitError("Could not fit a surface to the unfolded selection");
                L(r, r) = std::sqrt(s);
            } else {
                L(r, c) = s / L(c, c);
            }
        }
    }
    // Solve L y = b, then L^T x = y.
    for (int r = 0; r < count; ++r) {
        for (int k = std::max(0, r - band); k < r; ++k)
            for (int d = 0; d < 3; ++d)
                rhs[static_cast<std::size_t>(r)][static_cast<std::size_t>(d)] -=
                    L(r, k) * rhs[static_cast<std::size_t>(k)][static_cast<std::size_t>(d)];
        for (int d = 0; d < 3; ++d)
            rhs[static_cast<std::size_t>(r)][static_cast<std::size_t>(d)] /= L(r, r);
    }
    for (int r = count - 1; r >= 0; --r) {
        for (int k = r + 1; k <= std::min(count - 1, r + band); ++k)
            for (int d = 0; d < 3; ++d)
                rhs[static_cast<std::size_t>(r)][static_cast<std::size_t>(d)] -=
                    L(k, r) * rhs[static_cast<std::size_t>(k)][static_cast<std::size_t>(d)];
        for (int d = 0; d < 3; ++d)
            rhs[static_cast<std::size_t>(r)][static_cast<std::size_t>(d)] /= L(r, r);
    }

    TColgp_Array2OfPnt poles(1, ku, 1, kv);
    for (int i = 0; i < ku; ++i)
        for (int j = 0; j < kv; ++j) {
            const auto& p = rhs[static_cast<std::size_t>(index(i, j))];
            poles.SetValue(i + 1, j + 1, gp_Pnt(p[0], p[1], p[2]));
        }
    auto knots = [&](int spans, double a, TColStd_Array1OfReal& k, TColStd_Array1OfInteger& mult) {
        for (int i = 0; i <= spans; ++i) {
            k(i + 1) = a + spacing * i;
            mult(i + 1) = (i == 0 || i == spans) ? 4 : 1;
        }
    };
    TColStd_Array1OfReal uk(1, spansU + 1), vk(1, spansV + 1);
    TColStd_Array1OfInteger um(1, spansU + 1), vm(1, spansV + 1);
    knots(spansU, a0, uk, um);
    knots(spansV, b0, vk, vm);
    return new Geom_BSplineSurface(poles, uk, vk, um, vm, 3, 3);
}

/// Closed 2D polyline on `surface` through `uv` (parameter space), as a wire, or a null
/// wire. `counterClockwise` selects the orientation; `smoothing` Taubin steps. Points
/// flagged in `corners` (same size as `uv`, or empty) start a new edge, so neighbouring
/// faces can share edges when sewn.
TopoDS_Wire uvWire(const Handle(Geom_BSplineSurface) & surface, std::vector<gp_Pnt2d> uv, double tolerance,
                   bool counterClockwise, int smoothing, std::vector<char> corners = {})
{
    if (uv.size() < 3)
        return {};
    if (corners.size() != uv.size())
        corners.assign(uv.size(), 0);
    // Light smoothing of the staircase that mesh border edges make. Taubin's lambda/mu
    // steps, so the outline does not shrink the way plain Laplacian smoothing does.
    for (int iter = 0; iter < smoothing; ++iter) {
        const double w = iter % 2 == 0 ? 0.5 : -0.53;
        std::vector<gp_Pnt2d> s(uv.size());
        for (std::size_t i = 0; i < uv.size(); ++i) {
            const gp_Pnt2d& a = uv[(i + uv.size() - 1) % uv.size()];
            const gp_Pnt2d& b = uv[(i + 1) % uv.size()];
            s[i] = corners[i] != 0 ? uv[i]
                                   : gp_Pnt2d(uv[i].X() + w * (0.5 * (a.X() + b.X()) - uv[i].X()),
                                              uv[i].Y() + w * (0.5 * (a.Y() + b.Y()) - uv[i].Y()));
        }
        uv = std::move(s);
    }
    double area = 0.0;
    for (std::size_t i = 0; i < uv.size(); ++i) {
        const gp_Pnt2d& a = uv[i];
        const gp_Pnt2d& b = uv[(i + 1) % uv.size()];
        area += a.X() * b.Y() - b.X() * a.Y();
    }
    if ((area < 0.0) == counterClockwise) {
        std::reverse(uv.begin(), uv.end());
        std::reverse(corners.begin(), corners.end());
    }

    // Start at a corner; each run of points up to the next corner is one degree-1
    // B-spline edge (a polyline), the whole loop one edge without corners.
    const std::size_t count = uv.size();
    std::size_t start = 0;
    while (start < count && corners[start] == 0)
        ++start;
    if (start == count)
        start = 0;
    std::vector<std::vector<gp_Pnt2d>> runs{{uv[start]}};
    for (std::size_t k = 1; k <= count; ++k) {
        const std::size_t i = (start + k) % count;
        runs.back().push_back(uv[i]);
        if (k < count && corners[i] != 0)
            runs.push_back({uv[i]});
    }
    BRepBuilderAPI_MakeWire wire;
    for (const auto& run : runs) {
        if (run.size() < 2)
            continue;
        const int n = static_cast<int>(run.size());
        TColgp_Array1OfPnt2d poles(1, n);
        TColStd_Array1OfReal knots(1, n);
        TColStd_Array1OfInteger mults(1, n);
        for (int i = 1; i <= n; ++i) {
            poles(i) = run[static_cast<std::size_t>(i - 1)];
            knots(i) = i - 1;
            mults(i) = (i == 1 || i == n) ? 2 : 1;
        }
        Handle(Geom2d_BSplineCurve) curve = new Geom2d_BSplineCurve(poles, knots, mults, 1);
        TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(curve, surface);
        BRepLib::BuildCurves3d(edge, std::max(tolerance * 0.1, 1e-5));
        wire.Add(edge);
        if (!wire.IsDone())
            return {};
    }
    return wire.IsDone() ? wire.Wire() : TopoDS_Wire{};
}

/// uvWire() through the projections of `loop` (points near the surface).
TopoDS_Wire loopWire(const Handle(Geom_BSplineSurface) & surface, const std::vector<Vec3>& loop, double tolerance,
                     bool counterClockwise, int smoothing, const std::vector<char>& corners = {})
{
    GeomAPI_ProjectPointOnSurf projector;
    std::vector<gp_Pnt2d> uv;
    std::vector<char> kept;
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const auto& p = loop[i];
        projector.Init(gp_Pnt(p.x, p.y, p.z), surface);
        if (projector.NbPoints() == 0)
            continue;
        double u = 0.0, v = 0.0;
        projector.LowerDistanceParameters(u, v);
        uv.emplace_back(u, v);
        kept.push_back(i < corners.size() ? corners[i] : 0);
    }
    return uvWire(surface, std::move(uv), tolerance, counterClockwise, smoothing, std::move(kept));
}

/// Valid face on `surface` bounded by the outer `wire`, or a null face.
TopoDS_Face faceFromWire(const Handle(Geom_BSplineSurface) & surface, const TopoDS_Wire& wire)
{
    if (wire.IsNull())
        return {};
    BRepBuilderAPI_MakeFace makeFace(surface, wire, Standard_True);
    if (!makeFace.IsDone())
        return {};
    ShapeFix_Face fix(makeFace.Face());
    fix.Perform();
    TopoDS_Face face = fix.Face();
    if (face.IsNull() || !BRepCheck_Analyzer(face).IsValid())
        return {};
    return face;
}

/// Cuts the hole wires (clockwise in parameter space) out of `face`; returns how many
/// were cut. A hole that fails is left closed with a warning.
int cutHoles(TopoDS_Face& face, const std::vector<TopoDS_Wire>& holes, std::vector<std::string>& warnings,
             const std::function<void()>& checkCancel)
{
    int cut = 0;
    for (const auto& wire : holes) {
        checkCancel();
        try {
            if (wire.IsNull())
                throw Standard_Failure("no wire");
            BRepBuilderAPI_MakeFace add(face);
            add.Add(wire);
            if (!add.IsDone())
                throw Standard_Failure("add");
            ShapeFix_Face fix(add.Face());
            fix.Perform();
            const TopoDS_Face candidate = fix.Face();
            if (candidate.IsNull() || !BRepCheck_Analyzer(candidate).IsValid())
                throw Standard_Failure("invalid");
            face = candidate;
            ++cut;
        } catch (const Standard_Failure&) {
            warnings.emplace_back("A hole in the selection could not be cut out of the surface.");
        }
    }
    return cut;
}

} // namespace

SurfaceFitResult fitSurface(const core::TriangleMesh& mesh, const core::TriangleSet& requested,
                            const SurfaceFitOptions& options)
{
    const auto t0 = std::chrono::steady_clock::now();
    SurfaceFitResult result;
    SurfaceFitReport& report = result.report;
    const std::function<void()> checkCancel = [&options] {
        if (options.cancelled && options.cancelled())
            throw SurfaceFitError("Cancelled");
    };
    for (const auto t : requested)
        if (t >= mesh.triangleCount())
            throw SurfaceFitError("Selection refers to triangles outside the mesh");

    // One face has one outer outline: separate patches would silently be dropped. Stray
    // triangles that only touch the region at a corner (brush and lasso edges) are left
    // out with a warning.
    const core::MeshAdjacency adjacency(mesh);
    auto parts = selectionComponents(adjacency, requested);
    if (parts.empty())
        throw SurfaceFitError("Select a larger region (at least 16 vertices) to fit a surface");
    if (parts.size() > 1) {
        const double total = core::selectionArea(mesh, requested);
        const double main = core::selectionArea(mesh, parts.front());
        if (total - main > 0.02 * total)
            throw SurfaceFitError("The selection has " + std::to_string(parts.size()) +
                                  " separate patches; fit one connected region at a time");
        report.warnings.push_back("Left out " + std::to_string(requested.size() - parts.front().size()) +
                                  " selected triangles not connected to the region by an edge.");
    }
    const core::TriangleSet& selection = parts.front();

    // Selected vertices (each position once) and the mean normal.
    std::unordered_map<Vec3, std::uint32_t, PositionHash> seen;
    std::vector<Vec3> pts;
    Vec3 normalSum;
    for (const auto t : selection) {
        const auto& tri = mesh.triangles()[t];
        const auto& v = mesh.vertices();
        normalSum = normalSum + (v[tri[1]] - v[tri[0]]).cross(v[tri[2]] - v[tri[0]]);
        for (const auto vi : tri) {
            if (seen.try_emplace(v[vi], static_cast<std::uint32_t>(pts.size())).second)
                pts.push_back(v[vi]);
        }
    }
    if (pts.size() < 16)
        throw SurfaceFitError("Select a larger region (at least 16 vertices) to fit a surface");

    const Frame frame = pcaFrame(pts, normalSum);

    // A height field over the plane is required: warn when the region folds over.
    std::size_t steep = 0;
    for (const auto t : selection) {
        if (std::abs(mesh.faceNormal(t).dot(frame.z)) < 0.1)
            ++steep;
    }
    const bool folded = steep * 20 > selection.size();

    // Unfolding handles any amount of wrap and gives the surface the region's own shape
    // in parameter space; it needs a simple sheet. Auto falls back to the height field
    // over the mean plane for selections that cannot be unfolded.
    std::optional<Unfolded> unfolded;
    if (options.method != SurfaceFitMethod::Plane) {
        try {
            unfolded = unfoldPatch(mesh, selection, options.trimTo, checkCancel);
        } catch (const SurfaceFitError& error) {
            if (options.method == SurfaceFitMethod::Unfold)
                throw;
            report.warnings.push_back(std::string("Could not unfold the selection (") + error.what() +
                                      "); fit as a height field over its mean plane instead.");
        }
    }
    checkCancel();
    const bool unfold = unfolded.has_value();
    if (folded && !unfold)
        report.warnings.push_back("The region bends more than about 85 degrees away from its mean plane; the fit "
                                  "may be poor. Use the unfold method or split the region.");

    double spanU = 0.0, spanV = 0.0;
    int grid = options.gridSize;
    int nu = 0, nv = 0;
    std::vector<Vec3> nodeGrid;
    auto gridFor = [&](double longSide) {
        // Dense enough for the points (about 3 per node and side) and for the mesh
        // resolution (a node every two mean edge lengths, so coarse meshes of long or
        // curved regions keep their shape), 10..60 nodes along the longer side.
        if (options.gridSize > 0)
            return options.gridSize;
        double edgeSum = 0.0;
        for (const auto t : selection) {
            const auto& tri = mesh.triangles()[t];
            edgeSum += (mesh.vertices()[tri[1]] - mesh.vertices()[tri[0]]).length();
        }
        const double meanEdge = std::max(edgeSum / static_cast<double>(selection.size()), 1e-9);
        const double byPoints = std::sqrt(static_cast<double>(pts.size())) / 3.0;
        const double byEdges = longSide / (2.0 * meanEdge);
        return std::clamp(static_cast<int>(std::max(byPoints, byEdges)), 10, 60);
    };

    if (!unfold) {
        std::vector<std::array<double, 3>> uvw;
        uvw.reserve(pts.size());
        double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
        for (const auto& p : pts) {
            uvw.push_back(frame.toLocal(p));
            u0 = std::min(u0, uvw.back()[0]);
            u1 = std::max(u1, uvw.back()[0]);
            v0 = std::min(v0, uvw.back()[1]);
            v1 = std::max(v1, uvw.back()[1]);
        }
        spanU = std::max(u1 - u0, 1e-6);
        spanV = std::max(v1 - v0, 1e-6);
        if (std::min(spanU, spanV) < 1e-3 * std::max(spanU, spanV))
            throw SurfaceFitError("The selected region is degenerate (a line)");
        grid = gridFor(std::max(spanU, spanV));
        nu = spanU >= spanV ? grid : std::max(4, static_cast<int>(std::lround(grid * spanU / spanV)));
        nv = spanV >= spanU ? grid : std::max(4, static_cast<int>(std::lround(grid * spanV / spanU)));
        const double hu = spanU / (nu - 1);
        const double hv = spanV / (nv - 1);
        const Buckets buckets(uvw, u0, u1, v0, v1, std::clamp(grid, 8, 128));

        // Each node: moving least squares, a weighted quadratic through the points within 2
        // cells, evaluated at the node. Averages noise without flattening slopes or
        // curvature. Near the border the neighbourhood is one-sided and a plane is used.
        // Radius: two cells, or larger for sparse points (a forced fine grid, a small
        // selection) so that a disc holds about 12 points on average.
        const double sparseRadius =
            1.3 * std::sqrt(12.0 * spanU * spanV / (std::numbers::pi * static_cast<double>(pts.size())));
        const double baseRadius = std::max(2.0 * std::max(hu, hv), sparseRadius);
        std::vector<double> heights(static_cast<std::size_t>(nu) * static_cast<std::size_t>(nv), 0.0);
        std::vector<char> known(heights.size(), 0);
        std::size_t covered = 0; // nodes with data all around
        auto at = [nu](int i, int j) {
            return static_cast<std::size_t>(j) * static_cast<std::size_t>(nu) + static_cast<std::size_t>(i);
        };
        for (int j = 0; j < nv; ++j) {
            for (int i = 0; i < nu; ++i) {
                const double u = u0 + i * hu;
                const double v = v0 + j * hv;
                const double r0 = baseRadius;
                for (double r = r0; r < 2.3 * r0; r *= 1.5) {
                    const bool first = r == r0;
                    std::array<std::array<double, 6>, 6> m{};
                    std::array<double, 6> rhs{};
                    int count = 0;
                    double sw = 0.0, sh = 0.0, su = 0.0, sv = 0.0;
                    buckets.visit(u, v, r, [&](std::uint32_t k) {
                        const auto& q = uvw[k];
                        const double du = (q[0] - u) / r, dv = (q[1] - v) / r; // scaled for conditioning
                        const double d2 = du * du + dv * dv;
                        if (d2 > 1.0)
                            return;
                        const double t = 1.0 - d2;
                        const double w = t * t + 1e-3;
                        const std::array<double, 6> basis{1.0, du, dv, du * du, du * dv, dv * dv};
                        for (std::size_t x = 0; x < 6; ++x) {
                            rhs[x] += w * basis[x] * q[2];
                            for (std::size_t y = 0; y < 6; ++y)
                                m[x][y] += w * basis[x] * basis[y];
                        }
                        sw += w;
                        sh += w * q[2];
                        su += w * du;
                        sv += w * dv;
                        ++count;
                    });
                    if (count < 12)
                        continue;
                    // One-sided neighbourhood (border, outside the data): a quadratic would
                    // amplify noise there, so extrapolate with the plane only.
                    const bool oneSided = std::hypot(su / sw, sv / sw) > 0.2;
                    const auto solution = solveSymmetric6(m, rhs, oneSided ? 3 : 6);
                    heights[at(i, j)] = solution ? (*solution)[0] : sh / sw;
                    known[at(i, j)] = 1;
                    if (first && !oneSided)
                        ++covered;
                    break;
                }
            }
        }

        if (covered * 10 < heights.size() * 3)
            report.warnings.push_back("The region covers little of its bounding rectangle (long, thin or curved "
                                      "in plan); the fit may be poor there. Use the unfold method or split it.");

        // Nodes far from the data (the parts of the fit rectangle outside the selection) are
        // filled smoothly: harmonic relaxation, then biharmonic (thin plate) so slopes continue
        // across the selection border. Those parts are trimmed away, but jumps there would make
        // the B-spline wiggle inside the selection.
        {
            double mean = 0.0;
            std::size_t knownCount = 0;
            for (std::size_t k = 0; k < heights.size(); ++k) {
                if (known[k] != 0) {
                    mean += heights[k];
                    ++knownCount;
                }
            }
            if (knownCount == 0)
                throw SurfaceFitError("Not enough points in the selection to fit a surface");
            mean /= static_cast<double>(knownCount);
            for (std::size_t k = 0; k < heights.size(); ++k) {
                if (known[k] == 0)
                    heights[k] = mean;
            }
            fillUnknown(heights, known, nu, nv);
        }

        nodeGrid.resize(heights.size());
        for (int j = 0; j < nv; ++j)
            for (int i = 0; i < nu; ++i)
                nodeGrid[at(i, j)] = frame.toWorld(u0 + i * hu, v0 + j * hv, heights[at(i, j)]);
    }

    // Unfolded: the surface parameters are the unfolded (u, v) in mm, so the outline and
    // holes are known in parameter space and need no projection.
    Handle(Geom_BSplineSurface) surface;
    if (unfold) {
        spanU = unfolded->length;
        spanV = unfolded->width;
        const double longSide = std::max(spanU, spanV);
        grid = gridFor(longSide);
        surface = fitUnfolded(*unfolded, longSide / (grid - 1), 2, 1e-3);
    } else {
        TColgp_Array2OfPnt nodes(1, nu, 1, nv);
        for (int j = 0; j < nv; ++j)
            for (int i = 0; i < nu; ++i) {
                const Vec3& p =
                    nodeGrid[static_cast<std::size_t>(j) * static_cast<std::size_t>(nu) + static_cast<std::size_t>(i)];
                nodes.SetValue(i + 1, j + 1, gp_Pnt(p.x, p.y, p.z));
            }

        // Smoothing (variational) mode, the most stable on noisy data in the CAD-66 spike.
        // When the tolerance cannot be reached the fit fails; retry with a looser one and say so.
        double fitTolerance = options.tolerance;
        for (int attempt = 0; attempt < 4 && surface.IsNull(); ++attempt, fitTolerance *= 2.0) {
            try {
                GeomAPI_PointsToBSplineSurface fit;
                // Weights: length, curvature, torsion (only their ratio matters).
                fit.Init(nodes, 1e-3, 1e-4, 0.0, 8, GeomAbs_C2, fitTolerance);
                if (fit.IsDone())
                    surface = fit.Surface();
            } catch (const Standard_Failure&) {
                // try again with a looser tolerance
            }
        }
        if (surface.IsNull())
            throw SurfaceFitError("B-spline surface fit failed; the region may be too noisy or folded");
        if (fitTolerance / 2.0 > options.tolerance)
            report.warnings.push_back("The fit needed a tolerance of " + std::to_string(fitTolerance / 2.0) + " mm.");
    }
    checkCancel();
    report.method = unfold ? "unfold" : "plane";
    report.uPoles = surface->NbUPoles();
    report.vPoles = surface->NbVPoles();

    TopoDS_Face face;
    {
        // Trim to the outer outline of the selection and cut the holes; fall back to the
        // whole fit rectangle.
        std::vector<TopoDS_Wire> holeWires;
        // Exact mesh border (no smoothing) with corners where neighbouring patches meet,
        // for sewing; else a smoothed, resampled outline.
        const int smoothing = std::max(0, options.outlineSmoothing);
        std::unordered_map<Vec3, char, PositionHash> cornerSet;
        for (const auto& c : options.outlineCorners)
            cornerSet.emplace(c, 1);
        auto cornersOf = [&](const std::vector<Vec3>& loop) {
            std::vector<char> flags(loop.size(), 0);
            for (std::size_t i = 0; i < loop.size(); ++i)
                flags[i] = cornerSet.count(loop[i]) != 0 ? 1 : 0;
            return flags;
        };
        auto uvLoopWire = [&](const std::vector<std::uint32_t>& loop, bool outer) {
            std::vector<Vec3> flat, at;
            for (const auto id : loop) {
                flat.push_back({unfolded->uv[id][0], unfolded->uv[id][1], 0.0});
                at.push_back(unfolded->positions[id]);
            }
            std::vector<char> corners;
            if (smoothing > 0) // two samples per border edge, so smoothing only rounds the staircase
                flat = resampleLoop(flat, std::clamp<std::size_t>(2 * loop.size(), 32, 4000));
            else
                corners = cornersOf(at);
            std::vector<gp_Pnt2d> uv;
            for (const auto& p : flat)
                uv.emplace_back(p.x, p.y);
            return uvWire(surface, std::move(uv), options.tolerance, outer, smoothing, std::move(corners));
        };
        auto meshLoopWire = [&](const std::vector<Vec3>& loop, bool outer, std::size_t minSamples,
                                std::size_t maxSamples) {
            if (smoothing == 0)
                return loopWire(surface, loop, options.tolerance, outer, 0, cornersOf(loop));
            const std::size_t samples = std::clamp<std::size_t>(loop.size(), minSamples, maxSamples);
            return loopWire(surface, resampleLoop(loop, samples), options.tolerance, outer, smoothing);
        };
        try {
            if (unfolded) {
                face = faceFromWire(surface, uvLoopWire(unfolded->outer, true));
                if (!face.IsNull())
                    for (const auto& hole : unfolded->holes)
                        holeWires.push_back(uvLoopWire(hole, false));
            } else {
                auto outlines = selectionOutlines(mesh, adjacency, options.trimTo.empty() ? selection : options.trimTo);
                // The outer outline encloses the largest area over the plane (a wiggly
                // hole can be the longest loop).
                auto enclosed = [&](const std::vector<Vec3>& loop) {
                    double a = 0.0;
                    for (std::size_t i = 0; i < loop.size(); ++i) {
                        const auto p = frame.toLocal(loop[i]);
                        const auto q = frame.toLocal(loop[(i + 1) % loop.size()]);
                        a += p[0] * q[1] - q[0] * p[1];
                    }
                    return std::abs(a);
                };
                for (std::size_t k = 1; k < outlines.size(); ++k)
                    if (enclosed(outlines[k]) > enclosed(outlines.front()))
                        std::swap(outlines[k], outlines.front());
                if (!outlines.empty())
                    face = faceFromWire(surface, meshLoopWire(outlines.front(), true, 16, 2000));
                // The selection is one connected patch, so every other loop is a hole.
                for (std::size_t k = 1; !face.IsNull() && k < outlines.size(); ++k)
                    holeWires.push_back(meshLoopWire(outlines[k], false, 8, 1000));
            }
        } catch (const Standard_Failure&) {
            face.Nullify();
        }
        report.trimmed = !face.IsNull();
        if (face.IsNull()) {
            face = BRepBuilderAPI_MakeFace(surface, 1e-6);
            report.warnings.push_back("Could not trim the surface to the selection outline; it covers the whole "
                                      "fit rectangle.");
        } else if (!holeWires.empty()) {
            report.holes = cutHoles(face, holeWires, report.warnings, checkCancel);
        }
    }

    checkCancel();
    // Mesh -> face.
    // Each point is projected with a local search (Extrema_GenLocateExtPS) started from
    // the nearest of a grid of surface samples: exact and much faster than a global
    // projection per point.
    double su0 = 0.0, su1 = 0.0, sv0 = 0.0, sv1 = 0.0;
    surface->Bounds(su0, su1, sv0, sv1);
    constexpr int seedCount = 64;
    std::vector<std::array<double, 3>> seedLocal;
    std::vector<std::array<double, 2>> seedParams;
    double su0l = 1e300, su1l = -1e300, sv0l = 1e300, sv1l = -1e300;
    for (int j = 0; j <= seedCount; ++j) {
        for (int i = 0; i <= seedCount; ++i) {
            const double pu = su0 + (su1 - su0) * i / seedCount;
            const double pv = sv0 + (sv1 - sv0) * j / seedCount;
            const gp_Pnt sp = surface->Value(pu, pv);
            seedLocal.push_back(frame.toLocal({sp.X(), sp.Y(), sp.Z()}));
            seedParams.push_back({pu, pv});
            su0l = std::min(su0l, seedLocal.back()[0]);
            su1l = std::max(su1l, seedLocal.back()[0]);
            sv0l = std::min(sv0l, seedLocal.back()[1]);
            sv1l = std::max(sv1l, seedLocal.back()[1]);
        }
    }
    const Buckets seeds(seedLocal, su0l, su1l + 1e-9, sv0l, sv1l + 1e-9, 32);
    const GeomAdaptor_Surface adaptor(surface);
    Extrema_GenLocateExtPS locate(adaptor);
    // With a trim subset, only its points are measured: the rest is fit data beyond the
    // face, covered by the neighbouring patch.
    std::vector<Vec3> trimPts;
    if (!options.trimTo.empty()) {
        std::unordered_set<Vec3, PositionHash> seenTrim;
        for (const auto t : options.trimTo)
            for (const auto vi : mesh.triangles()[t])
                if (seenTrim.insert(mesh.vertices()[vi]).second)
                    trimPts.push_back(mesh.vertices()[vi]);
    }
    const std::vector<Vec3>& measured = trimPts.empty() ? pts : trimPts;
    const std::size_t stride = std::max<std::size_t>(1, measured.size() / 20000);
    double sum = 0.0;
    std::size_t count = 0;
    // Points just outside the trim are on the face's edge (the smoothed outline passes a
    // little inside a coarse border), not off the face: they count as covered (CAD-255).
    // The reach is one mean edge length, at most 5 % of the selection's size; it does not
    // grow with the fit tolerance, so a loose tolerance cannot pass a sliver face.
    std::size_t nearOutline = 0;
    double outlineReach = 0.0;
    {
        const core::TriangleSet& tris = options.trimTo.empty() ? selection : options.trimTo;
        double edgeSum = 0.0;
        for (const auto t : tris) {
            const auto& tri = mesh.triangles()[t];
            for (int e = 0; e < 3; ++e)
                edgeSum += (mesh.vertices()[tri[(e + 1) % 3]] - mesh.vertices()[tri[e]]).length();
        }
        Vec3 lo = measured.empty() ? Vec3{} : measured.front(), hi = lo;
        for (const auto& q : measured) {
            lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
            hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
        }
        if (!tris.empty())
            outlineReach = std::min(edgeSum / (3.0 * static_cast<double>(tris.size())), 0.05 * (hi - lo).length());
    }

    // A point whose projection falls outside the trim is not on the created face: it is
    // measured to the face boundary (densely sampled polyline) and reported separately.
    std::optional<BRepTopAdaptor_FClass2d> classifier;
    std::vector<Vec3> boundary;
    if (report.trimmed) {
        classifier.emplace(face, 1e-7);
        for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
            const BRepAdaptor_Curve curve(TopoDS::Edge(ex.Current()));
            constexpr int n = 2000;
            for (int i = 0; i <= n; ++i) {
                const gp_Pnt p = curve.Value(curve.FirstParameter() +
                                             (curve.LastParameter() - curve.FirstParameter()) * i / n);
                boundary.push_back({p.X(), p.Y(), p.Z()});
            }
        }
    }
    auto boundaryDistance = [&](const Vec3& q) {
        double best = std::numeric_limits<double>::max();
        for (std::size_t i = 1; i < boundary.size(); ++i) {
            const Vec3 a = boundary[i - 1];
            const Vec3 ab = boundary[i] - a;
            const double len2 = ab.dot(ab);
            const double t = len2 > 0.0 ? std::clamp((q - a).dot(ab) / len2, 0.0, 1.0) : 0.0;
            best = std::min(best, (q - (a + ab * t)).length());
        }
        return best;
    };
    for (std::size_t i = 0; i < measured.size(); i += stride) {
        if ((i / stride) % 1024 == 0)
            checkCancel();
        const auto l = frame.toLocal(measured[i]);
        double d = 0.0;
        const auto k = seeds.nearest(l[0], l[1], l[2], d);
        try {
            locate.Perform(gp_Pnt(measured[i].x, measured[i].y, measured[i].z), seedParams[k][0], seedParams[k][1]);
            double pu = seedParams[k][0], pv = seedParams[k][1];
            if (locate.IsDone()) {
                d = std::min(d, std::sqrt(locate.SquareDistance()));
                locate.Point().Parameter(pu, pv);
            }
            if (classifier && classifier->Perform(gp_Pnt2d(pu, pv)) == TopAbs_OUT) {
                const double off = boundaryDistance(measured[i]);
                report.outsideTrimMax = std::max(report.outsideTrimMax, off);
                if (off <= outlineReach)
                    ++nearOutline;
                continue; // not on the created face; reported as outsideTrimMax
            }
        } catch (const Standard_Failure&) {
            // keep the sample distance
        }
        report.meshToSurfaceMax = std::max(report.meshToSurfaceMax, d);
        sum += d * d;
        ++count;
    }
    report.meshToSurfaceRms = count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
    // Most of the selection must lie on the face. When the trim outline went wrong (an
    // area folded over itself, e.g. a whole car body seen from above), the face is a
    // sliver and nothing was measured on it: deviation 0 "within the tolerance" (CAD-255).
    const std::size_t sampled = (measured.size() + stride - 1) / stride;
    if (sampled > 0 && (count + nearOutline) * 2 < sampled) {
        char pct[32];
        std::snprintf(pct, sizeof pct, "%.0f",
                      100.0 * static_cast<double>(count + nearOutline) / static_cast<double>(sampled));
        throw SurfaceFitError(std::string("The surface covers only ") + pct +
                              " % of the selected area (its outline could not follow the selection). Select a "
                              "smaller area that does not fold over itself");
    }

    checkCancel();
    // Surface -> mesh, sampled on the face's own triangulation (so only inside the trim).
    // Measured against the selection grown by two rings: the smoothed trim outline may
    // pass a fraction of a triangle outside the selected triangles, which is not a
    // deviation from the scanned surface.
    const core::TriangleSet reference =
        core::growSelection(adjacency, options.trimTo.empty() ? selection : options.trimTo, 2);
    std::unordered_map<Vec3, std::uint32_t, PositionHash> refIndex;
    std::vector<Vec3> refPts;
    std::vector<std::array<double, 3>> refLocal;
    double ru0 = 1e300, ru1 = -1e300, rv0 = 1e300, rv1 = -1e300;
    for (const auto t : reference) {
        for (const auto vi : mesh.triangles()[t]) {
            const Vec3& p = mesh.vertices()[vi];
            const auto [it, inserted] = refIndex.try_emplace(p, static_cast<std::uint32_t>(refPts.size()));
            if (inserted) {
                refPts.push_back(p);
                refLocal.push_back(frame.toLocal(p));
                ru0 = std::min(ru0, refLocal.back()[0]);
                ru1 = std::max(ru1, refLocal.back()[0]);
                rv0 = std::min(rv0, refLocal.back()[1]);
                rv1 = std::max(rv1, refLocal.back()[1]);
            }
        }
    }
    const Buckets refBuckets(refLocal, ru0, ru1 + 1e-9, rv0, rv1 + 1e-9, std::clamp(grid, 8, 128));
    // Reference triangles by their (u, v) bounding boxes.
    const int triGrid = std::clamp(grid, 8, 128);
    const double triCellU = std::max(ru1 - ru0, 1e-9) / triGrid;
    const double triCellV = std::max(rv1 - rv0, 1e-9) / triGrid;
    auto triCell = [triGrid](double x, double x0, double cell) {
        return std::clamp(static_cast<int>((x - x0) / cell), 0, triGrid - 1);
    };
    std::vector<std::vector<std::uint32_t>> triCells(static_cast<std::size_t>(triGrid * triGrid));
    for (std::uint32_t k = 0; k < reference.size(); ++k) {
        const auto& tv = mesh.triangles()[reference[k]];
        double a0 = 1e300, a1 = -1e300, b0 = 1e300, b1 = -1e300;
        for (const auto vi : tv) {
            const auto lp = frame.toLocal(mesh.vertices()[vi]);
            a0 = std::min(a0, lp[0]);
            a1 = std::max(a1, lp[0]);
            b0 = std::min(b0, lp[1]);
            b1 = std::max(b1, lp[1]);
        }
        for (int cj = triCell(b0, rv0, triCellV); cj <= triCell(b1, rv0, triCellV); ++cj)
            for (int ci = triCell(a0, ru0, triCellU); ci <= triCell(a1, ru0, triCellU); ++ci)
                triCells[static_cast<std::size_t>(cj * triGrid + ci)].push_back(k);
    }
    std::vector<std::uint32_t> triStamp(reference.size(), 0);
    std::uint32_t stamp = 0;
    const double diag = std::hypot(spanU, spanV);
    BRepMesh_IncrementalMesh mesher(face, std::max(diag * 2e-3, 1e-4), false, 0.3, false);
    TopLoc_Location loc;
    const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    if (!tri.IsNull()) {
        const int nodesCount = tri->NbNodes();
        const int step = std::max(1, nodesCount / 6000);
        for (int i = 1; i <= nodesCount; i += step) {
            const gp_Pnt p = tri->Node(i).Transformed(loc.Transformation());
            const Vec3 q{p.X(), p.Y(), p.Z()};
            const auto l = frame.toLocal(q);
            // Distance to the mesh surface: the nearest vertex bounds it from above; every
            // triangle that could be closer has its (u, v) box within that bound.
            double d = 0.0;
            refBuckets.nearest(l[0], l[1], l[2], d);
            ++stamp;
            const int i0 = triCell(l[0] - d, ru0, triCellU), i1 = triCell(l[0] + d, ru0, triCellU);
            const int j0 = triCell(l[1] - d, rv0, triCellV), j1 = triCell(l[1] + d, rv0, triCellV);
            for (int cj = j0; cj <= j1; ++cj) {
                for (int ci = i0; ci <= i1; ++ci) {
                    for (const auto t : triCells[static_cast<std::size_t>(cj * triGrid + ci)]) {
                        if (triStamp[t] == stamp)
                            continue;
                        triStamp[t] = stamp;
                        const auto& tv = mesh.triangles()[reference[t]];
                        d = std::min(d, core::pointTriangleDistance(q, mesh.vertices()[tv[0]], mesh.vertices()[tv[1]],
                                                                    mesh.vertices()[tv[2]]));
                    }
                }
            }
            report.surfaceToMeshMax = std::max(report.surfaceToMeshMax, d);
        }
    }

    report.withinTolerance =
        report.meshToSurfaceMax <= options.tolerance && report.surfaceToMeshMax <= options.tolerance;
    if (report.outsideTrimMax > options.tolerance) {
        char mm[32];
        std::snprintf(mm, sizeof mm, "%.2f", report.outsideTrimMax);
        report.warnings.push_back(std::string("The smoothed outline leaves selected mesh up to ") + mm +
                                  " mm outside the face (rounded corners or a coarse mesh edge).");
    }
    if (!report.withinTolerance)
        report.warnings.push_back("Deviation exceeds the tolerance; try a smaller or more compact region, or a "
                                  "larger tolerance.");

    result.face = wrapShape(face);
    report.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return result;
}

} // namespace aerocad::geometry
