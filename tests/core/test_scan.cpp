// SPDX-License-Identifier: GPL-3.0-or-later
// Scan helpers: units, alignment, repair, decimation.
#include "aerocad/core/Align.hpp"
#include "aerocad/core/MeshRepair.hpp"
#include "aerocad/core/MeshSimplify.hpp"
#include "aerocad/core/Units.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
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
    CHECK(s.loopedBoundaryEdges == 3);
}

TEST_CASE("meshStats separates boundary edges that no loop closes", "[core][scan]")
{
    // Tetrahedron without its bottom and with one side doubled: edge 0-1 is non-manifold,
    // so the bottom's boundary is the open chain 1-2-0 and no loop closes it.
    const std::vector<Vec3> v{{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {0, 0, 10}};
    const std::vector<TriangleMesh::Triangle> t{{0, 1, 3}, {1, 2, 3}, {2, 0, 3}, {0, 1, 3}};
    const MeshStats s = meshStats(TriangleMesh(v, t));
    CHECK(s.boundaryEdges == 2);
    CHECK(s.holes == 0);
    CHECK(s.loopedBoundaryEdges == 0);
    CHECK(s.nonManifoldEdges > 0);
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
    CHECK(meshStats(open).largestBoundaryEdges == 4);
    CHECK(meshStats(c).largestBoundaryEdges == 0);
    RepairOptions fill;
    fill.maxHoleEdges = 3;
    RepairReport large;
    CHECK(meshStats(repairMesh(open, fill, &large)).holes == 1); // too large to fill
    CHECK(large.largeBoundaries == 1);
    CHECK(large.filledHoles == 0);
    fill.maxHoleEdges = 8;
    RepairReport frep;
    const TriangleMesh filled = repairMesh(open, fill, &frep);
    CHECK(frep.filledHoles == 1);
    CHECK(frep.largeBoundaries == 0);
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

TEST_CASE("decimateToTarget never collapses a mesh to nothing", "[core][scan]")
{
    // Many tiny separate triangles along a line: clustering merges each one away.
    TriangleMesh m;
    for (std::uint32_t i = 0; i < 2000; ++i) {
        const double x = i * 3.0;
        const auto a = m.addVertex({x, 0, 0});
        const auto b = m.addVertex({x + 1, 0, 0});
        const auto c = m.addVertex({x, 1, 0});
        m.addTriangle(a, b, c);
    }
    const TriangleMesh d = decimateToTarget(m, 1000);
    CHECK_FALSE(d.isEmpty());
}

TEST_CASE("axisAngle gives back the rotation, also half turns", "[core][align]")
{
    for (const auto& [axis, angle] : std::vector<std::pair<Vec3, double>>{
             {{0, 0, 1}, 0.3}, {{1, 2, 3}, 2.0}, {{1, 0, 0}, std::numbers::pi}, {{0.3, -1, 0.2}, std::numbers::pi}}) {
        const Mat3 r = Mat3::rotation(axis, angle);
        const AxisAngle aa = axisAngle(r);
        const Mat3 back = Mat3::rotation(aa.axis, aa.angle);
        for (int i = 0; i < 9; ++i)
            CHECK(back.m[static_cast<std::size_t>(i)] == Approx(r.m[static_cast<std::size_t>(i)]).margin(1e-9));
    }
    CHECK(axisAngle(Mat3{}).angle == 0.0);
}

TEST_CASE("patchNormal: a face, a face next to a bevel, a noisy tilted scan", "[core][align]")
{
    const TriangleMesh box = cube();
    const Vec3 top = patchNormal(box, {0.5, 0.5, 1}, 2, 0.2);
    CHECK(top.z == Approx(1).margin(1e-9));

    // 100 mm square face (two triangles) with a 1 mm 45 degree bevel along x = 100; click
    // 1 mm from the bevel.
    std::vector<Vec3> v{{0, 0, 0}, {100, 0, 0}, {100, 100, 0}, {0, 100, 0}, {101, 0, -1}, {101, 100, -1}};
    std::vector<TriangleMesh::Triangle> t{{0, 1, 2}, {0, 2, 3}, {1, 4, 5}, {1, 5, 2}};
    const TriangleMesh bevel(std::move(v), std::move(t));
    REQUIRE(bevel.faceNormal(2).x > 0.5);
    const Vec3 n = patchNormal(bevel, {99, 50, 0}, 0, 3.0);
    CHECK(std::acos(std::clamp(n.z, -1.0, 1.0)) * 180 / std::numbers::pi < 3.0);

    // 60 x 60 mm grid, 3 mm spacing, tilted 10 degrees about X, +-0.05 mm noise.
    const Mat3 tilt = Mat3::rotation({1, 0, 0}, 10 * std::numbers::pi / 180);
    std::vector<Vec3> gv;
    std::vector<TriangleMesh::Triangle> gt;
    const int n1 = 21;
    for (int j = 0; j < n1; ++j)
        for (int i = 0; i < n1; ++i) {
            const double noise = 0.05 * std::sin(i * 12.9898 + j * 78.233) * std::cos(i * 4.1 + j * 1.7);
            gv.push_back(tilt * Vec3{i * 3.0, j * 3.0, noise});
        }
    for (int j = 0; j + 1 < n1; ++j)
        for (int i = 0; i + 1 < n1; ++i) {
            const auto k = static_cast<std::uint32_t>(j * n1 + i);
            gt.push_back({k, k + 1, k + n1 + 1});
            gt.push_back({k, k + n1 + 1, k + n1});
        }
    const TriangleMesh scan(std::move(gv), std::move(gt));
    const Vec3 want = tilt * Vec3{0, 0, 1};
    const Vec3 got = patchNormal(scan, tilt * Vec3{30, 30, 0}, 2 * (10 * 20 + 10), 6.0);
    CHECK(std::acos(std::clamp(got.dot(want), -1.0, 1.0)) * 180 / std::numbers::pi < 1.0);
}

TEST_CASE("faceTurn lays a face down or turns it about Z to the front", "[core][align]")
{
    const Vec3 n = Vec3{1, 2, 0.5}.normalized();
    const Vec3 down = faceTurn(n, FaceTarget::Down) * n;
    CHECK(down.z == Approx(-1).margin(1e-9));
    const Mat3 front = faceTurn(n, FaceTarget::Front);
    const Vec3 f = front * n;
    CHECK(f.x == Approx(0).margin(1e-9));
    CHECK(f.y < 0);
    CHECK(f.z == Approx(n.z).margin(1e-9)); // about Z only
    const Vec3 up{0, 0.1, 1};
    CHECK((faceTurn(up, FaceTarget::Front) * up.normalized()).y == Approx(-1).margin(1e-9));
}


TEST_CASE("smoothCornerNormals: one unit normal per corner, also where flipped faces cancel", "[core][scan]")
{
    // A flat grid whose two triangles in every square face opposite ways (Codex's case on
    // PR #105): around many vertices the face normals sum to zero, before and after
    // simplification.
    std::vector<Vec3> v;
    std::vector<TriangleMesh::Triangle> t;
    const int n = 64;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            v.push_back({double(i), double(j), 0.0});
    auto idx = [&](int i, int j) { return static_cast<std::uint32_t>(j * (n + 1) + i); };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            t.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
            t.push_back({idx(i, j), idx(i, j + 1), idx(i + 1, j + 1)}); // turned the other way
        }
    const TriangleMesh grid(std::move(v), std::move(t));
    for (const TriangleMesh& mesh : {grid, decimateToTarget(grid, 2000)}) {
        REQUIRE(mesh.triangleCount() > 0);
        const auto normals = smoothCornerNormals(mesh);
        REQUIRE(normals.size() == mesh.triangleCount() * 3);
        std::size_t corner = 0;
        for (const auto& tri : mesh.triangles()) {
            const Vec3 face = (mesh.vertices()[tri[1]] - mesh.vertices()[tri[0]])
                                  .cross(mesh.vertices()[tri[2]] - mesh.vertices()[tri[0]]);
            for (int k = 0; k < 3; ++k) {
                const Vec3& nk = normals[corner++];
                CHECK(nk.length() == Catch::Approx(1.0));
                // Flat: along z, on the side of its own triangle.
                CHECK(std::abs(nk.z) == Catch::Approx(1.0));
                CHECK(nk.dot(face) > 0.0);
            }
        }
    }
}

TEST_CASE("smoothCornerNormals averages across triangles on a curved surface", "[core][scan]")
{
    // A cylinder of radius 10 around z, 2 degree facets: at a corner a facet's normal is 1
    // degree off radial; the averaged one is within half of that, also with every third
    // triangle flipped.
    std::vector<Vec3> v;
    std::vector<TriangleMesh::Triangle> t;
    const int seg = 180;
    for (int k = 0; k < seg; ++k) {
        const double a = 2.0 * std::numbers::pi * k / seg;
        v.push_back({10.0 * std::cos(a), 10.0 * std::sin(a), 0.0});
        v.push_back({10.0 * std::cos(a), 10.0 * std::sin(a), 5.0});
    }
    for (int k = 0; k < seg; ++k) {
        const auto a0 = static_cast<std::uint32_t>(2 * k), a1 = a0 + 1;
        const auto b0 = static_cast<std::uint32_t>(2 * ((k + 1) % seg)), b1 = b0 + 1;
        t.push_back(k % 3 == 0 ? TriangleMesh::Triangle{a0, a1, b0} : TriangleMesh::Triangle{a0, b0, a1});
        t.push_back({b0, b1, a1});
    }
    const TriangleMesh cylinder(std::move(v), std::move(t));
    const auto normals = smoothCornerNormals(cylinder);
    std::size_t corner = 0;
    for (const auto& tri : cylinder.triangles())
        for (const std::uint32_t vi : tri) {
            const Vec3& p = cylinder.vertices()[vi];
            const Vec3 radial = Vec3{p.x, p.y, 0.0}.normalized();
            CHECK(std::abs(normals[corner++].dot(radial)) > std::cos(0.5 * std::numbers::pi / 180.0));
        }
}
