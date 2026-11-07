// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end test (CAD-23): import -> region selection -> base surface -> modelling -> export,
// then save, reopen, change a parameter and export again (CAD-28). Runs on every file of the
// test model set (CAD-22, tests/data/models) through the command layer only, the same calls
// the UI and an MCP client make.
#include "aerocad/commands/Session.hpp"
#include "aerocad/core/Path.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <filesystem>
#include <numbers>
#include <random>
#include <string>

using namespace aerocad;
using Catch::Approx;
using commands::Json;
namespace fs = std::filesystem;

namespace {

Json run(commands::Session& s, const std::string& name, const Json& params = Json::object())
{
    const auto r = s.execute(name, params);
    INFO(name << " " << params.dump() << " -> " << r.error);
    REQUIRE(r.ok);
    return r.data;
}

class TempDir {
public:
    TempDir()
    {
        std::random_device rd;
        m_path = fs::temp_directory_path() / ("aerocad_e2e_" + std::to_string(rd()));
        fs::create_directories(m_path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] std::string file(const std::string& name) const { return core::pathToUtf8(m_path / name); }

private:
    fs::path m_path;
};

std::string modelFile(const std::string& name)
{
    return std::string(AEROCAD_TEST_DATA_DIR) + "/models/" + name;
}

Json partById(commands::Session& s, std::uint64_t id)
{
    const Json parts = run(s, "document.parts");
    for (const auto& p : parts.at("parts"))
        if (p.at("id").get<std::uint64_t>() == id)
            return p;
    FAIL("no part " << id);
    return {};
}

/// Body `id` from timeline.list (volume, partId, ...).
Json body(commands::Session& s, std::uint64_t id)
{
    const Json timeline = run(s, "timeline.list");
    for (const auto& b : timeline.at("bodies"))
        if (b.at("id").get<std::uint64_t>() == id)
            return b;
    FAIL("no body " << id);
    return {};
}

/// Imports an exported file into a fresh session and returns its part.check result.
Json reimportCheck(const std::string& path)
{
    commands::Session s;
    const Json part = run(s, "document.import", {{"path", path}});
    return run(s, "part.check", {{"id", part.at("id")}});
}

} // namespace

TEST_CASE("Test model set: every format imports as the same 240 mm dome", "[e2e][io]")
{
    const std::string file = GENERATE("dome.step", "dome.stl", "dome.3mf", "dome.obj");
    INFO(file);
    commands::Session s;
    const Json part = run(s, "document.import", {{"path", modelFile(file)}});
    CHECK(part.at("triangles").get<std::size_t>() > 1000);
    const Json size = run(s, "document.bounds").at("bounds").at("size");
    CHECK(size[0].get<double>() == Approx(240).margin(1));
    CHECK(size[1].get<double>() == Approx(240).margin(1));
    CHECK(size[2].get<double>() == Approx(45.7).margin(1));

    const Json check = run(s, "part.check", {{"id", part.at("id")}});
    CHECK(check.at("watertight").get<bool>());
    // Revolved profile: 1.46e6 mm^3 exact, the tessellation is slightly smaller.
    CHECK(check.at("volume").get<double>() == Approx(1.4598e6).epsilon(0.01));
}

TEST_CASE("Workflow: import, select, fit, offset, thicken, cut, export, save, reopen, re-export", "[e2e][commands]")
{
    const std::string file = GENERATE("dome.step", "dome.stl", "dome.3mf", "dome.obj");
    INFO(file);
    TempDir dir;

    commands::Session s;
    const Json imported = run(s, "document.import", {{"path", modelFile(file)}});
    const auto scanId = imported.at("id").get<std::uint64_t>();
    const double top = imported.at("bounds").at("max")[2].get<double>();

    // 1. Region selection: a brush stroke on the crown of the dome.
    const Json selected =
        run(s, "selection.brush", {{"id", scanId}, {"points", Json::array({{0, 0, top}})}, {"radius", 45}});
    INFO(selected.dump());
    REQUIRE(partById(s, scanId).at("selectedTriangles").get<std::size_t>() > 20);

    // 2. Base surface fitted to the selection, then a 2 mm clearance offset. The model
    //    meshes are coarse (about 2000 triangles), so the fit stays within 0.5 mm rather
    //    than the 0.25 mm default.
    const Json fitted = run(s, "surface.fit_selection", {{"id", scanId}, {"name", "Base"}, {"tolerance", 0.5}});
    INFO(fitted.dump());
    CHECK(fitted.at("hasBrep").get<bool>());
    CHECK(fitted.at("fit").at("withinTolerance").get<bool>());
    const Json offset = run(s, "surface.offset", {{"id", fitted.at("id")}, {"distance", 2}, {"name", "Clearance"}});
    CHECK(offset.at("bounds").at("max")[2].get<double>() > top + 1);

    // 3. Modelling: a 3 mm shell on the clearance surface, turned into a timeline body,
    //    with a mounting hole whose diameter is a user parameter.
    const Json solid = run(s, "surface.thicken", {{"id", offset.at("id")}, {"thickness", 3}, {"name", "Cover"}});
    const double shellVolume = solid.at("volume").get<double>();
    CHECK(shellVolume > 1000);
    const auto bodyId =
        run(s, "feature.base_body", {{"part", solid.at("id")}, {"name", "Cover"}}).at("id").get<std::uint64_t>();
    CHECK(body(s, bodyId).at("volume").get<double>() == Approx(shellVolume).epsilon(1e-3));
    run(s, "parameter.set", {{"name", "holeD"}, {"expression", "12 mm"}});
    const Json hole = run(s, "feature.cylinder",
                          {{"center", {0, 0, 0}},
                           {"diameter", "holeD"},
                           {"height", 200},
                           {"operation", "cut"},
                           {"bodies", Json::array({bodyId})}});
    INFO(hole.dump());
    const double cutVolume = body(s, bodyId).at("volume").get<double>();
    CHECK(cutVolume < shellVolume);
    // The hole goes through the 3 mm crown: about pi * 6^2 * 3 mm^3 removed.
    CHECK(shellVolume - cutVolume == Approx(std::numbers::pi * 36 * 3).epsilon(0.15));
    const auto bodyPart = body(s, bodyId).at("partId").get<std::uint64_t>();

    // 4. Export check and export in every format; each file reads back as the same solid.
    const Json check = run(s, "part.check", {{"id", bodyPart}});
    CHECK(check.at("watertight").get<bool>());
    for (const std::string ext : {"stl", "3mf", "step"}) {
        INFO(ext);
        const std::string out = dir.file("cover." + ext);
        const Json exported =
            run(s, "document.export", {{"path", out}, {"ids", Json::array({bodyPart})}, {"requireValid", true}});
        CHECK(exported.at("warnings").empty());
        const Json back = reimportCheck(out);
        CHECK(back.at("watertight").get<bool>());
        CHECK(back.at("volume").get<double>() == Approx(cutVolume).epsilon(0.02));
    }

    // 5. Save, reopen in a new session (as after restarting the app), change the parameter,
    //    export again.
    const std::string project = dir.file("cover.aerocad");
    run(s, "project.save", {{"path", project}});

    commands::Session reopened;
    run(reopened, "project.open", {{"path", project}});
    CHECK(run(reopened, "document.parts").at("parts").size() == run(s, "document.parts").at("parts").size());
    const Json scanBack = partById(reopened, scanId);
    CHECK(scanBack.at("selectedTriangles") == partById(s, scanId).at("selectedTriangles"));
    CHECK(partById(reopened, fitted.at("id").get<std::uint64_t>()).at("hasBrep").get<bool>());

    run(reopened, "parameter.set", {{"name", "holeD"}, {"expression", "20 mm"}});
    const double widerVolume = body(reopened, bodyId).at("volume").get<double>();
    CHECK(widerVolume < cutVolume);
    CHECK(cutVolume - widerVolume == Approx(std::numbers::pi * (100 - 36) * 3).epsilon(0.15));

    const std::string again = dir.file("cover-20mm.stl");
    run(reopened, "document.export", {{"path", again}, {"ids", Json::array({bodyPart})}, {"requireValid", true}});
    CHECK(reimportCheck(again).at("volume").get<double>() == Approx(widerVolume).epsilon(0.02));

    // The change is undoable after reopening, like any other edit.
    run(reopened, "edit.undo");
    CHECK(body(reopened, bodyId).at("volume").get<double>() == Approx(cutVolume).epsilon(1e-6));
    CHECK(body(reopened, bodyId).at("partId").get<std::uint64_t>() == bodyPart);
}

TEST_CASE("Workflow on an imperfect scan: open, noisy mesh with a hole", "[e2e][commands][scan]")
{
    TempDir dir;
    commands::Session s;
    const Json scan = run(s, "document.import", {{"path", modelFile("dome_scan.stl")}});
    const auto scanId = scan.at("id").get<std::uint64_t>();
    CHECK(scan.at("triangles").get<std::size_t>() > 8000);

    // The check reports the scan as open (outline and hole), so it is not exported as a solid.
    const Json check = run(s, "part.check", {{"id", scanId}});
    CHECK_FALSE(check.at("closed").get<bool>());
    CHECK(check.at("boundaryEdges").get<std::size_t>() > 100);
    const auto refused = s.execute("document.export", {{"path", dir.file("scan.stl")}, {"requireValid", true}});
    CHECK_FALSE(refused.ok);

    // A region around the hole: the fit averages the 0.05 mm noise and bridges the gap.
    // Over the hole the nearest scan point is up to 8 mm away, so surfaceToMeshMax is
    // larger there; the fit itself (mesh to surface) stays at noise level.
    const double top = scan.at("bounds").at("max")[2].get<double>();
    run(s, "selection.brush", {{"id", scanId}, {"points", Json::array({{40, 10, top}})}, {"radius", 40}});
    const Json fitted = run(s, "surface.fit_selection", {{"id", scanId}, {"tolerance", 0.5}});
    INFO(fitted.dump());
    CHECK(fitted.at("fit").at("meshToSurfaceMax").get<double>() < 0.5);
    CHECK(fitted.at("fit").at("meshToSurfaceRms").get<double>() < 0.1);

    // From there on it is a clean B-rep: thicken and export a watertight part.
    const Json solid = run(s, "surface.thicken", {{"id", fitted.at("id")}, {"thickness", 2.5}});
    CHECK(solid.at("volume").get<double>() > 1000);
    const std::string out = dir.file("patch.3mf");
    const Json exported =
        run(s, "document.export", {{"path", out}, {"ids", Json::array({solid.at("id")})}, {"requireValid", true}});
    CHECK(exported.at("warnings").empty());
    CHECK(reimportCheck(out).at("watertight").get<bool>());
}
