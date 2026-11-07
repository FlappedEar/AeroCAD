// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cfd/CaseWriter.hpp"
#include "aerocad/cfd/Process.hpp"
#include "aerocad/cfd/Results.hpp"
#include "aerocad/cfd/RunManager.hpp"
#include "aerocad/cfd/Settings.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

using namespace aerocad;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir()
    {
        std::random_device rd;
        m_path = fs::temp_directory_path() / ("aerocad_cfd_" + std::to_string(rd()));
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
void fakeFinishedRun(const fs::path& dir, double cd, double dragN, double liftN = 0.2)
{
    std::ostringstream coeffs, forces, residuals;
    coeffs << "# Force and moment coefficients\n# Time Cd Cd(f) Cd(r) Cl Cl(f) Cl(r) CmPitch CmRoll CmYaw Cs Cs(f) "
              "Cs(r)\n";
    forces << "# Force\n# Time total_x total_y total_z pressure_x pressure_y pressure_z viscous_x viscous_y viscous_z\n";
    residuals << "# Solver information\n# Time U_solver Ux_initial Ux_final Ux_iters p_solver p_initial p_final "
                 "p_iters p_converged\n";
    for (int i = 1; i <= 100; ++i) {
        const double c = cd + 0.5 / i; // Converges to cd.
        const double f = dragN * c / cd;
        coeffs << i << ' ' << c << " 0 0 0.1 0 0 0 0 0 0.01 0 0\n";
        forces << i << ' ' << f << " 0.01 " << liftN << ' ' << f * 0.9 << " 0 0 " << f * 0.1 << " 0 0\n";
        residuals << i << " smoothSolver " << 1.0 / i << " 0 3 GAMG " << 0.1 / i << " 0 3 true\n";
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
    CHECK(slurp(tmp.path() / "Allrun").find("runParallel simpleFoam") != std::string::npos);
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
    s.groundHeight = 60.0; // Above the part's lowest point.
    CHECK_THROWS(cfd::writeCase(tmp.path(), g, s));

    const auto j = cfd::toJson(s);
    CHECK(cfd::settingsFromJson(j).groundMoving == false);
    // Runs written before the floor settings existed had a moving floor at the default height.
    auto old = cfd::toJson(cfd::Settings{});
    old.erase("groundMoving");
    old.erase("groundHeight");
    CHECK(cfd::differingConditions(old, cfd::toJson(cfd::Settings{})).empty());
    CHECK(cfd::differingConditions(old, j) == std::vector<std::string>{"flowDirection", "ground", "groundMoving", "groundHeight"});
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
    CHECK(q.finalResiduals.at("p") == Approx(0.001));
    REQUIRE(q.cdFluctuation);
    CHECK(*q.cdFluctuation < 0.02);
    CHECK(q.status == "ok");

    write(tmp.path() / "log.checkMesh", "    cells:  10\n\nFailed 2 mesh checks.\n");
    const auto bad = cfd::assessQuality(tmp.path());
    CHECK(bad.meshOk == false);
    CHECK(bad.failedMeshChecks == 2);
    CHECK(bad.status == "warning");
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
    CHECK(cfd::planEngineSetup(installed, Platform::Linux, {}).action == "manual");

    installed.dockerRunning = true;
    step = cfd::planEngineSetup(installed, Platform::MacOS, {});
    CHECK(step.action == "pull");
    CHECK(step.command.empty());
    installed.imagePresent = true;
    CHECK(cfd::planEngineSetup(installed, Platform::MacOS, {}).action == "none");

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
    CHECK(cmp.at("verdict") == "comparable");

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
    CHECK(cmp.at("verdict") == "comparable");
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
