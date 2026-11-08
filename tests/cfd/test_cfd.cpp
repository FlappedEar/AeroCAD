// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cfd/CaseWriter.hpp"
#include "aerocad/cfd/Process.hpp"
#include "aerocad/cfd/Results.hpp"
#include "aerocad/cfd/RunManager.hpp"
#include "aerocad/cfd/Settings.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <random>
#include <sstream>
#include <thread>

using namespace aerocad;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

// Case directories are mounted into the engine's container. On macOS the engine's Linux VM
// (Colima, Docker Desktop) shares only the home directory by default, not $TMPDIR
// (/var/folders/...), so cases go under the home directory there.
fs::path caseTempRoot()
{
#ifdef __APPLE__
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
        return fs::path(home) / ".aerocad" / "test-runs";
#endif
    return fs::temp_directory_path();
}

class TempDir {
public:
    TempDir()
    {
        std::random_device rd;
        m_path = caseTempRoot() / ("aerocad_cfd_" + std::to_string(rd()));
        fs::create_directories(m_path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

/// Closed box with outward normals, mm.
core::TriangleMesh box(core::Vec3 origin, core::Vec3 size)
{
    core::TriangleMesh m;
    for (int i = 0; i < 8; ++i)
        m.addVertex({origin.x + ((i & 1) ? size.x : 0), origin.y + ((i & 2) ? size.y : 0),
                     origin.z + ((i & 4) ? size.z : 0)});
    const int f[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                          {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
    for (const auto& t : f)
        m.addTriangle(static_cast<std::uint32_t>(t[0]), static_cast<std::uint32_t>(t[1]),
                      static_cast<std::uint32_t>(t[2]));
    return m;
}

std::string slurp(const fs::path& p)
{
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

/// Writes the output files of a finished solver run into a case directory, in the
/// formats OpenFOAM v2506 writes them (abridged).
const char* const kBrokenMeshLog =
    "Checking geometry...\n"
    "    Overall domain bounding box (-0.1 -0.2 0) (0.3 0.2 0.2)\n"
    " ***Zero or negative cell volume detected.  Minimum negative volume: -1.2e-15, Number of negative volume "
    "cells: 3\n"
    "  <<Writing 3 zero volume cells to set zeroVolumeCells\n"
    "    Mesh non-orthogonality Max: 88.1 average: 9.2\n"
    " ***Number of non-orthogonality errors: 12\n"
    " ***Error in face pyramids: 24 faces are incorrectly oriented.\n"
    " ***Max skewness = 6.2, 4 highly skew faces detected which may impair the quality of the results\n"
    "\nFailed 4 mesh checks.\n\nEnd\n";
const char* const kPoorMeshLog = "Checking geometry...\n"
                                 "    Mesh non-orthogonality Max: 72.1 average: 9.2\n"
                                 " ***Number of non-orthogonality errors: 12\n"
                                 " ***Max skewness = 6.2, 4 highly skew faces detected which may impair the "
                                 "quality of the results\n"
                                 "\nFailed 2 mesh checks.\n\nEnd\n";

void fakeFinishedRun(const fs::path& dir, double cd, double dragN, double liftN = 0.2)
{
    std::ostringstream coeffs, forces, residuals;
    coeffs << "# Force and moment coefficients\n# Time Cd Cd(f) Cd(r) Cl Cl(f) Cl(r) CmPitch CmRoll CmYaw Cs Cs(f) "
              "Cs(r)\n";
    forces << "# Force\n# Time total_x total_y total_z pressure_x pressure_y pressure_z viscous_x viscous_y viscous_z\n";
    residuals << "# Solver information\n# Time U_solver Ux_initial Ux_final Ux_iters Uy_initial Uz_initial "
                 "p_solver p_initial p_final p_iters p_converged k_initial omega_initial\n";
    for (int i = 1; i <= 100; ++i) {
        const double c = cd + 0.5 / i; // Converges to cd.
        const double f = dragN * c / cd;
        coeffs << i << ' ' << c << " 0 0 0.1 0 0 0 0 0 0.01 0 0\n";
        forces << i << ' ' << f << " 0.01 " << liftN << ' ' << f * 0.9 << " 0 0 " << f * 0.1 << " 0 0\n";
        residuals << i << " smoothSolver " << 1e-4 / i << " 0 3 " << 1e-4 / i << ' ' << 1e-4 / i
                  << " GAMG " << 1e-3 / i << " 0 3 true " << 1e-4 / i << ' ' << 1e-4 / i << '\n';
    }
    write(dir / "postProcessing" / "forceCoeffs" / "0" / "coefficient.dat", coeffs.str());
    write(dir / "postProcessing" / "forces" / "0" / "force.dat", forces.str());
    write(dir / "postProcessing" / "solverInfo" / "0" / "solverInfo.dat", residuals.str());
    write(dir / "log.checkMesh", "Mesh stats\n    cells:            19893\n\nMesh OK.\n\nEnd\n");
    write(dir / "log.simpleFoam", "Time = 100\n\ntime step continuity errors : sum local = 2.4e-05, global = 1e-7\n"
                                  "SIMPLE solution converged in 100 iterations\nEnd\n");
    write(dir / "postProcessing" / "surfaces" / "100" / "part.vtk",
          "# vtk DataFile Version 2.0\ntime='100'\nASCII\nDATASET POLYDATA\nFIELD FieldData 1\nTimeValue 1 1 float\n100\n"
          "POINTS 3 float\n0 0 0 0.1 0 0\n0 0.1 0\nPOLYGONS 1 4\n3 0 1 2\nPOINT_DATA 3\nFIELD FieldData 2\n"
          "p 1 3 float\n200 -100 0\nU 3 3 float\n0 0 0 0 0 0 0 0 0\n");
    write(dir / "aerocad.stage", "done\n");
    write(dir / "aerocad.status", "finished\n");
    write(dir / "aerocad.engine", "v2506\n");
}

} // namespace

TEST_CASE("Settings: defaults, parsing and validation", "[cfd]")
{
    const cfd::Settings d;
    CHECK(d.preset == cfd::kExternalFlowPreset);
    CHECK(d.effectiveIterations() == 300);
    CHECK(d.effectiveProcesses() >= 1);

    const auto s = cfd::settingsFromJson({{"speed", 33.0}, {"mesh", "medium"}, {"flowDirection", "-X"},
                                          {"ground", true}, {"referenceArea", 0.5}, {"id", 7}});
    CHECK(s.speed == 33.0);
    CHECK(s.mesh == cfd::MeshLevel::Medium);
    CHECK(s.effectiveIterations() == 500);
    CHECK(s.flowDirection == "-X");
    CHECK(s.ground);
    CHECK(s.referenceArea == 0.5);

    CHECK_THROWS(cfd::settingsFromJson({{"speed", -1.0}}));
    CHECK_THROWS(cfd::settingsFromJson({{"speed", 400.0}}));
    CHECK_THROWS(cfd::settingsFromJson({{"flowDirection", "+Z"}}));
    CHECK_THROWS(cfd::settingsFromJson({{"mesh", "ultra"}}));
    CHECK_THROWS(cfd::settingsFromJson({{"preset", "other"}}));

    const auto j = cfd::toJson(s);
    CHECK(cfd::differingConditions(j, j).empty());
    auto k = j;
    k["speed"] = 30.0;
    k["processes"] = 64; // Not a condition.
    CHECK(cfd::differingConditions(j, k) == std::vector<std::string>{"speed"});
}

TEST_CASE("CaseFrame turns the flow direction into +X and back", "[cfd]")
{
    for (const char* dir : {"+X", "-X", "+Y", "-Y"}) {
        const auto f = cfd::CaseFrame::forFlow(dir, {10, 20, 30});
        const core::Vec3 flow = dir[1] == 'X' ? core::Vec3{dir[0] == '+' ? 1.0 : -1.0, 0, 0}
                                              : core::Vec3{0, dir[0] == '+' ? 1.0 : -1.0, 0};
        const auto r = f.rotate(flow);
        CHECK(r.x == Approx(1.0));
        CHECK(r.y == Approx(0.0).margin(1e-12));
        CHECK(f.rotate({0, 0, 1}).z == Approx(1.0)); // Up stays up.
        const core::Vec3 p{123, -45, 67};
        const auto back = f.toModel(f.toCase(p));
        CHECK(back.x == Approx(p.x));
        CHECK(back.y == Approx(p.y));
        CHECK(back.z == Approx(p.z));
        CHECK(f.toCase({10, 20, 30}).length() == Approx(0.0).margin(1e-12));
        const auto rt = cfd::CaseFrame::fromJson(f.toJson());
        CHECK(rt.quarterTurns == f.quarterTurns);
    }
}

TEST_CASE("Frontal area of a box is its cross-section", "[cfd]")
{
    // Already in case frame (m): 0.2 long in X, 0.08 x 0.06 cross-section.
    const auto mesh = box({0, 0, 0}, {0.2, 0.08, 0.06});
    CHECK(cfd::frontalArea(mesh) == Approx(0.08 * 0.06).epsilon(0.01));
}

TEST_CASE("writeCase writes a complete OpenFOAM case in SI units", "[cfd]")
{
    TempDir tmp;
    cfd::CaseGeometry g;
    g.part = box({1000, 500, 300}, {80, 200, 60}); // Long along +Y.
    cfd::Settings s;
    s.flowDirection = "+Y";
    s.processes = 2;
    const auto info = cfd::writeCase(tmp.path(), g, s);

    // Flow along +Y in the model = +X in the case: the 200 mm length is along case X.
    CHECK(info.partBounds.size().x == Approx(0.2));
    CHECK(info.partBounds.size().y == Approx(0.08));
    CHECK(info.characteristicLength == Approx(0.2));
    CHECK(info.referenceArea == Approx(0.08 * 0.06).epsilon(0.01));
    CHECK(info.referenceLength == Approx(0.2));
    CHECK(info.domain.min.x == Approx(-0.1 - 0.6));
    CHECK(info.domain.max.x >= 0.1 + 1.6 - 1e-9);

    for (const char* f : {"Allrun", "case.json", "case.foam", "constant/triSurface/part.stl", "constant/transportProperties",
                          "constant/turbulenceProperties", "system/blockMeshDict", "system/snappyHexMeshDict",
                          "system/surfaceFeatureExtractDict", "system/controlDict", "system/fvSchemes",
                          "system/fvSolution", "system/decomposeParDict", "0.orig/U", "0.orig/p", "0.orig/k",
                          "0.orig/omega", "0.orig/nut", "0.orig/include/initialConditions"})
        CHECK(fs::is_regular_file(tmp.path() / f));
    CHECK_FALSE(fs::exists(tmp.path() / "constant/triSurface/context.stl"));

    const std::string stl = slurp(tmp.path() / "constant/triSurface/part.stl");
    CHECK(stl.rfind("solid part", 0) == 0);
    CHECK(stl.find("vertex 0.1 ") != std::string::npos); // Metres, centred.

    const std::string control = slurp(tmp.path() / "system/controlDict");
    CHECK(control.find("Aref " + std::to_string(info.referenceArea).substr(0, 5)) != std::string::npos);
    CHECK(control.find("magUInf $aerocadUInf") != std::string::npos);
    CHECK(control.find("patches (part)") != std::string::npos);
    // Section stacks for the result view (CAD-148): every plane, cropped to the region
    // around the part.
    for (const auto& stack : cfd::sectionStacks())
        for (std::size_t i = 0; i < stack.fractions.size(); ++i)
            CHECK(control.find(cfd::sectionName(stack.axis, static_cast<int>(i)) + " { type cuttingPlane;") !=
                  std::string::npos);
    CHECK(cfd::sectionName('x', 12) == "sectionX_12");
    {
        const auto region = cfd::resultRegion(info);
        const std::size_t at = control.find("sectionY_2 { type cuttingPlane; point ");
        REQUIRE(at != std::string::npos);
        const std::string line = control.substr(at, control.find('\n', at) - at);
        CHECK(line.find("normal (0 1 0)") != std::string::npos);
        CHECK(line.find("bounds (") != std::string::npos);
        // The region holds the part with 0.4 lengths around it and 1.5 lengths of wake.
        const double L = info.characteristicLength;
        CHECK(region.min.x == Approx(info.partBounds.min.x - 0.4 * L));
        CHECK(region.max.x == Approx(info.partBounds.max.x + 1.5 * L));
        CHECK(region.min.z == Approx(info.partBounds.min.z - 0.4 * L)); // No floor here.
    }
    CHECK(slurp(tmp.path() / "Allrun").find("runParallel simpleFoam") != std::string::npos);
    CHECK(slurp(tmp.path() / "Allrun").find("\"${WM_PROJECT_VERSION:-}\" > aerocad.engine") != std::string::npos); // CAD-84
    CHECK(slurp(tmp.path() / "system/blockMeshDict").find("ground") == std::string::npos);

    const auto rt = cfd::CaseInfo::fromJson(nlohmann::json::parse(slurp(tmp.path() / "case.json")));
    CHECK(rt.referenceArea == Approx(info.referenceArea));
    CHECK(rt.settings.flowDirection == "+Y");
    CHECK(rt.geometryHash == info.geometryHash);

    // Same geometry and settings: same case (reproducible).
    TempDir tmp2;
    CHECK(cfd::writeCase(tmp2.path(), g, s).geometryHash == info.geometryHash);
    CHECK(slurp(tmp2.path() / "system/snappyHexMeshDict") == slurp(tmp.path() / "system/snappyHexMeshDict"));
}

TEST_CASE("writeCase with ground, context and one process", "[cfd]")
{
    TempDir tmp;
    cfd::CaseGeometry g;
    g.part = box({0, 0, 50}, {100, 50, 20});
    g.context = box({-200, -100, 0}, {400, 250, 50});
    cfd::Settings s;
    s.flowDirection = "+X";
    s.ground = true;
    s.processes = 1;
    const auto info = cfd::writeCase(tmp.path(), g, s);
    CHECK(fs::is_regular_file(tmp.path() / "constant/triSurface/context.stl"));
    CHECK(slurp(tmp.path() / "system/blockMeshDict").find("ground { type wall;") != std::string::npos);
    CHECK(slurp(tmp.path() / "0.orig/U").find("ground { type fixedValue;") != std::string::npos);
    CHECK(slurp(tmp.path() / "system/snappyHexMeshDict").find("context.stl") != std::string::npos);
    CHECK(slurp(tmp.path() / "Allrun").find("runParallel") == std::string::npos);
    // Floor just below the context geometry (case z of model z=0 is -0.06).
    CHECK(info.domain.min.z == Approx(-0.06 - 0.02 * 0.1));
    CHECK_THROWS(cfd::writeCase(tmp.path(), cfd::CaseGeometry{}, s));
}

#ifndef _WIN32
TEST_CASE("Allrun keeps the engine of reused solver results (CAD-84)", "[cfd]")
{
    // OpenFOAM's RunFunctions skips an application whose log exists. A stand-in with the same
    // rule shows which engine a rerun records.
    TempDir foam;
    fs::create_directories(foam.path() / "bin/tools");
    write(foam.path() / "bin/tools/RunFunctions",
          "runApplication() { [ -f \"log.$1\" ] && return 0; echo ran > \"log.$1\"; }\n"
          "runParallel() { runApplication \"$@\"; }\n"
          "restore0Dir() { :; }\n");
    for (const int processes : {1, 2}) {
        TempDir tmp;
        cfd::CaseGeometry g;
        g.part = box({0, 0, 0}, {100, 50, 20});
        cfd::Settings s;
        s.processes = processes;
        cfd::writeCase(tmp.path(), g, s);
        const std::string allrun = "\"" + (tmp.path() / "Allrun").string() + "\"";
        const auto run = [&](const char* version) {
            const std::string cmd = "WM_PROJECT_DIR=\"" + foam.path().string() + "\" WM_PROJECT_VERSION=" + version +
                                    " sh " + allrun + " >/dev/null 2>&1";
            return std::system(cmd.c_str());
        };
        REQUIRE(run("v2406") == 0);
        CHECK(slurp(tmp.path() / "aerocad.engine") == "v2406\n");
        CHECK(slurp(tmp.path() / "aerocad.status") == "finished\n");

        // Rerun in another environment: the solver is skipped, so its results keep v2406.
        REQUIRE(run("v2506") == 0);
        CHECK(slurp(tmp.path() / "aerocad.engine") == "v2406\n");

        // An older run that recorded no engine stays unknown rather than taking the new one.
        fs::remove(tmp.path() / "aerocad.engine");
        REQUIRE(run("v2506") == 0);
        CHECK_FALSE(fs::exists(tmp.path() / "aerocad.engine"));

        // A clean solve records the engine that ran it.
        fs::remove(tmp.path() / "log.simpleFoam");
        REQUIRE(run("v2506") == 0);
        CHECK(slurp(tmp.path() / "aerocad.engine") == "v2506\n");
    }
}

TEST_CASE("Allrun stops before the solve on a mesh with fatal checkMesh defects (CAD-103)", "[cfd]")
{
    // Stand-in RunFunctions with OpenFOAM's rule that an application whose log exists is
    // skipped, so a prepared log.checkMesh is what the check step reads.
    TempDir foam;
    fs::create_directories(foam.path() / "bin/tools");
    write(foam.path() / "bin/tools/RunFunctions",
          "runApplication() { [ -f \"log.$1\" ] && return 0; echo ran > \"log.$1\"; }\n"
          "runParallel() { runApplication \"$@\"; }\n"
          "restore0Dir() { :; }\n");
    for (const int processes : {1, 3}) {
        INFO("processes " << processes);
        for (const bool broken : {true, false}) {
            INFO((broken ? "broken mesh" : "poor but usable mesh"));
            TempDir tmp;
            cfd::CaseGeometry g;
            g.part = box({0, 0, 0}, {100, 50, 20});
            cfd::Settings s;
            s.processes = processes;
            cfd::writeCase(tmp.path(), g, s);
            write(tmp.path() / "log.checkMesh", broken ? kBrokenMeshLog : kPoorMeshLog);
            const std::string cmd = "WM_PROJECT_DIR=\"" + foam.path().string() + "\" sh \"" +
                                    (tmp.path() / "Allrun").string() + "\" >/dev/null 2>&1";
            const int rc = std::system(cmd.c_str());
            if (broken) {
                CHECK(rc != 0);
                CHECK(slurp(tmp.path() / "aerocad.status") == "failed check\n");
                CHECK_FALSE(fs::exists(tmp.path() / "log.potentialFoam"));
                CHECK_FALSE(fs::exists(tmp.path() / "log.simpleFoam"));
                // The diagnosis stays in place.
                CHECK(slurp(tmp.path() / "log.checkMesh") == kBrokenMeshLog);
            } else {
                CHECK(rc == 0);
                CHECK(slurp(tmp.path() / "aerocad.status") == "finished\n");
                CHECK(fs::exists(tmp.path() / "log.simpleFoam"));
            }
        }
    }
}
#endif

TEST_CASE("writeCase with a fixed floor at a given height", "[cfd]")
{
    TempDir tmp;
    cfd::CaseGeometry g;
    g.part = box({0, 0, 50}, {100, 50, 20});    // Body 50 mm above the floor,
    g.context = box({40, 20, -5}, {10, 10, 60}); // on a support reaching through it.
    cfd::Settings s;
    s.flowDirection = "+X";
    s.ground = true;
    s.groundMoving = false;
    s.groundHeight = 0.0;
    const auto info = cfd::writeCase(tmp.path(), g, s);
    CHECK(slurp(tmp.path() / "0.orig/U").find("ground { type noSlip; }") != std::string::npos);
    // Case origin is the part's bounds; model z = 0 is 50 mm below the part.
    CHECK(info.domain.min.z == Approx(info.partBounds.min.z - 0.05));
    s.groundHeight = 60.0; // Above the part's lowest point: the message names both heights (CAD-147).
    CHECK_THROWS_WITH(cfd::writeCase(tmp.path(), g, s),
                      Catch::Matchers::ContainsSubstring("floor at Z = 60.0 mm is above the lowest point of the part "
                                                         "(Z = 50.0 mm)"));

    const auto j = cfd::toJson(s);
    CHECK(cfd::settingsFromJson(j).groundMoving == false);
    // Runs written before the floor settings existed had a moving floor at the default height.
    auto old = cfd::toJson(cfd::Settings{});
    old.erase("groundMoving");
    old.erase("groundHeight");
    CHECK(cfd::differingConditions(old, cfd::toJson(cfd::Settings{})).empty());
    CHECK(cfd::differingConditions(old, j) == std::vector<std::string>{"flowDirection", "ground", "groundMoving", "groundHeight"});
}

TEST_CASE("Differing conditions are described in words with both values (CAD-159)", "[cfd]")
{
    auto a = cfd::toJson(cfd::Settings{});
    auto b = a;
    a["ground"] = true;
    b["ground"] = true;
    a["groundHeight"] = -40.0;
    b["groundHeight"] = -150.0;
    CHECK(cfd::describeCondition("groundHeight", a, b) == "Floor height: A -40 mm, B -150 mm");
    b["groundHeight"] = nullptr;
    CHECK(cfd::describeCondition("groundHeight", a, b) == "Floor height: A -40 mm, B just under the part");
    b["ground"] = false;
    CHECK(cfd::describeCondition("ground", a, b) == "Floor: A on, B off");
    b["groundMoving"] = false;
    CHECK(cfd::describeCondition("groundMoving", a, b) == "Floor motion: A moving, B fixed");
    a["speed"] = 30.0;
    b["speed"] = 40.0;
    CHECK(cfd::describeCondition("speed", a, b) == "Air speed: A 30 m/s, B 40 m/s");
    b["flowDirection"] = "-Y";
    a["flowDirection"] = "+X";
    CHECK(cfd::describeCondition("flowDirection", a, b) == "Flow direction: A +X, B -Y");
    a["turbulenceIntensity"] = 0.01;
    b["turbulenceIntensity"] = 0.05;
    CHECK(cfd::describeCondition("turbulenceIntensity", a, b) == "Turbulence intensity: A 1 %, B 5 %");
    b["referenceArea"] = 0.0048;
    CHECK(cfd::describeCondition("referenceArea", a, b) == "Reference area: A from the part, B 48 cm²");
    CHECK(cfd::describeCondition("somethingNew", a, b) == "somethingNew");
}

TEST_CASE("readTable reads function object output and handles restarts", "[cfd]")
{
    TempDir tmp;
    const fs::path f = tmp.path() / "coefficient.dat";
    write(f, "# Force coefficients\n# Time\tCd\tCl\n1\t0.5\t0.1\n2\t0.6\t0.2\n3\t0.7\t0.3\n2\t0.65\t0.25\n3\t0.66\t0.26\n4 0.67");
    const auto t = cfd::readTable(f);
    REQUIRE(t.columns == std::vector<std::string>{"Cd", "Cl"});
    CHECK(t.time == std::vector<double>{1, 2, 3});
    CHECK(t.column("Cd")->back() == Approx(0.66));
    CHECK(t.column("nope") == nullptr);
    CHECK(cfd::readTable(tmp.path() / "missing.dat").empty());
}

TEST_CASE("readTable keeps truncated history evidence until a replacement", "[cfd]")
{
    TempDir tmp;
    const fs::path path = tmp.path() / "coefficient.dat";
    const std::string header = "# Time Cd Cl\n";
    const std::string damaged = header + "1 0.8 0.1\n2 0.8\n3 0.8 0.1\n";
    write(path, damaged);
    auto table = cfd::readTable(path);
    CHECK(table.incomplete);
    CHECK(table.time == std::vector<double>{1, 3});

    write(path, damaged + "2 0.9 0.1\n3 0.9 0.1\n");
    table = cfd::readTable(path);
    CHECK_FALSE(table.incomplete);
    CHECK(table.time == std::vector<double>{1, 2, 3});
    CHECK(table.column("Cd")->at(1) == Approx(0.9));

    // A new partial row at a previously complete time cannot reuse the old values.
    write(path, header + "1 0.8 0.1\n2 0.8 0.1\n3 0.8 0.1\n2 0.9\n");
    table = cfd::readTable(path);
    CHECK(table.incomplete);
    CHECK(table.time == std::vector<double>{1});

    // Sampling intervals need not be consecutive integers.
    write(path, header + "1.5 0.8 0.1\n3.5 0.8 0.1\n  \n");
    table = cfd::readTable(path);
    CHECK_FALSE(table.incomplete);
    CHECK_FALSE(table.invalidTime);
    CHECK(table.time == std::vector<double>{1.5, 3.5});
}

TEST_CASE("readTable retains invalid time evidence on short rows", "[cfd]")
{
    TempDir tmp;
    const fs::path path = tmp.path() / "coefficient.dat";
    for (const char* time : {"nan", "inf", "invalid"}) {
        CAPTURE(time);
        write(path, "# Time Cd Cl\n1 0.8 0.1\n" + std::string(time) + " 0.8\n3 0.8 0.1\n");
        const auto table = cfd::readTable(path);
        CHECK(table.invalidTime);
        CHECK(table.time == std::vector<double>{1, 3});
    }
}

TEST_CASE("readTable keeps live history while the last row is still being written", "[cfd]")
{
    TempDir tmp;
    const fs::path path = tmp.path() / "coefficient.dat";
    std::string history = "# Time Cd Cl\n";
    for (int time = 1; time <= 120; ++time)
        history += std::to_string(time) + " 0.8 0.1\n";

    // A refresh can see only the first digits of the next timestamp. Even a
    // complete-looking row may still have more digits coming in its last value.
    for (const char* tail : {"12", "121 0.8", "12 0.9 0.2", "# Time Cd"}) {
        CAPTURE(tail);
        write(path, history + tail);
        const auto table = cfd::readTable(path);
        CHECK(table.incomplete);
        REQUIRE(table.time.size() == 120);
        CHECK(table.time.back() == 120);
        REQUIRE(table.column("Cd") != nullptr);
        CHECK(table.column("Cd")->size() == 120);
        CHECK(table.column("Cd")->back() == Approx(0.8));
    }

    write(path, history + "121 0.9 0.2\n");
    auto table = cfd::readTable(path);
    CHECK_FALSE(table.incomplete);
    CHECK(table.time.size() == 121);
    CHECK(table.time.back() == 121);
    CHECK(table.column("Cd")->back() == Approx(0.9));

    // A newline makes a short repeated row committed restart evidence. It must
    // invalidate the old values rather than silently reuse a previous run.
    write(path, history + "12\n");
    table = cfd::readTable(path);
    CHECK(table.incomplete);
    CHECK(table.time.size() == 11);
    CHECK(table.time.back() == 11);

    write(path, history + "# Time Cd\n1 0.9\n");
    table = cfd::readTable(path);
    CHECK_FALSE(table.incomplete);
    CHECK(table.columns == std::vector<std::string>{"Cd"});
    REQUIRE(table.time.size() == 1);
    CHECK(table.time.back() == 1);
    CHECK(table.column("Cd")->back() == Approx(0.9));
}

TEST_CASE("readVtk reads OpenFOAM legacy polydata", "[cfd]")
{
    TempDir tmp;
    const fs::path f = tmp.path() / "s.vtk";
    write(f, "# vtk DataFile Version 2.0\nstreamLines\nASCII\nDATASET POLYDATA\nPOINTS 4 float\n0 0 0 1 0 0 2 0 0\n"
             "3 0 0\nLINES 1 5\n4 0 1 2 3\nPOINT_DATA 4\nFIELD FieldData 2\np 1 4 float\n1 2 3 4\nU 3 4 float\n"
             "1 0 0 2 0 0 3 0 0 4 0 0\n");
    const auto d = cfd::readVtk(f);
    CHECK(d.points.size() == 4);
    REQUIRE(d.lines.size() == 1);
    CHECK(d.lines[0].size() == 4);
    CHECK(d.scalars.at("p")[3] == 4.0);
    CHECK(d.vectors.at("U")[1].x == 2.0);

    write(f, "# vtk DataFile Version 2.0\nx\nASCII\nDATASET POLYDATA\nPOINTS 1 float\n0 0 0\nPOLYGONS 1 4\n3 0 1 2\n");
    CHECK_THROWS(cfd::readVtk(f)); // Index out of range.
}

TEST_CASE("cropPolygons keeps the polygons around a region", "[cfd]")
{
    // Two triangles: one at the origin, one far away. Point 1 is shared.
    cfd::PolyData d;
    d.points = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {10, 0, 0}, {10, 1, 0}};
    d.polygons = {{0, 1, 2}, {1, 3, 4}};
    d.scalars["p"] = {0, 1, 2, 3, 4};
    d.vectors["U"] = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}, {4, 0, 0}};
    core::BoundingBox box;
    box.extend({-1, -1, -1});
    box.extend({2, 2, 1});
    const auto c = cfd::cropPolygons(d, box);
    REQUIRE(c.polygons.size() == 1);
    CHECK(c.points.size() == 3);
    CHECK(c.polygons[0] == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(c.scalars.at("p") == std::vector<double>{0, 1, 2});
    CHECK(c.vectors.at("U")[2].x == 2.0);
    CHECK(cfd::cropPolygons(d, core::BoundingBox{}).empty());
}

TEST_CASE("cropLines splits lines at the region and appendPolyData merges sets", "[cfd]")
{
    cfd::PolyData d;
    d.points = {{0, 0, 0}, {1, 0, 0}, {5, 0, 0}, {1.5, 0, 0}, {1.8, 0, 0}};
    d.lines = {{0, 1, 2, 3, 4}};
    d.scalars["p"] = {0, 1, 2, 3, 4};
    core::BoundingBox box;
    box.extend({-1, -1, -1});
    box.extend({2, 1, 1});
    const auto c = cfd::cropLines(d, box);
    REQUIRE(c.lines.size() == 2); // Leaves the box at x = 5 and comes back.
    CHECK(c.points.size() == 4);
    CHECK(c.scalars.at("p") == std::vector<double>{0, 1, 3, 4});

    cfd::PolyData merged = c;
    cfd::PolyData other;
    other.points = {{0, 0, 1}, {0, 0, 2}};
    other.lines = {{0, 1}};
    other.scalars["p"] = {7, 8};
    other.scalars["k"] = {1, 1}; // Only in one set: dropped.
    cfd::appendPolyData(merged, other);
    CHECK(merged.points.size() == 6);
    REQUIRE(merged.lines.size() == 3);
    CHECK(merged.lines[2] == std::vector<std::uint32_t>{4, 5});
    CHECK(merged.scalars.at("p").size() == 6);
    CHECK(merged.scalars.at("p")[5] == 8.0);
    CHECK(merged.scalars.count("k") == 0);
}

TEST_CASE("surfaceDifference maps B - A onto A's surface", "[cfd]")
{
    cfd::PolyData a;
    a.points = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    a.polygons = {{0, 1, 2}};
    a.scalars["Cp"] = {0.5, -0.2, 0.0};
    cfd::PolyData b;
    b.points = {{0.05, 0, 0}, {10, 0.02, 0}, {50, 50, 0}}; // B has no surface near A's third point.
    b.scalars["Cp"] = {0.7, -0.5, 1.0};
    const auto d = cfd::surfaceDifference(a, b, 0.5);
    CHECK(d.points.size() == 3);
    CHECK(d.polygons.size() == 1);
    const auto& dcp = d.scalars.at("dCp");
    CHECK(dcp[0] == Approx(0.2));
    CHECK(dcp[1] == Approx(-0.3));
    CHECK(std::isnan(dcp[2]));
}

TEST_CASE("Quality report from logs and histories", "[cfd]")
{
    TempDir tmp;
    CHECK(cfd::assessQuality(tmp.path()).status == "unknown");
    fakeFinishedRun(tmp.path(), 0.8, 1.0);
    const auto q = cfd::assessQuality(tmp.path());
    CHECK(q.meshOk == true);
    CHECK(q.cells == 19893);
    CHECK(q.solverConverged);
    CHECK(q.continuityError == Approx(2.4e-5));
    CHECK(q.finalResiduals.at("p") == Approx(1e-5));
    REQUIRE(q.cdFluctuation);
    CHECK(*q.cdFluctuation < 0.02);
    CHECK(q.status == "ok");

    write(tmp.path() / "log.checkMesh", "    cells:  10\n\nFailed 2 mesh checks.\n");
    const auto bad = cfd::assessQuality(tmp.path());
    CHECK(bad.meshOk == false);
    CHECK(bad.failedMeshChecks == 2);
    CHECK(bad.status == "warning");
}

TEST_CASE("Quality report never accepts missing convergence evidence", "[cfd][quality]")
{
    TempDir tmp;
    fakeFinishedRun(tmp.path(), 0.8, 1.0);
    fs::remove(tmp.path() / "log.simpleFoam");
    fs::remove(tmp.path() / "postProcessing" / "solverInfo" / "0" / "solverInfo.dat");

    const auto q = cfd::assessQuality(tmp.path());
    CHECK(q.meshOk == true);
    REQUIRE(q.cdFluctuation);
    CHECK_FALSE(q.solverConverged);
    CHECK(q.status == "unknown");
    CHECK_FALSE(q.warnings.empty());
    CHECK(q.checks.at("convergence") == "missing");
    CHECK(q.checks.at("massBalance") == "missing");
    CHECK(q.checks.at("residual.p") == "missing");
}

TEST_CASE("Quality report accepts complete current evidence", "[cfd][quality]")
{
    TempDir tmp;
    fakeFinishedRun(tmp.path(), 0.8, 1.0);
    SECTION("complete log with the standard startup banner")
    {
        write(tmp.path() / "log.simpleFoam",
              "Exec   : simpleFoam\nDate   : fixture\nCreate time\n\nTime = 100\n"
              "time step continuity errors : sum local = 1e-5, global = 1e-7\n"
              "ExecutionTime = 1 s  ClockTime = 1 s\nSIMPLE solution converged in 100 iterations\nEnd\n");
    }
    SECTION("abridged log without a banner") {}

    const auto q = cfd::assessQuality(tmp.path());
    CHECK(q.status == "ok");
    CHECK(q.warnings.empty());
    REQUIRE(q.checks.size() == 10);
    for (const auto& [name, state] : q.checks) {
        INFO(name);
        CHECK(state == "passed");
    }
    CHECK(q.toJson().at("checks").at("convergence") == "passed");
}

TEST_CASE("Quality report requires complete current evidence", "[cfd][quality]")
{
    TempDir tmp;
    fakeFinishedRun(tmp.path(), 0.8, 1.0);
    const auto solver = tmp.path() / "log.simpleFoam";
    const auto residuals = tmp.path() / "postProcessing" / "solverInfo" / "0" / "solverInfo.dat";
    const auto coefficients = tmp.path() / "postProcessing" / "forceCoeffs" / "0" / "coefficient.dat";

    SECTION("missing mesh verdict") { fs::remove(tmp.path() / "log.checkMesh"); }
    SECTION("missing residual file") { fs::remove(residuals); }
    SECTION("missing force history") { fs::remove(coefficients); }
    SECTION("truncated solver log")
    {
        std::string log = slurp(solver);
        log.erase(log.rfind("End"));
        write(solver, log);
    }
    SECTION("interrupted restart after a converged run")
    {
        write(solver, slurp(solver) + "Time = 101\nSolving for Ux");
    }
    SECTION("interrupted restart before its first iteration")
    {
        write(solver, slurp(solver) + "Exec   : simpleFoam\nDate   : fixture\n");
    }
    SECTION("interrupted restart with a suppressed banner")
    {
        write(solver, slurp(solver) + "Create time\n\nCreate mesh for time = 0\n");
    }
    SECTION("truncated newest residual row") { write(residuals, slurp(residuals) + "101 smoothSolver 1e-8"); }
    SECTION("truncated newest coefficient row") { write(coefficients, slurp(coefficients) + "101 0.8"); }
    SECTION("residuals do not reach final iteration")
    {
        write(residuals, "# Time Ux_initial Uy_initial Uz_initial p_initial k_initial omega_initial\n"
                         "99 1e-6 1e-6 1e-6 1e-5 1e-6 1e-6\n");
    }
    SECTION("one turbulence field absent")
    {
        write(residuals, "# Time Ux_initial Uy_initial Uz_initial p_initial k_initial\n"
                         "100 1e-6 1e-6 1e-6 1e-5 1e-6\n");
    }

    const auto q = cfd::assessQuality(tmp.path());
    CHECK(q.status == "unknown");
    CHECK_FALSE(q.warnings.empty());
}

TEST_CASE("Quality report rejects failed convergence and invalid mass balance", "[cfd][quality]")
{
    TempDir tmp;
    fakeFinishedRun(tmp.path(), 0.8, 1.0);
    const auto solver = tmp.path() / "log.simpleFoam";

    SECTION("solver ends without meeting residual targets")
    {
        write(solver, "Time = 100\ntime step continuity errors : sum local = 1e-5, global = 1e-7\nEnd\n");
        const auto q = cfd::assessQuality(tmp.path());
        CHECK(q.status == "warning");
        CHECK_FALSE(q.solverConverged);
        CHECK(q.checks.at("convergence") == "failed");
    }
    SECTION("fatal error after convergence marker")
    {
        write(solver, slurp(solver) + "FOAM FATAL ERROR: synthetic fixture\n");
        const auto q = cfd::assessQuality(tmp.path());
        CHECK(q.status == "warning");
        CHECK_FALSE(q.solverConverged);
        CHECK(q.checks.at("convergence") == "failed");
    }
    SECTION("nonfinite negative and excessive continuity error")
    {
        for (const std::string value : {"nan", "inf", "-1", "0.01"}) {
            INFO(value);
            write(solver, "Time = 100\ntime step continuity errors : sum local = " + value +
                              ", global = 1e-7\nSIMPLE solution converged in 100 iterations\nEnd\n");
            const auto q = cfd::assessQuality(tmp.path());
            CHECK(q.status == "warning");
            CHECK(q.checks.at("massBalance") == "failed");
            CHECK_FALSE(q.warnings.empty());
        }
    }
    SECTION("failed evidence takes precedence over missing evidence")
    {
        fs::remove(solver);
        write(tmp.path() / "log.checkMesh", "Failed 2 mesh checks.\n");
        CHECK(cfd::assessQuality(tmp.path()).status == "warning");
    }
}

TEST_CASE("Quality report validates every velocity pressure and turbulence residual", "[cfd][quality]")
{
    TempDir tmp;
    fakeFinishedRun(tmp.path(), 0.8, 1.0);
    const std::vector<std::string> fields{"Ux", "Uy", "Uz", "p", "k", "omega"};
    for (std::size_t field = 0; field < fields.size(); ++field) {
        for (const std::string invalid : {"0.1", "nan", "inf", "-0.1"}) {
            INFO(fields[field]);
            INFO(invalid);
            std::ostringstream data;
            data << "# Time Ux_initial Uy_initial Uz_initial p_initial k_initial omega_initial\n100";
            for (std::size_t i = 0; i < fields.size(); ++i)
                data << ' ' << (i == field ? invalid : "1e-6");
            data << '\n';
            write(tmp.path() / "postProcessing" / "solverInfo" / "0" / "solverInfo.dat", data.str());
            const auto q = cfd::assessQuality(tmp.path());
            CHECK(q.status == "warning");
            CHECK(q.checks.at("residual." + fields[field]) == "failed");
            CHECK(q.finalResiduals.contains(fields[field]));
        }
    }
}

TEST_CASE("Quality report rejects nonfinite coefficients and iteration values", "[cfd][quality]")
{
    TempDir tmp;
    fakeFinishedRun(tmp.path(), 0.8, 1.0);
    const std::vector<std::string> fields{"Time", "Cd", "Cl", "Cs"};
    for (std::size_t field = 0; field < fields.size(); ++field) {
        for (const std::string invalid : {"nan", "inf", "1e999"}) {
            INFO(fields[field]);
            INFO(invalid);
            std::ostringstream data;
            data << "# Time Cd Cl Cs\n";
            for (int row = 1; row <= 100; ++row) {
                const std::vector<std::string> good{std::to_string(row), "0.8", "0.1", "0.01"};
                for (std::size_t i = 0; i < fields.size(); ++i)
                    data << (i == 0 ? "" : " ") << (row == 100 && i == field ? invalid : good[i]);
                data << '\n';
            }
            write(tmp.path() / "postProcessing" / "forceCoeffs" / "0" / "coefficient.dat", data.str());
            const auto q = cfd::assessQuality(tmp.path());
            CHECK(q.status == "warning");
            CHECK(q.checks.at("forceStability") == "failed");
        }
    }
}

#ifndef _WIN32
TEST_CASE("Process runs, reports exit codes and terminates its group", "[cfd]")
{
    TempDir tmp;
    auto p = cfd::Process::start({"/bin/sh", "-c", "echo hello; exit 3"}, tmp.path(), tmp.path() / "log");
    for (int i = 0; i < 200 && p.running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(p.exitCode() == 3);
    CHECK(slurp(tmp.path() / "log") == "hello\n");

    auto sleeper = cfd::Process::start({"/bin/sh", "-c", "sleep 30"}, tmp.path(), tmp.path() / "log");
    CHECK(sleeper.running());
    sleeper.terminate();
    for (int i = 0; i < 200 && sleeper.running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_FALSE(sleeper.running());
    CHECK(sleeper.exitCode() == 128 + 15);

    CHECK_THROWS(cfd::Process::start({"/definitely/not/here"}, tmp.path(), tmp.path() / "log"));
    const auto out = cfd::runCommand({"/bin/sh", "-c", "echo out; echo err >&2"});
    CHECK(out.exitCode == 0);
    CHECK(out.output.find("out") != std::string::npos);
    CHECK(out.output.find("err") != std::string::npos);
    CHECK(cfd::runCommand({"/bin/sh", "-c", "sleep 5"}, std::chrono::milliseconds(100)).timedOut);
    CHECK(!cfd::findExecutable("sh").empty());
}
#endif

TEST_CASE("RunManager: prepared, finished, imported and compared runs", "[cfd]")
{
    TempDir tmp;
    cfd::RunManager runs(tmp.path());
    CHECK(runs.list().empty());

    cfd::CaseGeometry g;
    g.part = box({0, 0, 0}, {200, 80, 60});
    cfd::Settings s;
    s.flowDirection = "+X";
    cfd::RunSource src;
    src.partId = 3;
    src.partName = "Rear wing";
    src.label = "A";
    const std::string a = runs.prepare(g, s, src);
    CHECK(a.find("rear-wing") != std::string::npos);
    auto st = runs.status(a);
    REQUIRE(st);
    CHECK(st->state == cfd::RunState::Interrupted);
    CHECK(st->message.find("Allrun") != std::string::npos);
    CHECK(st->source.partName == "Rear wing");
    CHECK_FALSE(runs.cancel(a));

    fakeFinishedRun(tmp.path() / a, 0.8, 1.0);
    st = runs.status(a);
    CHECK(st->state == cfd::RunState::Finished);
    CHECK(st->progress == 1.0);
    CHECK(st->iteration == 100);
    CHECK(st->cd == Approx(0.805));

    const auto r = runs.results(a, 10);
    CHECK(r.at("history").at("cd").size() == 10);
    CHECK(r.at("history").at("iteration").back() == 100.0);
    CHECK(r.at("summary").at("dragN").get<double>() > 1.0);
    CHECK(r.at("quality").at("status") == "ok");
    CHECK(r.at("fields").at("surface") == true);
    CHECK(r.at("fields").at("streamlines") == false);

    const auto surface = runs.field(a, "surface");
    REQUIRE(surface.points.size() == 3);
    CHECK(surface.points[1].x == Approx(100.0 + 100.0)); // Back to model mm (origin = part centre).
    CHECK(surface.scalars.at("Cp")[0] == Approx(200.0 / (0.5 * 20 * 20)));
    CHECK(surface.scalars.at("pressure")[0] == Approx(200.0 * 1.205));
    CHECK(runs.field(a, "streamlines").empty());
    CHECK_THROWS(runs.field(a, "bogus"));

    // B: same conditions, 10 % more drag. Imported from outside the runs directory.
    TempDir outside;
    cfd::writeCase(outside.path(), g, s);
    fakeFinishedRun(outside.path(), 0.88, 1.1);
    const std::string b = runs.importCase(outside.path(), "B");
    CHECK(runs.status(b)->state == cfd::RunState::Finished);
    const auto cmp = runs.compare(a, b);
    CHECK(cmp.at("sameConditions") == true);
    CHECK(cmp.at("forces").at("dragN").at("percent").get<double>() == Approx(10.0).epsilon(0.01));
    CHECK(cmp.at("verdict") == "comparable");
    CHECK(cmp.at("colorScales").contains("Cp"));

    // State survives a restart (new manager on the same directory).
    cfd::RunManager again(tmp.path());
    CHECK(again.list().size() == 2);
    CHECK(again.status(a)->state == cfd::RunState::Finished);
    CHECK(again.remove(b));
    CHECK(fs::exists(outside.path() / "case.json")); // Imported case files are kept.
    CHECK(again.list().size() == 1);
}

// Full run in OpenFOAM (Docker or local), about a minute. Hidden from the default test
// run; ctest -L openfoam runs it. Not part of the fast CI gate.
TEST_CASE("OpenFOAM: box in a wind tunnel", "[.openfoam]")
{
    const auto env = cfd::detectEnvironment();
    if (!env.recommended)
        SKIP("No simulation engine: " + env.message);
    TempDir tmp;
    cfd::RunManager runs(tmp.path());
    cfd::CaseGeometry g;
    g.part = box({0, 0, 0}, {80, 200, 60});
    cfd::Settings s; // +Y flow, 20 m/s, coarse.
    const std::string id = runs.start(g, s, {});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
    auto st = runs.status(id);
    while (st->state == cfd::RunState::Running && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        st = runs.status(id);
    }
    INFO(st->message);
    REQUIRE(st->state == cfd::RunState::Finished);
    const auto r = runs.results(id);
    // Rectangular box, length/height about 3: Cd of order 0.8-1 (blunt body); the coarse
    // mesh is a smoke test, not a validation (CAD-52).
    const double cd = r.at("summary").at("cd").get<double>();
    CHECK(cd > 0.5);
    CHECK(cd < 1.5);
    CHECK(r.at("summary").at("dragN").get<double>() ==
          Approx(cd * 0.5 * s.density * s.speed * s.speed * r.at("conditions").at("referenceArea").get<double>())
              .epsilon(0.05));
    CHECK(r.at("quality").at("meshOk") == true);
    for (const char* kind : {"surface", "sectionHorizontal", "sectionVertical", "streamlines"})
        CHECK_FALSE(runs.field(id, kind).empty());
    // Every plane of the section stacks (CAD-148) is listed and readable; a plane across
    // the flow lies at its listed distance behind the part's front.
    const auto& sections = r.at("fields").at("sections");
    REQUIRE(sections.size() == cfd::sectionStacks().size());
    for (const auto& stack : sections)
        for (const auto& p : stack.at("positions"))
            CHECK_FALSE(runs.field(id, p.at("kind").get<std::string>()).empty());
    {
        const auto& x = sections.at(0);
        REQUIRE(x.at("axis") == "x");
        const auto& p = x.at("positions").at(6); // Half the part's length.
        const auto plane = runs.field(id, p.at("kind").get<std::string>());
        double y = 0;
        for (const auto& q : plane.points)
            y += q.y;
        y /= static_cast<double>(plane.points.size());
        // +Y flow: the box spans y 0..200 mm, so the plane at 0.5 lies at y = 100 mm.
        CHECK(p.at("mm").get<double>() == Approx(100.0).margin(1.0));
        CHECK(y == Approx(100.0).margin(1.0));
    }
    // Streamlines come back in model coordinates: they travel along +Y.
    const auto lines = runs.field(id, "streamlines");
    const auto& u = lines.vectors.at("U");
    double uy = 0;
    for (const auto& v : u)
        uy += v.y;
    CHECK(uy > 0);
}

TEST_CASE("Install for me: the next setup step", "[cfd]")
{
    using cfd::Platform;
    cfd::Environment none;
    cfd::SetupTools brewOnly;
    brewOnly.brew = "/opt/homebrew/bin/brew";
    // No Docker: install the docker CLI and Colima with Homebrew, never Docker Desktop.
    auto step = cfd::planEngineSetup(none, Platform::MacOS, brewOnly);
    CHECK(step.action == "terminal");
    REQUIRE(step.command.size() == 5);
    CHECK(step.command[0] == "osascript");
    CHECK(step.command[4].find("/opt/homebrew/bin/brew install docker colima && /opt/homebrew/bin/colima start "
                               "--cpu $(sysctl -n hw.ncpu)") != std::string::npos);
    CHECK(step.command[4].find("docker-desktop") == std::string::npos);
    CHECK(step.message.find("Colima") != std::string::npos);
    // Docker Desktop already there but Homebrew too: still Colima.
    cfd::SetupTools withDesktop = brewOnly;
    withDesktop.dockerDesktop = true;
    cfd::Environment cliOnly;
    cliOnly.docker = "/opt/homebrew/bin/docker";
    CHECK(cfd::planEngineSetup(cliOnly, Platform::MacOS, withDesktop).action == "terminal");
    // No Homebrew: the Homebrew page; Docker Desktop is only started when it is all there is.
    step = cfd::planEngineSetup(none, Platform::MacOS, {});
    CHECK(step.action == "browser");
    CHECK(step.command == std::vector<std::string>{"open", "https://brew.sh"});
    cfd::SetupTools desktopOnly;
    desktopOnly.dockerDesktop = true;
    CHECK(cfd::planEngineSetup(cliOnly, Platform::MacOS, desktopOnly).command ==
          std::vector<std::string>{"open", "-a", "Docker"});
    CHECK(cfd::planEngineSetup(none, Platform::Linux, {}).command.front() == "xdg-open");

    // Colima and docker installed, engine stopped: start Colima only.
    cfd::SetupTools colima;
    colima.colima = "/usr/local/bin/colima";
    step = cfd::planEngineSetup(cliOnly, Platform::MacOS, colima);
    CHECK(step.action == "terminal");
    CHECK(step.command[4].find("/usr/local/bin/colima start --cpu") != std::string::npos);
    CHECK(step.command[4].find("install") == std::string::npos);

    cfd::Environment installed;
    installed.docker = "/usr/local/bin/docker";
    installed.message = "Docker is installed but not running. Start it (sudo systemctl start docker) and try again.";
    step = cfd::planEngineSetup(installed, Platform::Linux, {});
    CHECK(step.action == "manual");
    // The panel shows the engine message once; a manual step only repeats it (CAD-116).
    CHECK(step.message == installed.message);

    installed.dockerRunning = true;
    step = cfd::planEngineSetup(installed, Platform::MacOS, {});
    CHECK(step.action == "pull");
    CHECK(step.command.empty());
    installed.imagePresent = true;
    CHECK(cfd::planEngineSetup(installed, Platform::MacOS, {}).action == "none");

    // Remote engine: only the docker CLI is needed here, never Colima.
    cfd::Environment remote;
    remote.remoteHost = "ssh://arek@build-box";
    step = cfd::planEngineSetup(remote, Platform::MacOS, brewOnly);
    CHECK(step.action == "terminal");
    CHECK(step.command[4].find("brew install docker\"") != std::string::npos);
    remote.docker = "/opt/homebrew/bin/docker";
    remote.message = "Cannot reach Docker on ssh://arek@build-box";
    step = cfd::planEngineSetup(remote, Platform::MacOS, brewOnly);
    CHECK(step.action == "manual");
    CHECK(step.message == remote.message);
    remote.dockerRunning = true;
    step = cfd::planEngineSetup(remote, Platform::MacOS, brewOnly);
    CHECK(step.action == "pull");
    CHECK(step.message.find("on ssh://arek@build-box") != std::string::npos);

    cfd::Environment local;
    local.openfoamBashrc = "/usr/lib/openfoam/openfoam2506/etc/bashrc";
    CHECK(cfd::planEngineSetup(local, Platform::Linux, {}).action == "none");

    TempDir tmp;
    cfd::RunManager runs(tmp.path());
    CHECK(runs.imagePullStatus().is_null());
#ifndef _WIN32
    // A stand-in for `docker pull` printing what Docker prints without a terminal.
    const fs::path fake = tmp.path() / "fake-docker";
    write(fake, "#!/bin/sh\necho '2506: Pulling from opencfd/openfoam-default'\necho 'aaa: Pulling fs layer'\n"
                "echo 'bbb: Pulling fs layer'\necho 'aaa: Pull complete'\nsleep 1\necho 'bbb: Pull complete'\n");
    fs::permissions(fake, fs::perms::owner_all);
    REQUIRE(runs.startImagePull(fake));
    CHECK_FALSE(runs.startImagePull(fake)); // Already running.
    auto pull = runs.imagePullStatus();
    CHECK(pull.at("state") == "running");
    for (int i = 0; i < 100 && pull.at("state") == "running"; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        pull = runs.imagePullStatus();
    }
    CHECK(pull.at("state") == "finished");
    CHECK(pull.at("progress") == 1.0);

    write(fake, "#!/bin/sh\necho 'Error response from daemon: no space left'\nexit 1\n");
    REQUIRE(runs.startImagePull(fake));
    for (int i = 0; i < 100 && runs.imagePullStatus().at("state") == "running"; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pull = runs.imagePullStatus();
    CHECK(pull.at("state") == "failed");
    CHECK(pull.at("message").get<std::string>().find("no space left") != std::string::npos);
#endif
}

TEST_CASE("RunManager: mesh sensitivity of an A/B difference", "[cfd]")
{
    TempDir tmp;
    cfd::RunManager runs(tmp.path());
    cfd::CaseGeometry ga, gb;
    ga.part = box({0, 0, 0}, {200, 80, 60});
    gb.part = box({0, 0, 0}, {200, 80, 70});
    auto run = [&](const cfd::CaseGeometry& g, cfd::MeshLevel mesh, double dragN) {
        cfd::Settings s;
        s.flowDirection = "+X";
        s.mesh = mesh;
        const std::string id = runs.prepare(g, s, {});
        fakeFinishedRun(tmp.path() / id, dragN, dragN);
        return id;
    };
    const std::string a = run(ga, cfd::MeshLevel::Coarse, 1.0);
    const std::string b = run(gb, cfd::MeshLevel::Coarse, 1.1);
    auto cmp = runs.compare(a, b);
    CHECK(cmp.at("meshSensitivity").at("status") == "not assessed");
    CHECK(cmp.at("meshSensitivity").at("suggestedMesh") == "medium");
    // B is 10 mm taller, so it has its own flow domain (CAD-84): compared, but qualified.
    CHECK(cmp.at("verdict") == "different domains");

    // Same A and B on the medium mesh: difference 0.10 N -> 0.12 N, it holds.
    const std::string am = run(ga, cfd::MeshLevel::Medium, 1.0);
    const std::string bm = run(gb, cfd::MeshLevel::Medium, 1.12);
    CHECK(runs.sameCaseOnOtherMeshes(a).at("medium").front().id == am);
    cmp = runs.compare(a, b);
    const auto& ms = cmp.at("meshSensitivity");
    CHECK(ms.at("status") == "holds");
    CHECK(ms.at("decidingForce") == "dragN");
    CHECK(ms.at("message") == "The drag difference holds on the finer mesh.");
    CHECK(ms.at("levels").size() == 2);
    CHECK(ms.at("dragN").at("difference").get<double>() == Approx(0.12).epsilon(0.02));
    CHECK(ms.at("dragN").at("change").get<double>() == Approx(0.02).epsilon(0.1));
    CHECK(cmp.at("verdict") == "different domains");
    CHECK(runs.compare(am, bm).at("meshSensitivity").at("status") == "holds"); // Same pair, seen from medium.

    // On the fine mesh the difference shrinks to 0.03 N: changed by more than half of itself.
    run(ga, cfd::MeshLevel::Fine, 1.0);
    run(gb, cfd::MeshLevel::Fine, 1.03);
    cmp = runs.compare(a, b);
    CHECK(cmp.at("meshSensitivity").at("status") == "inconclusive");
    CHECK(cmp.at("meshSensitivity").at("meshes") == nlohmann::json{"medium", "fine"});
    CHECK(cmp.at("meshSensitivity").at("message").get<std::string>().rfind("The drag difference changes", 0) == 0);
    CHECK(cmp.at("verdict") == "difference inconclusive");

    // Other conditions (speed) are a different case, not another mesh level.
    cfd::Settings fast;
    fast.flowDirection = "+X";
    fast.speed = 30;
    fast.mesh = cfd::MeshLevel::Medium;
    fakeFinishedRun(tmp.path() / runs.prepare(ga, fast, {}), 1.0, 2.0);
    CHECK(runs.sameCaseOnOtherMeshes(a).at("medium").size() == 1);
}

TEST_CASE("RunManager: mesh check names a smaller force that does not hold", "[cfd]")
{
    TempDir tmp;
    cfd::RunManager runs(tmp.path());
    cfd::CaseGeometry ga, gb;
    ga.part = box({0, 0, 0}, {200, 80, 60});
    gb.part = box({0, 0, 0}, {200, 80, 70});
    auto run = [&](const cfd::CaseGeometry& g, cfd::MeshLevel mesh, double dragN, double liftN) {
        cfd::Settings s;
        s.flowDirection = "+X";
        s.mesh = mesh;
        const std::string id = runs.prepare(g, s, {});
        fakeFinishedRun(tmp.path() / id, dragN, dragN, liftN);
        return id;
    };
    // Drag difference -0.65 N -> -0.75 N holds; lift difference +0.03 N -> -0.22 N flips.
    const std::string a = run(ga, cfd::MeshLevel::Coarse, 3.73, -0.19);
    const std::string b = run(gb, cfd::MeshLevel::Coarse, 3.08, -0.16);
    run(ga, cfd::MeshLevel::Medium, 3.80, 0.05);
    run(gb, cfd::MeshLevel::Medium, 3.05, -0.17);
    const auto ms = runs.compare(a, b).at("meshSensitivity");
    CHECK(ms.at("status") == "holds");
    CHECK(ms.at("liftN").at("signChanged") == true);
    CHECK(ms.at("message") ==
          "The drag difference holds on the finer mesh. The smaller lift difference does not (it changes sign).");

    // A refined run with quality warnings: no mesh verdict from it.
    const std::string cm = run(ga, cfd::MeshLevel::Fine, 3.80, 0.05);
    run(gb, cfd::MeshLevel::Fine, 3.05, -0.17);
    write(tmp.path() / cm / "log.checkMesh", "Mesh stats\n    cells:            19893\n\nFailed 2 mesh checks.\n\nEnd\n");
    const auto cmp = runs.compare(a, b);
    CHECK(cmp.at("meshSensitivity").at("status") == "check quality");
    CHECK(cmp.at("meshSensitivity").at("message").get<std::string>().find("fine mesh") != std::string::npos);
    CHECK(cmp.at("verdict") == "check quality");
}

TEST_CASE("RunManager: mesh check shows its running runs, and does not apply to a repeat run", "[cfd]")
{
    TempDir tmp;
    cfd::RunManager runs(tmp.path());
    cfd::CaseGeometry ga, gb;
    ga.part = box({0, 0, 0}, {200, 80, 60});
    gb.part = box({0, 0, 0}, {200, 80, 70});
    auto prepare = [&](const cfd::CaseGeometry& g, cfd::MeshLevel mesh) {
        cfd::Settings s;
        s.flowDirection = "+X";
        s.mesh = mesh;
        return runs.prepare(g, s, {});
    };
    const std::string a = prepare(ga, cfd::MeshLevel::Coarse);
    fakeFinishedRun(tmp.path() / a, 1.0, 1.0);
    const std::string b = prepare(gb, cfd::MeshLevel::Coarse);
    fakeFinishedRun(tmp.path() / b, 1.1, 1.1);

    // A's medium run still solving: the check waits for it and names it, for its progress (CAD-120).
    const std::string am = prepare(ga, cfd::MeshLevel::Medium);
    write(tmp.path() / am / "aerocad.stage", "solve\n");
    write(tmp.path() / am / "aerocad.status", "");
    write(tmp.path() / am / "log.simpleFoam", "Time = 10\n");
    auto ms = runs.compare(a, b).at("meshSensitivity");
    CHECK(ms.at("status") == "pending");
    CHECK(ms.at("runningRuns") == nlohmann::json{am});

    // A repeat run (same case as A): its difference is run-to-run variation, so the mesh
    // check does not apply and offers no runs on another mesh. Otherwise A and B would share
    // the runs on the other level and the check would pair them arbitrarily (CAD-120).
    const std::string a2 = prepare(ga, cfd::MeshLevel::Coarse);
    fakeFinishedRun(tmp.path() / a2, 1.0, 1.01);
    ms = runs.compare(a, a2).at("meshSensitivity");
    CHECK(ms.at("status") == "same case");
    CHECK_FALSE(ms.contains("suggestedMesh"));
    CHECK(ms.at("message").get<std::string>().rfind("A and B are the same case", 0) == 0);
}

TEST_CASE("RunManager: runs from an earlier session", "[cfd]")
{
    TempDir tmp;
    std::string id;
    {
        cfd::RunManager runs(tmp.path());
        cfd::CaseGeometry g;
        g.part = box({0, 0, 0}, {100, 50, 20});
        id = runs.prepare(g, cfd::Settings{}, {});
    }
    const fs::path dir = tmp.path() / id;
    // As if a local engine had been started by an earlier session: no status file yet.
    fs::remove(dir / "aerocad.status");
    auto runJson = nlohmann::json::parse(slurp(dir / "run.json"));
    runJson["backend"] = "local";
    write(dir / "run.json", runJson.dump());
    write(dir / "aerocad.stage", "solve\n");
    write(dir / "log.simpleFoam", "Time = 1\n");

    cfd::RunManager runs(tmp.path());
    // Logs written just now: still running. It cannot be stopped from here, nor deleted.
    CHECK(runs.status(id)->state == cfd::RunState::Running);
    CHECK_FALSE(runs.cancel(id));
    CHECK_FALSE(fs::exists(dir / "aerocad.cancelled"));
    CHECK_FALSE(runs.remove(id));

    // Logs an hour old: the engine is gone.
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(1);
    for (const char* f : {"aerocad.stage", "log.simpleFoam"})
        fs::last_write_time(dir / f, old);
    CHECK(runs.status(id)->state == cfd::RunState::Interrupted);
    CHECK(runs.remove(id));
}

#ifndef _WIN32
TEST_CASE("Listing runs only reads; a remote run is followed again at start-up (CAD-81)", "[cfd]")
{
    // A remote run started by another session and not finished yet. Its "docker" is a stand-in
    // that records each call, so following the run (which starts the remote fetch) is visible.
    TempDir root;
    TempDir bin;
    const fs::path calls = bin.path() / "calls";
    const fs::path docker = bin.path() / "docker";
    write(docker, "#!/bin/sh\necho \"$@\" >> \"" + calls.string() + "\"\nexit 1\n");
    fs::permissions(docker, fs::perms::owner_all, fs::perm_options::add);

    cfd::RunManager runs(root.path());
    CHECK(runs.list().empty());
    const fs::path dir = root.path() / "20261107-120000-wing";
    const nlohmann::json j{{"id", dir.filename().string()}, {"created", "2026-11-07T12:00:00Z"}, {"backend", "docker"},
                           {"docker", docker.string()}, {"dockerHost", "ssh://arek@build-box"}};
    write(dir / "run.json", j.dump());

    // Found by a later listing (simulation.list): reported, but nothing is started or written.
    runs.rescan();
    const auto listed = runs.list();
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].state == cfd::RunState::Interrupted);
    CHECK(listed[0].message.find("ssh://arek@build-box") != std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(fs::exists(calls));
    CHECK_FALSE(fs::exists(dir / "log.aerocad"));

    // A new session takes it over at start-up.
    cfd::RunManager next(root.path());
    for (int i = 0; i < 50 && !fs::exists(calls); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(fs::exists(calls));
    CHECK(fs::exists(dir / "log.aerocad"));
}
#endif

#ifndef _WIN32
TEST_CASE("Local runs beyond the processor limit wait in a queue (CAD-144)", "[cfd]")
{
    // A stand-in docker: reports a running engine with the image, and "solves" by sleeping
    // until it is stopped.
    TempDir root;
    TempDir bin;
    const fs::path docker = bin.path() / "docker";
    write(docker, "#!/bin/sh\ncase \"$1\" in\n  info) echo 4 ;;\n  image) echo sha256:0 ;;\n"
                  "  run) exec sleep 30 ;;\nesac\nexit 0\n");
    fs::permissions(docker, fs::perms::owner_all, fs::perm_options::add);
    const auto keep = [](const char* name) {
        const char* v = std::getenv(name);
        return v != nullptr ? std::optional<std::string>(v) : std::nullopt;
    };
    const auto oldDocker = keep("AEROCAD_DOCKER"), oldHost = keep("AEROCAD_ENGINE_HOST"),
               oldDir = keep("AEROCAD_CFD_DIR");
    ::setenv("AEROCAD_DOCKER", docker.c_str(), 1);
    ::setenv("AEROCAD_CFD_DIR", root.path().c_str(), 1); // No engine.json of this machine.
    ::unsetenv("AEROCAD_ENGINE_HOST");

    cfd::RunManager runs(root.path());
    runs.setProcessorLimit(4);
    cfd::CaseGeometry g;
    g.part = box({0, 0, 0}, {200, 80, 60});
    cfd::Settings s;
    s.processes = 2;
    const std::string a = runs.start(g, s, {}, cfd::Backend::Docker);
    const std::string b = runs.start(g, s, {}, cfd::Backend::Docker);
    const std::string c = runs.start(g, s, {}, cfd::Backend::Docker);
    // A and B fill the 4 processors; C waits and says why.
    CHECK(runs.status(a)->stage != "queued");
    CHECK(runs.status(b)->stage != "queued");
    auto sc = runs.status(c);
    CHECK(sc->state == cfd::RunState::Running);
    CHECK(sc->stage == "queued");
    CHECK(sc->message.find("Waiting for the other runs") != std::string::npos);

    // When A ends, C starts by itself.
    REQUIRE(runs.cancel(a));
    for (int i = 0; i < 50 && runs.status(a)->state == cfd::RunState::Running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(runs.status(a)->state == cfd::RunState::Cancelled);
    CHECK(runs.status(c)->stage != "queued");

    // A queued run can be cancelled before it starts.
    const std::string d = runs.start(g, s, {}, cfd::Backend::Docker);
    CHECK(runs.status(d)->stage == "queued");
    REQUIRE(runs.cancel(d));
    CHECK(runs.status(d)->state == cfd::RunState::Cancelled);

    for (const auto& id : {b, c})
        runs.cancel(id);
    for (int i = 0;
         i < 50 && (runs.status(b)->state == cfd::RunState::Running || runs.status(c)->state == cfd::RunState::Running);
         ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto restore = [](const char* name, const std::optional<std::string>& v) {
        if (v)
            ::setenv(name, v->c_str(), 1);
        else
            ::unsetenv(name);
    };
    restore("AEROCAD_DOCKER", oldDocker);
    restore("AEROCAD_ENGINE_HOST", oldHost);
    restore("AEROCAD_CFD_DIR", oldDir);
}

TEST_CASE("Queued runs start in submission order, and one that cannot start does not block the rest (CAD-144)", "[cfd]")
{
    TempDir root;
    TempDir bin;
    const fs::path docker = bin.path() / "docker";
    write(docker, "#!/bin/sh\ncase \"$1\" in\n  info) echo 4 ;;\n  image) echo sha256:0 ;;\n"
                  "  run) exec sleep 1 ;;\nesac\nexit 0\n");
    fs::permissions(docker, fs::perms::owner_all, fs::perm_options::add);
    const auto keep = [](const char* name) {
        const char* v = std::getenv(name);
        return v != nullptr ? std::optional<std::string>(v) : std::nullopt;
    };
    const auto oldDocker = keep("AEROCAD_DOCKER"), oldHost = keep("AEROCAD_ENGINE_HOST"),
               oldDir = keep("AEROCAD_CFD_DIR");
    ::setenv("AEROCAD_DOCKER", docker.c_str(), 1);
    ::setenv("AEROCAD_CFD_DIR", root.path().c_str(), 1);
    ::unsetenv("AEROCAD_ENGINE_HOST");

    cfd::RunManager runs(root.path());
    runs.setProcessorLimit(2);
    cfd::CaseGeometry g;
    g.part = box({0, 0, 0}, {200, 80, 60});
    cfd::Settings s;
    s.processes = 2;
    const auto named = [](const char* name) {
        cfd::RunSource src;
        src.partName = name;
        return src;
    };
    const auto waitWhileRunning = [&](const std::string& id) {
        for (int i = 0;
             i < 50 && runs.status(id)->state == cfd::RunState::Running && runs.status(id)->stage != "queued"; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };
    const std::string first = runs.start(g, s, named("first"), cfd::Backend::Docker);
    // Submitted in this order within one second: the id of "aaa" sorts before "zzz".
    const std::string z = runs.start(g, s, named("zzz"), cfd::Backend::Docker);
    const std::string a = runs.start(g, s, named("aaa"), cfd::Backend::Docker);
    REQUIRE(runs.status(z)->stage == "queued");
    REQUIRE(runs.status(a)->stage == "queued");

    waitWhileRunning(first);
    CHECK(runs.status(z)->stage != "queued"); // The earlier submission goes first.
    CHECK(runs.status(a)->stage == "queued");

    // The engine disappears while "aaa" waits: it fails alone, and the list still works.
    fs::remove(docker);
    waitWhileRunning(z);
    std::vector<cfd::RunStatus> all;
    REQUIRE_NOTHROW(all = runs.list());
    CHECK(all.size() == 3);
    const auto sa = runs.status(a);
    CHECK(sa->state == cfd::RunState::Failed);
    CHECK(sa->message.find("could not start") != std::string::npos);

    const auto restore = [](const char* name, const std::optional<std::string>& v) {
        if (v)
            ::setenv(name, v->c_str(), 1);
        else
            ::unsetenv(name);
    };
    restore("AEROCAD_DOCKER", oldDocker);
    restore("AEROCAD_ENGINE_HOST", oldHost);
    restore("AEROCAD_CFD_DIR", oldDir);
}

TEST_CASE("A running run reports the time so far and at most how long is left (CAD-149)", "[cfd]")
{
    TempDir root;
    TempDir bin;
    const fs::path docker = bin.path() / "docker";
    write(docker, "#!/bin/sh\ncase \"$1\" in\n  info) echo 4 ;;\n  image) echo sha256:0 ;;\n"
                  "  run) exec sleep 30 ;;\nesac\nexit 0\n");
    fs::permissions(docker, fs::perms::owner_all, fs::perm_options::add);
    const auto keep = [](const char* name) {
        const char* v = std::getenv(name);
        return v != nullptr ? std::optional<std::string>(v) : std::nullopt;
    };
    const auto oldDocker = keep("AEROCAD_DOCKER"), oldHost = keep("AEROCAD_ENGINE_HOST"),
               oldDir = keep("AEROCAD_CFD_DIR");
    ::setenv("AEROCAD_DOCKER", docker.c_str(), 1);
    ::setenv("AEROCAD_CFD_DIR", root.path().c_str(), 1);
    ::unsetenv("AEROCAD_ENGINE_HOST");

    cfd::RunManager runs(root.path());
    cfd::CaseGeometry g;
    g.part = box({0, 0, 0}, {200, 80, 60});
    const std::string id = runs.start(g, cfd::Settings{}, {}, cfd::Backend::Docker);
    auto st = runs.status(id);
    REQUIRE(st->state == cfd::RunState::Running);
    REQUIRE(st->elapsedSeconds.has_value());
    CHECK(*st->elapsedSeconds >= 0);
    CHECK_FALSE(st->remainingSeconds.has_value()); // Not solving yet: unknown.
    CHECK(st->toJson().at("remainingSeconds").is_null());

    // Solving: 100 of the iterations done in the 50 s since the initialisation ended.
    const fs::path dir = st->caseDir;
    write(dir / "aerocad.stage", "solve\n");
    std::ostringstream coeffs;
    coeffs << "# Force and moment coefficients\n# Time Cd Cl Cs\n";
    for (int i = 1; i <= 100; ++i)
        coeffs << i << " 0.8 0.1 0.01\n";
    write(dir / "postProcessing" / "forceCoeffs" / "0" / "coefficient.dat", coeffs.str());
    write(dir / "log.potentialFoam", "End\n");
    fs::last_write_time(dir / "log.potentialFoam", fs::file_time_type::clock::now() - std::chrono::seconds(50));
    st = runs.status(id);
    REQUIRE(st->iteration == 100);
    REQUIRE(st->maxIterations > 100);
    REQUIRE(st->remainingSeconds.has_value());
    const double expected = 50.0 / 100.0 * (st->maxIterations - 100);
    CHECK(*st->remainingSeconds >= expected - 1);
    CHECK(*st->remainingSeconds <= expected + 3);
    CHECK(st->toJson().at("remainingSeconds") == *st->remainingSeconds);

    // Finished or stopped runs have no running time.
    REQUIRE(runs.cancel(id));
    for (int i = 0; i < 50 && runs.status(id)->state == cfd::RunState::Running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    st = runs.status(id);
    CHECK_FALSE(st->elapsedSeconds.has_value());
    CHECK_FALSE(st->remainingSeconds.has_value());

    const auto restore = [](const char* name, const std::optional<std::string>& v) {
        if (v)
            ::setenv(name, v->c_str(), 1);
        else
            ::unsetenv(name);
    };
    restore("AEROCAD_DOCKER", oldDocker);
    restore("AEROCAD_ENGINE_HOST", oldHost);
    restore("AEROCAD_CFD_DIR", oldDir);
}
#endif

TEST_CASE("Remote engine host: validation and docker command line", "[cfd]")
{
    CHECK(cfd::isValidEngineHost(""));
    CHECK(cfd::isValidEngineHost("ssh://arek@192.168.1.20"));
    CHECK(cfd::isValidEngineHost("ssh://arek@build-box.local:2222"));
    CHECK(cfd::isValidEngineHost("unix:///var/run/docker.sock"));
    CHECK_FALSE(cfd::isValidEngineHost("arek@build-box"));
    CHECK_FALSE(cfd::isValidEngineHost("ssh://"));
    CHECK_FALSE(cfd::isValidEngineHost("tcp://build-box:2375")); // Unauthenticated: not offered.
    CHECK_FALSE(cfd::isValidEngineHost("ssh://a@b c"));
    CHECK_FALSE(cfd::isValidEngineHost("ssh://a@b;reboot"));
    CHECK_FALSE(cfd::isValidEngineHost("ssh://a@b$(id)"));
    CHECK(cfd::dockerCommand("/usr/bin/docker", "") == std::vector<std::string>{"/usr/bin/docker"});
    CHECK(cfd::dockerCommand("/usr/bin/docker", "ssh://a@b") ==
          std::vector<std::string>{"/usr/bin/docker", "-H", "ssh://a@b"});
}
TEST_CASE("Different surroundings or domains are not identical conditions (CAD-84)", "[cfd]")
{
    TempDir tmp;
    cfd::RunManager runs(tmp.path());
    cfd::Settings s;
    s.flowDirection = "+X";
    // The audit's case: the same target part alone and next to a context part.
    cfd::CaseGeometry alone, withContext, otherContext;
    alone.part = box({0, 0, 0}, {200, 80, 60});
    withContext.part = alone.part;
    withContext.context = box({-120, 20, 0}, {240, 240, 45});
    otherContext.part = alone.part;
    otherContext.context = box({-120, 20, 0}, {240, 240, 30});
    const std::string a = runs.prepare(alone, s, {});
    const std::string b = runs.prepare(withContext, s, {});
    const std::string c = runs.prepare(withContext, s, {});
    const std::string d = runs.prepare(otherContext, s, {});
    for (const auto& id : {a, b, c, d})
        fakeFinishedRun(tmp.path() / id, 0.8, 1.0);

    auto cmp = runs.compare(a, b);
    INFO(cmp.dump());
    CHECK(cmp.at("sameConditions") == false);
    const auto diff = cmp.at("differingConditions").get<std::vector<std::string>>();
    CHECK(std::find(diff.begin(), diff.end(), "context") != diff.end());
    CHECK(std::find(diff.begin(), diff.end(), "domain") != diff.end());
    CHECK(cmp.at("sameDomain") == false);
    CHECK(cmp.at("verdict") == "not comparable");

    // Same part, same surroundings: comparable.
    cmp = runs.compare(b, c);
    CHECK(cmp.at("sameConditions") == true);
    CHECK(cmp.at("sameDomain") == true);
    CHECK(cmp.at("verdict") == "comparable");

    // Surroundings of another shape, even within the same domain, are a different case.
    cmp = runs.compare(b, d);
    CHECK(cmp.at("sameConditions") == false);
    const auto diff2 = cmp.at("differingConditions").get<std::vector<std::string>>();
    CHECK(std::find(diff2.begin(), diff2.end(), "context") != diff2.end());

    // The resolved setup is part of the results.
    const auto cond = runs.results(b).at("conditions");
    CHECK(cond.contains("domain"));
    CHECK_FALSE(cond.at("contextHash").is_null());
}

namespace {

void editCaseJson(const fs::path& runDir, const std::function<void(nlohmann::json&)>& edit)
{
    const fs::path caseJson = runDir / "case.json";
    nlohmann::json j;
    {
        std::ifstream in(caseJson);
        in >> j;
    }
    edit(j);
    std::ofstream(caseJson) << j.dump(2);
}

bool contains(const nlohmann::json& list, const std::string& value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}

} // namespace

TEST_CASE("Setup evidence an older run did not record is unknown, not equal (CAD-84)", "[cfd]")
{
    TempDir tmp;
    cfd::RunManager runs(tmp.path());
    cfd::Settings s;
    s.flowDirection = "+X";
    cfd::CaseGeometry alone, withContext;
    alone.part = box({0, 0, 0}, {200, 80, 60});
    withContext.part = alone.part;
    withContext.context = box({-120, 20, 0}, {240, 240, 45});
    const std::string a = runs.prepare(alone, s, {});
    const std::string b = runs.prepare(withContext, s, {});
    const std::string c = runs.prepare(alone, s, {});
    for (const auto& id : {a, b, c})
        fakeFinishedRun(tmp.path() / id, 0.8, 1.0);

    // Matched records: identical conditions.
    auto cmp = runs.compare(a, c);
    INFO(cmp.dump());
    CHECK(cmp.at("sameConditions") == true);
    CHECK(cmp.at("unknownConditions").empty());
    CHECK(cmp.at("verdict") == "comparable");

    // The audit's pair with the surroundings of the context run not recorded.
    editCaseJson(tmp.path() / b, [](nlohmann::json& j) { j.erase("contextHash"); });
    cmp = runs.compare(a, b);
    INFO(cmp.dump());
    CHECK(cmp.at("sameConditions") == false);
    CHECK(contains(cmp.at("unknownConditions"), "context"));
    CHECK(contains(cmp.at("differingConditions"), "domain"));
    CHECK(cmp.at("verdict") == "not comparable");

    // The engine that actually ran: different versions are different conditions, even though
    // both cases were written for the same target version.
    write(tmp.path() / a / "aerocad.engine", "v2406\n");
    write(tmp.path() / c / "aerocad.engine", "v2512\n");
    cmp = runs.compare(a, c);
    INFO(cmp.dump());
    CHECK(contains(cmp.at("differingConditions"), "engine"));
    CHECK(cmp.at("verdict") == "not comparable");
    CHECK(runs.results(a).at("conditions").at("engine") == "v2406");
    CHECK(runs.results(a).at("conditions").at("engineTarget") == runs.results(c).at("conditions").at("engineTarget"));

    // Otherwise matched runs that did not record the engine: unknown, with a note.
    for (const auto& id : {a, c})
        fs::remove(tmp.path() / id / "aerocad.engine");
    cmp = runs.compare(a, c);
    INFO(cmp.dump());
    CHECK(cmp.at("sameConditions") == false);
    CHECK(contains(cmp.at("unknownConditions"), "engine"));
    CHECK(cmp.at("verdict") == "not comparable");
    bool noted = false;
    for (const auto& n : cmp.at("notes"))
        noted = noted || n.get<std::string>().find("engine of both runs was not recorded") != std::string::npos;
    CHECK(noted);
}

TEST_CASE("A finished run whose recorded domain is missing is compared as unknown, not an error (CAD-84)", "[cfd]")
{
    TempDir tmp;
    cfd::RunManager runs(tmp.path());
    cfd::CaseGeometry g;
    g.part = box({0, 0, 0}, {200, 80, 60});
    cfd::Settings s;
    s.flowDirection = "+X";
    const std::string a = runs.prepare(g, s, {});
    const std::string b = runs.prepare(g, s, {});
    for (const auto& id : {a, b})
        fakeFinishedRun(tmp.path() / id, 0.8, 1.0); // with surface field output
    editCaseJson(tmp.path() / b, [](nlohmann::json& j) { j.erase("domain"); });
    nlohmann::json cmp;
    REQUIRE_NOTHROW(cmp = runs.compare(a, b));
    INFO(cmp.dump());
    CHECK(contains(cmp.at("unknownConditions"), "domain"));
    CHECK(cmp.at("sameConditions") == false);
    CHECK(cmp.at("verdict") == "not comparable");
}

TEST_CASE("A target-part variant of another size is compared on a different domain (CAD-84)", "[cfd]")
{
    TempDir tmp;
    cfd::RunManager runs(tmp.path());
    cfd::Settings s;
    s.flowDirection = "+X";
    cfd::CaseGeometry base, longer;
    base.part = box({0, 0, 0}, {200, 80, 60});
    base.context = box({-120, 120, 0}, {240, 240, 45});
    longer.part = box({0, 0, 0}, {215, 80, 60}); // e.g. the same part with a lip
    longer.context = base.context;
    const std::string a = runs.prepare(base, s, {});
    const std::string b = runs.prepare(longer, s, {});
    for (const auto& id : {a, b})
        fakeFinishedRun(tmp.path() / id, 0.8, 1.0);
    const auto cmp = runs.compare(a, b);
    INFO(cmp.dump());
    CHECK(cmp.at("sameConditions") == false);
    CHECK(cmp.at("sameDomain") == false);
    CHECK(cmp.at("differingConditions") == nlohmann::json::array({"domain"}));
    CHECK(cmp.at("conditionChanges") == nlohmann::json::array({"Flow domain: different size or position (see below)"}));
    CHECK(cmp.at("verdict") == "different domains");
    CHECK_FALSE(cmp.at("forces").empty()); // the variant is still compared
    bool noted = false;
    for (const auto& n : cmp.at("notes"))
        noted = noted || n.get<std::string>().find("because the part differs in size or position") != std::string::npos;
    CHECK(noted);
}

TEST_CASE("A/B names a floor-height difference with both heights and as the domain's cause (CAD-159)", "[cfd]")
{
    TempDir tmp;
    cfd::RunManager runs(tmp.path());
    cfd::Settings s;
    s.flowDirection = "+X";
    s.ground = true;
    cfd::CaseGeometry g;
    g.part = box({0, 0, 0}, {200, 80, 60});
    s.groundHeight = -40.0;
    const std::string a = runs.prepare(g, s, {});
    s.groundHeight = -150.0;
    const std::string b = runs.prepare(g, s, {});
    for (const auto& id : {a, b})
        fakeFinishedRun(tmp.path() / id, 0.8, 1.0);
    const auto cmp = runs.compare(a, b);
    INFO(cmp.dump());
    CHECK(contains(cmp.at("differingConditions"), "groundHeight"));
    CHECK(contains(cmp.at("conditionChanges"), "Floor height: A -40 mm, B -150 mm"));
    CHECK(cmp.at("verdict") == "not comparable");
    bool cause = false, wrongCause = false;
    for (const auto& n : cmp.at("notes")) {
        const auto text = n.get<std::string>();
        cause = cause || text.find("because the floor differs") != std::string::npos;
        wrongCause = wrongCause || text.find("part differs") != std::string::npos;
    }
    CHECK(cause);
    CHECK_FALSE(wrongCause);
}

TEST_CASE("The same surroundings given in another part order are the same context (CAD-84)", "[cfd]")
{
    TempDir tmp;
    cfd::RunManager runs(tmp.path());
    cfd::Settings s;
    s.flowDirection = "+X";
    const auto first = box({-120, 120, 0}, {240, 240, 45});
    const auto second = box({300, -200, 0}, {360, -140, 30});
    cfd::CaseGeometry ab, ba;
    ab.part = box({0, 0, 0}, {200, 80, 60});
    ba.part = ab.part;
    ab.context.append(first);
    ab.context.append(second);
    ba.context.append(second);
    ba.context.append(first);
    const std::string a = runs.prepare(ab, s, {});
    const std::string b = runs.prepare(ba, s, {});
    for (const auto& id : {a, b})
        fakeFinishedRun(tmp.path() / id, 0.8, 1.0);
    const auto cmp = runs.compare(a, b);
    INFO(cmp.dump());
    CHECK(cmp.at("sameConditions") == true);
    CHECK(cmp.at("verdict") == "comparable");
}

TEST_CASE("A failed run says why: fatal mesh defects or a diverged solve (CAD-103)", "[cfd]")
{
    TempDir tmp;
    cfd::RunManager runs(tmp.path());
    cfd::CaseGeometry g;
    g.part = box({0, 0, 0}, {200, 80, 60});
    const std::string id = runs.prepare(g, cfd::Settings{}, cfd::RunSource{});
    const fs::path dir = tmp.path() / id;

    // Stopped by the check: the fatal reports, not the warnings or the "Failed N" count.
    write(dir / "log.checkMesh", kBrokenMeshLog);
    write(dir / "aerocad.status", "failed check\n");
    auto st = runs.status(id);
    REQUIRE(st);
    CHECK(st->state == cfd::RunState::Failed);
    INFO(st->message);
    CHECK(st->message.find("solve was not started") != std::string::npos);
    CHECK(st->message.find("Zero or negative cell volume detected.") != std::string::npos);
    CHECK(st->message.find("..") == std::string::npos);
    CHECK(st->message.find("Number of negative volume cells: 3") != std::string::npos);
    CHECK(st->message.find("Error in face pyramids: 24 faces are incorrectly oriented.") != std::string::npos);
    CHECK(st->message.find("skewness") == std::string::npos);
    CHECK(st->message.find("non-orthogonality") == std::string::npos);
    CHECK(st->message.find("log.checkMesh") != std::string::npos);

    // A solver that diverged and died of SIGFPE in a parallel run: the reason, not the
    // MPI separator line or a stack frame.
    const std::string crash = "Time = 254\n\nsmoothSolver:  Solving for Ux, Initial residual = 0.9\n\nTime = 255\n\n"
                              "[3] #0  Foam::error::printStack(Foam::Ostream&) at ??:?\n"
                              "[3] #1  Foam::sigFpe::sigHandler(int) at ??:?\n"
                              "[3] #2  ? in /lib/x86_64-linux-gnu/libc.so.6\n";
    const std::string separator = "--------------------------------------------------------------------------\n";
    for (const std::string& tail :
         {std::string(), separator,
          separator +
              "prterun noticed that process rank 3 with PID 0 on node x exited on signal 8 (Floating "
              "point exception).\n" +
              separator}) {
        write(dir / "log.simpleFoam", crash + tail);
        write(dir / "aerocad.status", "failed solve\n");
        st = runs.status(id);
        REQUIRE(st);
        INFO(st->message);
        CHECK(st->message.find("Step 'solve' failed (log.simpleFoam)") != std::string::npos);
        CHECK(st->message.find("floating-point exception: the solution diverged at iteration 255") !=
              std::string::npos);
        CHECK(st->message.find("----") == std::string::npos);
    }

    // OpenFOAM's start-up banner when trapping is on is not a crash: a later segmentation
    // fault keeps its own reason.
    write(dir / "log.simpleFoam",
          "sigFpe : Enabling floating point exception trapping (FOAM_SIGFPE).\nTime = 12\n\n"
          "[2] #0  Foam::error::printStack(Foam::Ostream&) at ??:?\n[2] #1  Foam::sigSegv::sigHandler(int) at ??:?\n" +
              separator +
              "prterun noticed that process rank 2 with PID 0 on node x exited on signal 11 (Segmentation "
              "fault).\n" +
              separator);
    st = runs.status(id);
    INFO(st->message);
    CHECK(st->message.find("diverged") == std::string::npos);
    CHECK(st->message.find("Segmentation fault") != std::string::npos);

    // A fatal error keeps its first line; a mesh check without fatal reports keeps the old summary.
    write(dir / "log.simpleFoam", "--> FOAM FATAL ERROR: (openfoam-2506)\n\n    Unknown patch type wall2\n\n"
                                  "    From Foam::fvPatch::New\n");
    st = runs.status(id);
    CHECK(st->message.find("Unknown patch type wall2") != std::string::npos);
    write(dir / "log.checkMesh", "Checking geometry...\n\nEnd\n");
    write(dir / "aerocad.status", "failed check\n");
    st = runs.status(id);
    CHECK(st->message == "Step 'check' failed (log.checkMesh): End");
}

TEST_CASE("Part sizes outside a wind tunnel's range are flagged (CAD-109)", "[cfd]")
{
    CHECK(cfd::partSizeWarning(0.2).empty());
    CHECK(cfd::partSizeWarning(cfd::kMinPlausiblePartLength).empty());
    CHECK(cfd::partSizeWarning(cfd::kMaxPlausiblePartLength).empty());
    CHECK(cfd::partSizeWarning(0.0).empty());
    CHECK(cfd::partSizeWarning(0.00112).find("only 1.12 mm long") != std::string::npos);
    CHECK(cfd::partSizeWarning(4600.0).find("4600 m long") != std::string::npos);
}
