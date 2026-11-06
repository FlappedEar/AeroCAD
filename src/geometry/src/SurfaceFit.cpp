// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/SurfaceFit.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepLib.hxx>
#include <BRepAdaptor_Curve.hxx>
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
#include <TopExp_Explorer.hxx>
#include <TopAbs_State.hxx>
#include <gp_Pnt2d.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <math_Jacobi.hxx>
#include <math_Matrix.hxx>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <unordered_map>

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

/// Number of edge-connected patches in a sorted selection.
std::size_t selectionComponents(const core::MeshAdjacency& adjacency, const core::TriangleSet& selection)
{
    std::unordered_map<std::uint32_t, char> visited;
    visited.reserve(selection.size());
    for (const auto t : selection)
        visited.emplace(t, 0);
    std::size_t parts = 0;
    std::vector<std::uint32_t> stack;
    for (const auto start : selection) {
        if (visited[start] != 0)
            continue;
        ++parts;
        visited[start] = 1;
        stack.push_back(start);
        while (!stack.empty()) {
            const auto t = stack.back();
            stack.pop_back();
            for (const auto nb : adjacency.neighbours(t)) {
                const auto it = visited.find(nb);
                if (it != visited.end() && it->second == 0) {
                    it->second = 1;
                    stack.push_back(nb);
                }
            }
        }
    }
    return parts;
}

/// Face bounded by `outline` (points near the surface), or a null face on failure.
TopoDS_Face trimmedFace(const Handle(Geom_BSplineSurface) & surface, const std::vector<Vec3>& outline, double tolerance)
{
    GeomAPI_ProjectPointOnSurf projector;
    std::vector<gp_Pnt2d> uv;
    for (const auto& p : outline) {
        projector.Init(gp_Pnt(p.x, p.y, p.z), surface);
        if (projector.NbPoints() == 0)
            continue;
        double u = 0.0, v = 0.0;
        projector.LowerDistanceParameters(u, v);
        uv.emplace_back(u, v);
    }
    if (uv.size() < 8)
        return {};

    // Light smoothing of the staircase that mesh border edges make. Taubin's lambda/mu
    // steps, so the outline does not shrink the way plain Laplacian smoothing does.
    for (int iter = 0; iter < 6; ++iter) {
        const double w = iter % 2 == 0 ? 0.5 : -0.53;
        std::vector<gp_Pnt2d> s(uv.size());
        for (std::size_t i = 0; i < uv.size(); ++i) {
            const gp_Pnt2d& a = uv[(i + uv.size() - 1) % uv.size()];
            const gp_Pnt2d& b = uv[(i + 1) % uv.size()];
            s[i] = gp_Pnt2d(uv[i].X() + w * (0.5 * (a.X() + b.X()) - uv[i].X()),
                            uv[i].Y() + w * (0.5 * (a.Y() + b.Y()) - uv[i].Y()));
        }
        uv = std::move(s);
    }

    // Outer boundary must run counter-clockwise in parameter space.
    double area = 0.0;
    for (std::size_t i = 0; i < uv.size(); ++i) {
        const gp_Pnt2d& a = uv[i];
        const gp_Pnt2d& b = uv[(i + 1) % uv.size()];
        area += a.X() * b.Y() - b.X() * a.Y();
    }
    if (area < 0.0)
        std::reverse(uv.begin(), uv.end());

    // Closed degree-1 B-spline through the points (a polyline as one edge).
    const int n = static_cast<int>(uv.size()) + 1;
    TColgp_Array1OfPnt2d poles(1, n);
    TColStd_Array1OfReal knots(1, n);
    TColStd_Array1OfInteger mults(1, n);
    for (int i = 1; i <= n; ++i) {
        poles(i) = uv[static_cast<std::size_t>((i - 1) % (n - 1))];
        knots(i) = i - 1;
        mults(i) = (i == 1 || i == n) ? 2 : 1;
    }
    Handle(Geom2d_BSplineCurve) curve = new Geom2d_BSplineCurve(poles, knots, mults, 1);

    TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(curve, surface);
    BRepLib::BuildCurves3d(edge, std::max(tolerance * 0.1, 1e-5));
    BRepBuilderAPI_MakeWire wire(edge);
    if (!wire.IsDone())
        return {};
    BRepBuilderAPI_MakeFace makeFace(surface, wire.Wire(), Standard_True);
    if (!makeFace.IsDone())
        return {};
    ShapeFix_Face fix(makeFace.Face());
    fix.Perform();
    TopoDS_Face face = fix.Face();
    if (face.IsNull() || !BRepCheck_Analyzer(face).IsValid())
        return {};
    return face;
}

} // namespace

SurfaceFitResult fitSurface(const core::TriangleMesh& mesh, const core::TriangleSet& selection,
                            const SurfaceFitOptions& options)
{
    const auto t0 = std::chrono::steady_clock::now();
    SurfaceFitResult result;
    SurfaceFitReport& report = result.report;

    // Selected vertices (each position once) and the mean normal.
    std::unordered_map<Vec3, std::uint32_t, PositionHash> seen;
    std::vector<Vec3> pts;
    Vec3 normalSum;
    for (const auto t : selection) {
        if (t >= mesh.triangleCount())
            throw SurfaceFitError("Selection refers to triangles outside the mesh");
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
    const core::MeshAdjacency adjacency(mesh);

    // One face has one outer outline: separate patches would silently be dropped.
    {
        const std::size_t parts = selectionComponents(adjacency, selection);
        if (parts > 1)
            throw SurfaceFitError("The selection has " + std::to_string(parts) +
                                  " separate patches; fit one connected region at a time");
    }

    // A height field over the plane is required: warn when the region folds over.
    std::size_t steep = 0;
    for (const auto t : selection) {
        if (std::abs(mesh.faceNormal(t).dot(frame.z)) < 0.1)
            ++steep;
    }
    if (steep * 20 > selection.size())
        report.warnings.push_back("The region bends more than about 85 degrees away from its mean plane; the fit "
                                  "may be poor. Select a flatter region or split it.");

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
    const double spanU = std::max(u1 - u0, 1e-6);
    const double spanV = std::max(v1 - v0, 1e-6);
    if (std::min(spanU, spanV) < 1e-3 * std::max(spanU, spanV))
        throw SurfaceFitError("The selected region is degenerate (a line)");

    // Grid: dense enough for the points (about 3 per node and side) and for the mesh
    // resolution (a node every two mean edge lengths, so coarse meshes of long or curved
    // regions keep their shape), 10..60 nodes along the longer side.
    int grid = options.gridSize;
    if (grid <= 0) {
        double edgeSum = 0.0;
        for (const auto t : selection) {
            const auto& tri = mesh.triangles()[t];
            edgeSum += (mesh.vertices()[tri[1]] - mesh.vertices()[tri[0]]).length();
        }
        const double meanEdge = std::max(edgeSum / static_cast<double>(selection.size()), 1e-9);
        const double byPoints = std::sqrt(static_cast<double>(pts.size())) / 3.0;
        const double byEdges = std::max(spanU, spanV) / (2.0 * meanEdge);
        grid = std::clamp(static_cast<int>(std::max(byPoints, byEdges)), 10, 60);
    }
    const int nu = spanU >= spanV ? grid : std::max(4, static_cast<int>(std::lround(grid * spanU / spanV)));
    const int nv = spanV >= spanU ? grid : std::max(4, static_cast<int>(std::lround(grid * spanV / spanU)));
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
        report.warnings.push_back("The region covers little of its bounding rectangle (long, thin or curved in "
                                  "plan); the fit may be poor there. Select a more compact area or split it.");

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
        auto value = [&](int i, int j) { return heights[at(std::clamp(i, 0, nu - 1), std::clamp(j, 0, nv - 1))]; };
        for (int iter = 0; iter < 2000; ++iter) {
            const bool biharmonic = iter >= 500;
            for (int j = 0; j < nv; ++j) {
                for (int i = 0; i < nu; ++i) {
                    if (known[at(i, j)] != 0)
                        continue;
                    const double n4 = value(i - 1, j) + value(i + 1, j) + value(i, j - 1) + value(i, j + 1);
                    if (!biharmonic || i < 2 || j < 2 || i >= nu - 2 || j >= nv - 2) {
                        heights[at(i, j)] = 0.25 * n4;
                        continue;
                    }
                    const double diag =
                        value(i - 1, j - 1) + value(i + 1, j - 1) + value(i - 1, j + 1) + value(i + 1, j + 1);
                    const double far = value(i - 2, j) + value(i + 2, j) + value(i, j - 2) + value(i, j + 2);
                    heights[at(i, j)] = (8.0 * n4 - 2.0 * diag - far) / 20.0;
                }
            }
        }
    }

    TColgp_Array2OfPnt nodes(1, nu, 1, nv);
    for (int j = 0; j < nv; ++j) {
        for (int i = 0; i < nu; ++i) {
            const Vec3 p = frame.toWorld(u0 + i * hu, v0 + j * hv, heights[at(i, j)]);
            nodes.SetValue(i + 1, j + 1, gp_Pnt(p.x, p.y, p.z));
        }
    }

    // Smoothing (variational) mode, the most stable on noisy data in the CAD-66 spike.
    // When the tolerance cannot be reached the fit fails; retry with a looser one and say so.
    Handle(Geom_BSplineSurface) surface;
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
    report.uPoles = surface->NbUPoles();
    report.vPoles = surface->NbVPoles();

    // Trim to the outer outline of the selection; fall back to the whole fit rectangle.
    TopoDS_Face face;
    try {
        const auto outlines = selectionOutlines(mesh, adjacency, selection);
        if (!outlines.empty()) {
            const std::size_t samples = std::clamp<std::size_t>(outlines.front().size(), 16, 400);
            face = trimmedFace(surface, resampleLoop(outlines.front(), samples), options.tolerance);
            if (outlines.size() > 1 && !face.IsNull())
                report.warnings.push_back("Holes in the selection are not cut out of the surface yet.");
        }
    } catch (const Standard_Failure&) {
        face.Nullify();
    }
    report.trimmed = !face.IsNull();
    if (face.IsNull()) {
        face = BRepBuilderAPI_MakeFace(surface, 1e-6);
        report.warnings.push_back("Could not trim the surface to the selection outline; it covers the whole fit "
                                  "rectangle.");
    }

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
    const std::size_t stride = std::max<std::size_t>(1, pts.size() / 20000);
    double sum = 0.0;
    std::size_t count = 0;

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
    for (std::size_t i = 0; i < pts.size(); i += stride) {
        const auto l = frame.toLocal(pts[i]);
        double d = 0.0;
        const auto k = seeds.nearest(l[0], l[1], l[2], d);
        try {
            locate.Perform(gp_Pnt(pts[i].x, pts[i].y, pts[i].z), seedParams[k][0], seedParams[k][1]);
            double pu = seedParams[k][0], pv = seedParams[k][1];
            if (locate.IsDone()) {
                d = std::min(d, std::sqrt(locate.SquareDistance()));
                locate.Point().Parameter(pu, pv);
            }
            if (classifier && classifier->Perform(gp_Pnt2d(pu, pv)) == TopAbs_OUT) {
                report.outsideTrimMax = std::max(report.outsideTrimMax, boundaryDistance(pts[i]));
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

    // Surface -> mesh, sampled on the face's own triangulation (so only inside the trim).
    // Measured against the selection grown by two rings: the smoothed trim outline may
    // pass a fraction of a triangle outside the selected triangles, which is not a
    // deviation from the scanned surface.
    const core::TriangleSet reference = core::growSelection(adjacency, selection, 2);
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
        report.warnings.push_back("The smoothed outline leaves selected mesh up to " +
                                  std::to_string(report.outsideTrimMax) +
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
