// SPDX-License-Identifier: GPL-3.0-or-later
// mesh.*, part.convert_units, part.calibrate, part.align, part.transform, part.place.
#include "aerocad/commands/Session.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace aerocad;
using Catch::Approx;
using commands::Json;

namespace {

struct Tri {
    double v[9];
};

/// ASCII STL written to a unique temp file, removed on destruction.
class TempMesh {
public:
    explicit TempMesh(const std::vector<Tri>& tris)
    {
        std::random_device rd;
        m_path = std::filesystem::temp_directory_path() / ("aerocad_scan_" + std::to_string(rd()) + ".stl");
        std::ofstream out(m_path, std::ios::binary);
        out << "solid t\n";
        for (const auto& t : tris) {
            out << "facet normal 0 0 0\nouter loop\n";
            for (int i = 0; i < 3; ++i)
                out << "vertex " << t.v[3 * i] << ' ' << t.v[3 * i + 1] << ' ' << t.v[3 * i + 2] << '\n';
            out << "endloop\nendfacet\n";
        }
        out << "endsolid t\n";
    }
    ~TempMesh()
    {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }
    TempMesh(const TempMesh&) = delete;
    TempMesh& operator=(const TempMesh&) = delete;
    [[nodiscard]] std::string path() const { return m_path.string(); }

private:
    std::filesystem::path m_path;
};

/// Axis-aligned box [0,sx]x[0,sy]x[0,sz], outward normals; `skipTop` leaves the top open.
std::vector<Tri> box(double sx, double sy, double sz, bool skipTop = false)
{
    const double p[8][3] = {{0, 0, 0},  {sx, 0, 0},  {sx, sy, 0},  {0, sy, 0},
                            {0, 0, sz}, {sx, 0, sz}, {sx, sy, sz}, {0, sy, sz}};
    const int f[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                          {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
    std::vector<Tri> out;
    for (int i = 0; i < 12; ++i) {
        if (skipTop && (i == 2 || i == 3))
            continue;
        Tri t{};
        for (int k = 0; k < 3; ++k)
            for (int c = 0; c < 3; ++c)
                t.v[3 * k + c] = p[f[i][k]][c];
        out.push_back(t);
    }
    return out;
}

Json run(commands::Session& s, const std::string& name, const Json& params)
{
    const auto r = s.execute(name, params);
    INFO(name << ": " << r.error);
    REQUIRE(r.ok);
    return r.data;
}

std::uint64_t importBox(commands::Session& s, const TempMesh& file, const Json& extra = Json::object())
{
    Json params = extra;
    params["path"] = file.path();
    return run(s, "document.import", params)["id"].get<std::uint64_t>();
}

} // namespace

TEST_CASE("document.import scales from the given units", "[commands][scan]")
{
    const TempMesh file(box(1, 2, 3));
    commands::Session s;
    const auto r = run(s, "document.import", {{"path", file.path()}, {"units", "in"}});
    CHECK(r["units"] == "in");
    CHECK(r["bounds"]["size"][2].get<double>() == Approx(3 * 25.4));
    CHECK_FALSE(s.execute("document.import", {{"path", file.path()}, {"units", "parsec"}}).ok);
}

TEST_CASE("document.import uses a staged parse result", "[commands][scan]")
{
    const TempMesh file(box(1, 1, 1));
    commands::Session s;
    io::ImportResult staged;
    staged.format = io::FileFormat::Stl;
    staged.mesh = core::TriangleMesh({{0, 0, 0}, {5, 0, 0}, {0, 5, 0}}, {{0, 1, 2}});
    s.importStage().put(file.path(), std::move(staged));
    CHECK(run(s, "document.import", {{"path", file.path()}})["triangles"] == 1);
    // Consumed: a second import reads the file.
    CHECK(run(s, "document.import", {{"path", file.path()}})["triangles"] == 12);
}

TEST_CASE("mesh.stats and mesh.repair fill a hole and are undoable", "[commands][scan]")
{
    const TempMesh file(box(10, 10, 10, true));
    commands::Session s;
    const auto id = importBox(s, file);
    const auto stats = run(s, "mesh.stats", {{"id", id}});
    CHECK(stats["holes"] == 1);
    CHECK(stats["closed"] == false);

    const auto rep = run(s, "mesh.repair", {{"id", id}, {"maxHoleEdges", 10}});
    CHECK(rep["filledHoles"] == 1);
    CHECK(rep["stats"]["closed"] == true);
    CHECK(run(s, "mesh.stats", {{"id", id}})["holes"] == 0);

    run(s, "edit.undo", Json::object());
    CHECK(run(s, "mesh.stats", {{"id", id}})["holes"] == 1);

    CHECK_FALSE(s.execute("mesh.repair", {{"id", 999}}).ok);
}

TEST_CASE("mesh.flip_normals and mesh.decimate", "[commands][scan]")
{
    const TempMesh file(box(10, 10, 10));
    commands::Session s;
    const auto id = importBox(s, file);
    run(s, "mesh.flip_normals", {{"id", id}});
    // Repair turns a closed inside-out shell back outwards.
    CHECK(run(s, "mesh.repair", {{"id", id}})["flippedTriangles"] == 12);
    const auto d = run(s, "mesh.decimate", {{"id", id}, {"targetTriangles", 100}});
    CHECK(d["triangles"] == 12); // already below target
}

TEST_CASE("part.convert_units and part.calibrate rescale", "[commands][scan]")
{
    const TempMesh file(box(2, 1, 1));
    commands::Session s;
    const auto id = importBox(s, file);
    const auto c = run(s, "part.convert_units", {{"id", id}, {"from", "cm"}});
    CHECK(c["factor"].get<double>() == Approx(10));
    CHECK(c["bounds"]["size"][0].get<double>() == Approx(20));

    // Known dimension: the 20 mm edge is really 50 mm.
    const auto cal =
        run(s, "part.calibrate", {{"id", id}, {"a", {0, 0, 0}}, {"b", {20, 0, 0}}, {"distance", 5}, {"units", "cm"}});
    CHECK(cal["factor"].get<double>() == Approx(2.5));
    CHECK(cal["bounds"]["min"][0].get<double>() == Approx(0)); // a stays fixed
    CHECK(cal["bounds"]["size"][0].get<double>() == Approx(50));
    CHECK_FALSE(s.execute("part.calibrate", {{"id", id}, {"a", {1, 1, 1}}, {"b", {1, 1, 1}}, {"distance", 5}}).ok);
}

TEST_CASE("part.transform scales, rotates and moves in one undo step", "[commands][scan]")
{
    const TempMesh file(box(10, 20, 30));
    commands::Session s;
    const auto id = importBox(s, file);
    // 200 % about the centre (5, 10, 15), 90 degrees about Z, then 10 mm up.
    const auto r = run(s, "part.transform",
                       {{"id", id}, {"scale", 2.0}, {"axis", {0, 0, 1}}, {"angle", 90}, {"translate", {0, 0, 10}}});
    const auto& b = r["bounds"];
    CHECK(b["size"][0].get<double>() == Approx(40));
    CHECK(b["size"][1].get<double>() == Approx(20));
    CHECK(b["size"][2].get<double>() == Approx(60));
    CHECK(b["min"][0].get<double>() == Approx(5 - 20));
    CHECK(b["min"][2].get<double>() == Approx(15 - 30 + 10));
    CHECK_FALSE(s.execute("part.transform", {{"id", id}, {"scale", -1}}).ok);
    run(s, "edit.undo", Json::object());
    const auto back = run(s, "document.parts", Json::object())["parts"][0]["bounds"];
    CHECK(back["size"][0].get<double>() == Approx(10));
    CHECK(back["min"][2].get<double>() == Approx(0));
}

TEST_CASE("part.face_orientation lays a part on a face and turns a face to the front", "[commands][scan]")
{
    const TempMesh file(box(10, 20, 30));
    commands::Session s;
    const auto id = importBox(s, file);
    // Lay on the +X face: X points down, the box lies on the ground.
    auto o = run(s, "part.face_orientation", {{"id", id}, {"point", {10, 10, 15}}, {"mode", "down"}});
    CHECK(o["normal"][0].get<double>() == Approx(1));
    auto applied = run(s, "part.transform",
                       {{"id", id}, {"center", o["center"]}, {"axis", o["axis"]}, {"angle", o["angle"]},
                        {"translate", o["translate"]}});
    auto b = applied["bounds"];
    CHECK(b["size"][0].get<double>() == Approx(30));
    CHECK(b["size"][2].get<double>() == Approx(10));
    CHECK(b["min"][2].get<double>() == Approx(0).margin(1e-9));
    const double height = b["min"][2].get<double>();

    // Face Front with the face that is at +Y now: it ends up at the front (min Y), height kept.
    const double yMax = b["max"][1].get<double>();
    const double cx = (b["min"][0].get<double>() + b["max"][0].get<double>()) / 2;
    o = run(s, "part.face_orientation", {{"id", id}, {"point", {cx, yMax, 5}}, {"mode", "front"}});
    applied = run(s, "part.transform",
                  {{"id", id}, {"center", o["center"]}, {"axis", o["axis"]}, {"angle", o["angle"]},
                   {"translate", o["translate"]}});
    b = applied["bounds"];
    CHECK(b["size"][1].get<double>() == Approx(20));
    CHECK(b["min"][2].get<double>() == Approx(height).margin(1e-9));
    // The picked point (on the +Y face before) is now on the -Y side: half a turn about Z
    // through the centre moves yMax to the minimum.
    CHECK(b["min"][1].get<double>() == Approx(2 * o["center"][1].get<double>() - yMax));

    // A pending transform is taken into account: lay down on top of a 90 degree turn.
    o = run(s, "part.face_orientation",
            {{"id", id}, {"point", {cx, yMax, 5}}, {"mode", "down"}, {"axis", {0, 0, 1}}, {"angle", 90}});
    CHECK(o["angle"].get<double>() > 0);
    CHECK_FALSE(s.execute("part.face_orientation", {{"id", 999}, {"point", {0, 0, 0}}, {"mode", "down"}}).ok);
}

TEST_CASE("part.align puts a tilted face on the XY plane", "[commands][scan]")
{
    const TempMesh file(box(10, 20, 30));
    commands::Session s;
    const auto id = importBox(s, file);
    // Tilt by 30 degrees about X, then move away.
    run(s, "part.transform",
        {{"id", id}, {"axis", {1, 0, 0}}, {"angle", 30}, {"center", {0, 0, 0}}, {"translate", {5, 6, 7}}});

    // Points on the (tilted) +Y face y=20 of the original box.
    const double a = 30.0 * 3.14159265358979323846 / 180.0;
    auto rot = [&](double x, double y, double z) {
        return Json::array({x + 5, y * std::cos(a) - z * std::sin(a) + 6, y * std::sin(a) + z * std::cos(a) + 7});
    };
    const Json pts = Json::array({rot(0, 20, 0), rot(10, 20, 0), rot(10, 20, 30), rot(0, 20, 30)});
    const auto r = run(s, "part.align", {{"id", id}, {"points", pts}, {"flip", false}, {"origin", rot(0, 20, 0)}});
    CHECK(r["fitRms"].get<double>() == Approx(0).margin(1e-6));
    // The face is now z = 0 and the box extends 20 mm to one side of it.
    const auto& b = r["bounds"];
    const double zmin = b["min"][2].get<double>();
    const double zmax = b["max"][2].get<double>();
    CHECK(std::min(std::abs(zmin), std::abs(zmax)) == Approx(0).margin(1e-6));
    CHECK(zmax - zmin == Approx(20));

    // Triangle region: the bottom face triangles of the box (indices of its two faces).
    const TempMesh file2(box(10, 20, 30));
    const auto id2 = importBox(s, file2);
    run(s, "part.transform", {{"id", id2}, {"axis", {0, 1, 1}}, {"angle", 50}});
    const auto r2 = run(s, "part.align", {{"id", id2}, {"triangles", {0, 1}}, {"flip", true}});
    // The bottom face's outward normal points away from the body; flipped, it maps to +Z and the
    // body ends up standing on the XY plane.
    CHECK(r2["bounds"]["min"][2].get<double>() == Approx(0).margin(1e-6));
    CHECK(r2["bounds"]["size"][2].get<double>() == Approx(30));

    run(s, "part.place", {{"id", id2}});
    const auto parts = run(s, "document.parts", Json::object())["parts"];
    CHECK(parts[1]["bounds"]["min"][2].get<double>() == Approx(0).margin(1e-9));

    CHECK_FALSE(s.execute("part.align", {{"id", id}}).ok);
    CHECK_FALSE(s.execute("part.align", {{"id", id}, {"points", {{0, 0, 0}, {1, 1, 1}, {2, 2, 2}}}}).ok);
}
