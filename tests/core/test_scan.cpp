// SPDX-License-Identifier: GPL-3.0-or-later
// Scan helpers: units, alignment, repair, decimation.
#include "aerocad/core/Align.hpp"
#include "aerocad/core/MeshRepair.hpp"
#include "aerocad/core/MeshSimplify.hpp"
#include "aerocad/core/Units.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <vector>

using namespace aerocad::core;
using Catch::Approx;

namespace {

/// Closed unit cube [0,1]^3 with outward normals (12 triangles, 8 vertices).
TriangleMesh cube()
{
    std::vector<Vec3> v{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    std::vector<TriangleMesh::Triangle> t{{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                                          {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
    return {std::move(v), std::move(t)};
}

/// UV sphere of radius r with n segments (closed, outward).
TriangleMesh sphere(double r, int n)
{
    TriangleMesh m;
    const auto rings = n;
    const auto segs = 2 * n;
    for (int i = 0; i <= rings; ++i) {
        const double th = std::numbers::pi * i / rings;
        for (int j = 0; j < segs; ++j) {
            const double ph = 2 * std::numbers::pi * j / segs;
            m.addVertex({r * std::sin(th) * std::cos(ph), r * std::sin(th) * std::sin(ph), r * std::cos(th)});
        }
    }
    auto id = [&](int i, int j) { return static_cast<std::uint32_t>(i * segs + (j % segs)); };
    MeshBuilder b;
    for (int i = 0; i < rings; ++i) {
        for (int j = 0; j < segs; ++j) {
            const auto& vs = m.vertices();
            b.addTriangle(vs[id(i, j)], vs[id(i + 1, j)], vs[id(i + 1, j + 1)]);
            b.addTriangle(vs[id(i, j)], vs[id(i + 1, j + 1)], vs[id(i, j + 1)]);
        }
    }
    return b.take();
}

double signedVolume(const TriangleMesh& m)
{
    double v = 0;
    for (const auto& t : m.triangles())
        v += m.vertices()[t[0]].dot(m.vertices()[t[1]].cross(m.vertices()[t[2]]));
    return v / 6.0;
}

} // namespace

TEST_CASE("Length units convert to millimetres and parse", "[core][scan]")
{
    CHECK(toMillimetres(LengthUnit::Inch) == Approx(25.4));
    CHECK(toMillimetres(LengthUnit::Metre) == Approx(1000));
    CHECK(parseLengthUnit("MM") == LengthUnit::Millimetre);
    CHECK(parseLengthUnit("inches") == LengthUnit::Inch);
    CHECK(parseLengthUnit("µm") == LengthUnit::Micrometre);
    CHECK_FALSE(parseLengthUnit("parsec").has_value());
    for (const auto u : allLengthUnits())
        CHECK(parseLengthUnit(unitSymbol(u)) == u);
}

TEST_CASE("fitPlane finds the plane of noise-free points", "[core][scan]")
{
    std::vector<Vec3> pts;
    const Vec3 n = Vec3{1, 2, 3}.normalized();
    const Vec3 u = n.cross(Vec3{0, 0, 1}).normalized();
    const Vec3 w = n.cross(u);
    for (int i = 0; i < 10; ++i)
        for (int j = 0; j < 10; ++j)
            pts.push_back(Vec3{5, 5, 5} + u * i + w * (j * 0.5));
    const auto plane = fitPlane(pts);
    REQUIRE(plane);
    CHECK(std::abs(plane->normal.dot(n)) == Approx(1.0));
    CHECK(plane->rms == Approx(0.0).margin(1e-6));

    CHECK_FALSE(fitPlane({{0, 0, 0}, {1, 1, 1}, {2, 2, 2}}).has_value());
    CHECK_FALSE(fitPlane({{0, 0, 0}, {1, 0, 0}}).has_value());
}

TEST_CASE("fitPlaneToTriangles weights by area, not by vertex count", "[core][scan]")
{
    // A 40 x 10 strip on z = 0: the left half finely meshed (many vertices), the right
    // half two big triangles. Vertex-based fitting would pull the centre to the left.
    std::vector<Vec3> v;
    std::vector<TriangleMesh::Triangle> t;
    const int n = 20;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            v.push_back({20.0 * i / n, 10.0 * j / n, 0.0});
    auto idx = [](int i, int j) { return static_cast<std::uint32_t>(j * (n + 1) + i); };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            t.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
            t.push_back({idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)});
        }
    const auto a = static_cast<std::uint32_t>(v.size());
    v.push_back({20, 0, 0});
    v.push_back({40, 0, 0});
    v.push_back({40, 10, 0});
    v.push_back({20, 10, 0});
    t.push_back({a, a + 1, a + 2});
    t.push_back({a, a + 2, a + 3});
    const TriangleMesh mesh(std::move(v), std::move(t));
    std::vector<std::size_t> all(mesh.triangleCount());
    for (std::size_t i = 0; i < all.size(); ++i)
        all[i] = i;
    const auto plane = fitPlaneToTriangles(mesh, all);
    REQUIRE(plane);
    CHECK(std::abs(plane->normal.z) == Approx(1.0));
    CHECK(plane->origin.x == Approx(20.0));
    CHECK(plane->origin.y == Approx(5.0));
    CHECK(std::abs(plane->major.x) == Approx(1.0));
    CHECK(plane->rms == Approx(0.0).margin(1e-9));
    CHECK_FALSE(fitPlaneToTriangles(mesh, {}).has_value());
}

TEST_CASE("frameToWorld maps a tilted plane onto XY", "[core][scan]")
{
    const Vec3 origin{10, -4, 7};
    const Vec3 z = Vec3{0, 1, 1}.normalized();
    const auto tf = frameToWorld(origin, z, Vec3{1, 0, 0});
    REQUIRE(tf);
    const Vec3 o = tf->apply(origin);
    CHECK(o.length() == Approx(0).margin(1e-12));
    const Vec3 zz = tf->linear * z;
    CHECK(zz.z == Approx(1.0));
    const Vec3 xx = tf->linear * Vec3{1, 0, 0};
    CHECK(xx.x == Approx(1.0));
    CHECK(tf->linear.determinant() == Approx(1.0));

    // Without hint: still a proper rotation that sends z to +Z.
    const auto tf2 = frameToWorld({}, Vec3{0, 0, -1});
    REQUIRE(tf2);
    CHECK((tf2->linear * Vec3{0, 0, -1}).z == Approx(1.0));
    CHECK(tf2->linear.determinant() == Approx(1.0));
    CHECK_FALSE(frameToWorld({}, {}).has_value());
}

TEST_CASE("transformMesh keeps outward normals under mirroring", "[core][scan]")
{
    TriangleMesh m = cube();
    const double v0 = signedVolume(m);
    transformMesh(m, Affine{Mat3::fromRows({-1, 0, 0}, {0, 1, 0}, {0, 0, 1}), {}});
    CHECK(signedVolume(m) == Approx(v0));
    transformMesh(m, Affine{Mat3::rotation({0, 0, 1}, std::numbers::pi / 2), {1, 2, 3}});
    CHECK(signedVolume(m) == Approx(v0));
    CHECK(m.bounds().min.z == Approx(3));
}

TEST_CASE("meshStats reports holes, duplicates and flipped faces", "[core][scan]")
{
    const MeshStats closed = meshStats(cube());
    CHECK(closed.boundaryEdges == 0);
    CHECK(closed.holes == 0);
    CHECK(closed.components == 1);
    CHECK(closed.inconsistentEdges == 0);

    auto v = cube().vertices();
    auto t = cube().triangles();
    t.erase(t.begin() + 2);      // open the top: one 3-edge hole
    t.push_back(t[0]);           // duplicate
    std::swap(t[5][1], t[5][2]); // flip one face
    v.push_back({9, 9, 9});      // unused
    const MeshStats s = meshStats(TriangleMesh(v, t));
    CHECK(s.holes == 1);
    CHECK(s.boundaryEdges == 3);
    CHECK(s.duplicateTriangles == 1);
    CHECK(s.inconsistentEdges > 0);
    CHECK(s.unusedVertices == 1);
}

TEST_CASE("repairMesh welds, dedupes, orients and fills holes", "[core][scan]")
{
    // Triangle soup of the cube with one face flipped, one duplicate and slightly
    // jittered copies of the vertices.
    const TriangleMesh c = cube();
    TriangleMesh soup;
    int k = 0;
    for (const auto& t : c.triangles()) {
        std::array<std::uint32_t, 3> ids{};
        for (int i = 0; i < 3; ++i) {
            const double jitter = 1e-5 * ((k++ % 3) - 1);
            ids[static_cast<std::size_t>(i)] =
                soup.addVertex(c.vertices()[t[static_cast<std::size_t>(i)]] + Vec3{jitter, 0, 0});
        }
        soup.addTriangle(ids[0], ids[1], ids[2]);
    }
    {
        auto tris = soup.triangles();
        std::swap(tris[3][1], tris[3][2]);
        tris.push_back(tris[0]);
        soup = TriangleMesh(soup.vertices(), tris);
    }
    // Turn every face inside out as well: orientation must pick outward normals.
    flipNormals(soup);

    RepairOptions opt;
    opt.mergeDistance = 1e-3;
    RepairReport rep;
    const TriangleMesh fixed = repairMesh(soup, opt, &rep);
    CHECK(fixed.vertexCount() == 8);
    CHECK(fixed.triangleCount() == 12);
    CHECK(rep.removedDuplicates == 1);
    CHECK(rep.flippedTriangles > 0);
    const MeshStats s = meshStats(fixed);
    CHECK(s.holes == 0);
    CHECK(s.inconsistentEdges == 0);
    CHECK(signedVolume(fixed) == Approx(1.0).epsilon(1e-3));

    // Hole filling: remove the two top triangles -> one 4-edge hole.
    auto tris = c.triangles();
    tris.erase(tris.begin() + 2, tris.begin() + 4);
    const TriangleMesh open(c.vertices(), tris);
    REQUIRE(meshStats(open).holes == 1);
    RepairOptions fill;
    fill.maxHoleEdges = 3;
    CHECK(meshStats(repairMesh(open, fill)).holes == 1); // too large to fill
    fill.maxHoleEdges = 8;
    RepairReport frep;
    const TriangleMesh filled = repairMesh(open, fill, &frep);
    CHECK(frep.filledHoles == 1);
    CHECK(frep.addedTriangles == 2); // ear clipping: a square needs two triangles
    const MeshStats fs = meshStats(filled);
    CHECK(fs.holes == 0);
    CHECK(fs.inconsistentEdges == 0);
    CHECK(signedVolume(filled) == Approx(1.0));
}

TEST_CASE("decimateToTarget reduces a dense sphere and keeps its shape", "[core][scan]")
{
    const TriangleMesh s = sphere(50.0, 200); // ~160k triangles
    REQUIRE(s.triangleCount() > 100000);
    const TriangleMesh d = decimateToTarget(s, 10000);
    CHECK(d.triangleCount() <= 10000);
    CHECK(d.triangleCount() > 2000);
    CHECK(d.isValid());
    const auto b0 = s.bounds();
    const auto b1 = d.bounds();
    CHECK(b1.size().x == Approx(b0.size().x).epsilon(0.05));
    for (const auto& p : d.vertices())
        CHECK(p.length() == Approx(50.0).epsilon(0.03));
    // Small meshes are returned unchanged.
    CHECK(decimateToTarget(cube(), 100).triangleCount() == 12);
}

TEST_CASE("repairMesh closes an inside-out open shell facing outwards", "[core][scan]")
{
    const TriangleMesh c = cube();
    auto tris = c.triangles();
    tris.erase(tris.begin() + 2, tris.begin() + 4); // open top
    TriangleMesh open(c.vertices(), tris);
    flipNormals(open);
    RepairOptions opt;
    opt.maxHoleEdges = 8;
    const TriangleMesh fixed = repairMesh(open, opt);
    CHECK(meshStats(fixed).boundaryEdges == 0);
    CHECK(signedVolume(fixed) == Approx(1.0));
}

TEST_CASE("holes along flipped rim faces are still counted", "[core][scan]")
{
    const TriangleMesh c = cube();
    auto tris = c.triangles();
    tris.erase(tris.begin() + 2, tris.begin() + 4); // open top
    std::swap(tris[4][1], tris[4][2]);              // flip a side triangle on the rim
    const MeshStats s = meshStats(TriangleMesh(c.vertices(), tris));
    CHECK(s.holes == 1);
}

TEST_CASE("concave holes are filled by ear clipping without overlap", "[core][scan]")
{
    // Flat U-shaped ring (outer U minus nothing): a planar strip whose outline is a concave
    // octagon. Build a closed U-shaped prism and remove its top cap: the hole is the U.
    const std::vector<std::array<double, 2>> outline{{0, 0}, {3, 0}, {3, 3}, {2, 3}, {2, 1}, {1, 1}, {1, 3}, {0, 3}};
    TriangleMesh m;
    for (const auto& p : outline)
        m.addVertex({p[0], p[1], 0});
    for (const auto& p : outline)
        m.addVertex({p[0], p[1], 1});
    const auto n = static_cast<std::uint32_t>(outline.size());
    // Sides (outline is counter-clockwise seen from +Z, so outward normals use this order).
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t j = (i + 1) % n;
        m.addTriangle(i, j, j + n);
        m.addTriangle(i, j + n, i + n);
    }
    // Bottom cap (facing -Z) as rectangles of the U.
    m.addTriangle(0, 5, 4); // fan-free manual triangulation of the bottom U
    m.addTriangle(0, 4, 1);
    m.addTriangle(1, 4, 3);
    m.addTriangle(1, 3, 2);
    m.addTriangle(0, 7, 6);
    m.addTriangle(0, 6, 5);
    REQUIRE(meshStats(m).holes == 1);

    RepairOptions opt;
    opt.maxHoleEdges = 8;
    opt.fixOrientation = false;
    RepairReport rep;
    const TriangleMesh filled = repairMesh(m, opt, &rep);
    CHECK(rep.filledHoles == 1);
    CHECK(rep.addedTriangles == 6);
    // Area of the new cap equals the U area (7) and every cap triangle faces +Z.
    double capArea = 0;
    for (std::size_t t = m.triangleCount(); t < filled.triangleCount(); ++t) {
        const auto& tri = filled.triangles()[t];
        const Vec3 nrm = (filled.vertices()[tri[1]] - filled.vertices()[tri[0]])
                             .cross(filled.vertices()[tri[2]] - filled.vertices()[tri[0]]);
        CHECK(nrm.z > 0);
        capArea += 0.5 * nrm.length();
    }
    CHECK(capArea == Approx(7.0));
    CHECK(meshStats(filled).boundaryEdges == 0);
    CHECK(signedVolume(filled) == Approx(7.0));
}
