// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cfd/RunManager.hpp"
#include "aerocad/commands/Session.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

using namespace aerocad;
using commands::Json;
namespace fs = std::filesystem;

namespace {

/// Points the runs directory at a fresh temp dir for the lifetime of the object.
class RunsDir {
public:
    RunsDir()
    {
        std::random_device rd;
        m_path = fs::temp_directory_path() / ("aerocad_sim_" + std::to_string(rd()));
#ifdef _WIN32
        _putenv_s("AEROCAD_CFD_DIR", m_path.string().c_str());
#else
        ::setenv("AEROCAD_CFD_DIR", m_path.c_str(), 1);
#endif
    }
    ~RunsDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    RunsDir(const RunsDir&) = delete;
    RunsDir& operator=(const RunsDir&) = delete;
    [[nodiscard]] const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

std::uint64_t importBox(commands::Session& session)
{
    const auto r = session.execute("document.import", {{"path", AEROCAD_TEST_DATA_DIR "/box_200x80x60.stl"}});
    REQUIRE(r.ok);
    return r.data.at("id").get<std::uint64_t>();
}

void writeOutput(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    REQUIRE(out.is_open());
    out << text;
}

// Synthetic OpenFOAM output, not a computed aerodynamic result. Completion is set
// separately so identical files can exercise every run lifecycle state.
void writeResultFixture(const fs::path& dir, bool coefficientsOnly = false)
{
    std::ostringstream coefficients, forces, residuals;
    coefficients << "# Time Cd Cl Cs\n";
    forces << "# Time total_x total_y total_z pressure_x viscous_x\n";
    residuals << "# Time Ux_initial Uy_initial Uz_initial p_initial k_initial omega_initial\n";
    for (int i = 1; i <= 20; ++i) {
        coefficients << i << " 0.8 0.1 0.01\n";
        forces << i << " 1.0 0.01 0.2 0.9 0.1\n";
        residuals << i << " 1e-6 1e-6 1e-6 1e-6 1e-6 1e-6\n";
    }
    writeOutput(dir / "postProcessing/forceCoeffs/0/coefficient.dat", coefficients.str());
    writeOutput(dir / "log.checkMesh", "Mesh stats\n    cells: 1000\nMesh OK.\nEnd\n");
    if (!coefficientsOnly) {
        writeOutput(dir / "postProcessing/forces/0/force.dat", forces.str());
        writeOutput(dir / "postProcessing/solverInfo/0/solverInfo.dat", residuals.str());
        writeOutput(dir / "log.simpleFoam", "Time = 20\ntime step continuity errors : sum local = 1e-7, global = 1e-8\n"
                                          "SIMPLE solution converged in 20 iterations\nEnd\n");
    }
}

std::string prepareRun(commands::Session& session, std::uint64_t part)
{
    const auto r = session.execute("simulation.run", {{"id", part}, {"prepareOnly", true}});
    REQUIRE(r.ok);
    return r.data.at("id");
}

} // namespace

TEST_CASE("simulation.* commands are registered with schemas", "[commands][simulation]")
{
    RunsDir dir;
    commands::Session session;
    for (const char* name : {"simulation.environment", "simulation.propose_run", "simulation.run", "simulation.status",
                             "simulation.list", "simulation.cancel", "simulation.delete", "simulation.results",
                             "simulation.field", "simulation.compare", "simulation.import",
                             "simulation.install_engine", "simulation.refine", "simulation.set_engine"}) {
        INFO(name);
        const auto* info = session.registry().info(name);
        REQUIRE(info != nullptr);
        CHECK_FALSE(info->modifiesDocument);
    }
    CHECK(session.simulations().root() == dir.path());

    const auto env = session.execute("simulation.environment");
    REQUIRE(env.ok);
    CHECK(env.data.at("openfoamVersion") == "v2506");
    CHECK(env.data.at("image") == "opencfd/openfoam-default:2506");
    CHECK(env.data.at("defaults").at("speed") == 20.0);
    CHECK(env.data.at("setup").contains("action"));
    CHECK(env.data.at("imagePull").is_null());
}

TEST_CASE("simulation.propose_run and a prepared run", "[commands][simulation]")
{
    RunsDir dir;
    commands::Session session;
    const auto id = importBox(session);

    auto r = session.execute("simulation.propose_run", {{"id", id}, {"speed", 30.0}, {"flowDirection", "+X"}});
    REQUIRE(r.ok);
    CHECK(r.data.at("settings").at("speed") == 30.0);
    // 200 x 80 x 60 mm box, flow along X: frontal area 80 x 60 mm.
    CHECK(std::abs(r.data.at("referenceArea").get<double>() - 0.0048) < 0.0048 * 0.01);
    CHECK(std::abs(r.data.at("characteristicLength").get<double>() - 0.2) < 1e-9);

    CHECK_FALSE(session.execute("simulation.propose_run", {{"id", 999}}).ok);
    CHECK_FALSE(session.execute("simulation.propose_run", {{"id", id}, {"speed", -5.0}}).ok);
    CHECK_FALSE(session.execute("simulation.propose_run", {{"id", id}, {"flowDirection", "up"}}).ok);
    CHECK_FALSE(session.execute("simulation.propose_run", {{"id", id}, {"unknown", 1}}).ok);

    r = session.execute("simulation.run", {{"id", id}, {"prepareOnly", true}, {"label", "A"}});
    REQUIRE(r.ok);
    const std::string run = r.data.at("id");
    CHECK(r.data.at("source").at("label") == "A");
    CHECK(fs::is_regular_file(dir.path() / run / "Allrun"));

    r = session.execute("simulation.list");
    REQUIRE(r.ok);
    CHECK(r.data.at("runs").size() == 1);
    CHECK(r.data.at("runs")[0].at("sourceInDocument") == true);
    CHECK(r.data.at("runs")[0].at("source").at("partFingerprint").is_string());
    r = session.execute("simulation.status", {{"run", run}});
    REQUIRE(r.ok);
    CHECK(r.data.at("state") == "interrupted");
    r = session.execute("simulation.results", {{"run", run}});
    REQUIRE(r.ok);
    CHECK(r.data.at("summary").at("cd").is_null());
    CHECK(r.data.at("conditions").at("openfoam") == "v2506");
    CHECK_FALSE(session.execute("simulation.cancel", {{"run", run}}).ok);
    r = session.execute("simulation.field", {{"run", run}, {"kind", "surface"}});
    REQUIRE(r.ok);
    CHECK(r.data.at("available") == false);
    CHECK_FALSE(session.execute("simulation.field", {{"run", run}, {"kind", "bogus"}}).ok);
    CHECK_FALSE(session.execute("simulation.status", {{"run", "nope"}}).ok);

    r = session.execute("simulation.compare", {{"a", run}, {"b", run}});
    REQUIRE(r.ok);
    CHECK(r.data.at("sameConditions") == true);
    CHECK(r.data.at("forces").empty()); // No results yet.

    // The same run on the medium mesh: same case, so the comparison can check the mesh.
    r = session.execute("simulation.refine", {{"run", run}, {"prepareOnly", true}});
    REQUIRE(r.ok);
    const std::string refined = r.data.at("id");
    CHECK(session.simulations().caseInfo(refined)->settings.mesh == cfd::MeshLevel::Medium);
    CHECK(session.simulations().caseInfo(refined)->geometryHash == session.simulations().caseInfo(run)->geometryHash);
    CHECK(r.data.at("source").at("label") == "A");
    CHECK(session.simulations().sameCaseOnOtherMeshes(run).empty()); // Prepared, not run: not counted.
    CHECK_FALSE(session.execute("simulation.refine", {{"run", refined}, {"mesh", "medium"}}).ok);
    CHECK(session.execute("simulation.delete", {{"run", refined}}).ok);

    // Other parts in the flow changed since the run: a refinement would be another case.
    const auto ctxPart = session.execute("document.import", {{"path", AEROCAD_TEST_DATA_DIR "/box_200x80x60.stl"}});
    REQUIRE(ctxPart.ok);
    const auto ctxId = ctxPart.data.at("id").get<std::uint64_t>();
    REQUIRE(session.execute("part.scale", {{"id", ctxId}, {"factor", 0.5}}).ok);
    r = session.execute("simulation.run", {{"id", id}, {"context", {ctxId}}, {"prepareOnly", true}});
    REQUIRE(r.ok);
    const std::string withContext = r.data.at("id");
    REQUIRE(session.execute("part.scale", {{"id", ctxId}, {"factor", 1.5}}).ok);
    r = session.execute("simulation.refine", {{"run", withContext}, {"prepareOnly", true}});
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("has changed") != std::string::npos);
    CHECK(session.execute("simulation.delete", {{"run", withContext}}).ok);
    REQUIRE(session.execute("part.remove", {{"id", ctxId}}).ok);

    // The part is gone (or another document has a part with the same id): not its source.
    REQUIRE(session.execute("part.remove", {{"id", id}}).ok);
    r = session.execute("simulation.status", {{"run", run}});
    REQUIRE(r.ok);
    CHECK(r.data.at("sourceInDocument") == false);
    CHECK_FALSE(session.execute("simulation.refine", {{"run", run}, {"prepareOnly", true}}).ok);
    const auto other = session.execute("document.import", {{"path", AEROCAD_TEST_DATA_DIR "/box_200x80x60.stl"}});
    REQUIRE(other.ok);
    if (other.data.at("id").get<std::uint64_t>() == id) {
        r = session.execute("simulation.status", {{"run", run}});
        CHECK(r.data.at("sourceInDocument") == true); // Same id and same geometry.
    }

    CHECK(session.execute("simulation.delete", {{"run", run}}).ok);
    CHECK_FALSE(fs::exists(dir.path() / run));
    CHECK_FALSE(session.execute("simulation.import", {{"path", dir.path().string()}}).ok);
}

TEST_CASE("simulation results require successful completion independently of output files", "[commands][simulation]")
{
    const std::string state = GENERATE("prepared", "running", "interrupted", "cancelled", "failed", "finished");
    CAPTURE(state);
    RunsDir dir;
    commands::Session session;
    const auto part = importBox(session);
    const auto a = prepareRun(session, part), b = prepareRun(session, part);
    const fs::path caseA = dir.path() / a;
    writeResultFixture(caseA);
    writeResultFixture(dir.path() / b);
    writeOutput(dir.path() / b / "aerocad.status", "finished\n");
    if (state == "cancelled") {
        fs::remove(caseA / "aerocad.status");
        writeOutput(caseA / "aerocad.cancelled", "cancelled\n");
    } else if (state == "interrupted") {
        fs::remove(caseA / "aerocad.status");
        const auto old = fs::file_time_type::clock::now() - std::chrono::hours(1);
        for (const auto& entry : fs::recursive_directory_iterator(caseA))
            if (entry.is_regular_file())
                fs::last_write_time(entry.path(), old);
    } else {
        writeOutput(caseA / "aerocad.status", state == "failed" ? "failed: solve\n" : state + "\n");
    }
    const bool finished = state == "finished";
    const std::string expected = state == "prepared" ? "interrupted" : state;
    const auto result = session.execute("simulation.results", {{"run", a}});
    REQUIRE(result.ok);
    CHECK(result.data.at("run").at("state") == expected);
    REQUIRE(result.data.at("finalResults").at("available") == finished);
    CHECK(result.data.at("finalResults").at("reasons").empty() == finished);
    for (const char* field : {"cd", "cl", "cs", "dragN", "liftN", "sideN", "pressureDragN", "viscousDragN"})
        CHECK(result.data.at("summary").at(field).is_number() == finished);
    CHECK(result.data.at("history").at("iteration").size() == 20);
    CHECK(result.data.at("history").at("cd")[0] == 0.8);
    CHECK(result.data.at("provisional") == !finished);
    // A partial run is excluded on either side, and two partial runs cannot validate each other.
    for (const auto& pair : {std::pair{a, b}, std::pair{b, a}, std::pair{a, a}}) {
        const auto comparison = session.execute("simulation.compare", {{"a", pair.first}, {"b", pair.second}});
        REQUIRE(comparison.ok);
        CHECK(comparison.data.at("sameConditions") == true);
        CHECK(comparison.data.at("verdict") == (finished ? "comparable" : "not comparable"));
        CHECK(comparison.data.at("forces").empty() == !finished);
        CHECK(comparison.data.at("coefficientsOnReferenceAreaOfA").empty() == !finished);
        CHECK(comparison.data.at("reasons").empty() == finished);
        CHECK(comparison.data.at("states").at(pair.first == a ? "a" : "b") == expected);
    }
}

TEST_CASE("prepared simulation coefficients cannot produce final comparison values", "[commands][simulation]")
{
    RunsDir dir;
    commands::Session session;
    const auto part = importBox(session);
    const auto a = prepareRun(session, part), b = prepareRun(session, part);
    writeResultFixture(dir.path() / a, true);
    writeResultFixture(dir.path() / b, true);
    const auto comparison = session.execute("simulation.compare", {{"a", a}, {"b", b}});
    REQUIRE(comparison.ok);
    CHECK(comparison.data.at("verdict") == "not comparable");
    CHECK(comparison.data.at("coefficientsOnReferenceAreaOfA").empty());
    CHECK(comparison.data.at("forces").empty());
    CHECK(comparison.data.at("colorScales").empty());
}

TEST_CASE("finished simulation requires complete finite force and coefficient results", "[commands][simulation]")
{
    const std::string missing =
        GENERATE("coefficients", "forces", "column", "truncated", "stale", "missingSample", "nonfinite", "overflow",
                 "trailingForce", "trailingCoefficient", "invalidForceTime", "invalidCoefficientTime");
    CAPTURE(missing);
    RunsDir dir;
    commands::Session session;
    const auto part = importBox(session);
    const auto a = prepareRun(session, part), b = prepareRun(session, part);
    const auto caseA = dir.path() / a;
    writeResultFixture(caseA);
    writeResultFixture(dir.path() / b);
    writeOutput(caseA / "aerocad.status", "finished\n");
    writeOutput(dir.path() / b / "aerocad.status", "finished\n");
    const auto forces = caseA / "postProcessing/forces/0/force.dat";
    if (missing == "coefficients")
        fs::remove(caseA / "postProcessing/forceCoeffs/0/coefficient.dat");
    else if (missing == "forces")
        fs::remove(forces);
    else if (missing == "column")
        writeOutput(forces, "# Time total_x total_y total_z pressure_x\n20 1.0 0.01 0.2 0.9\n");
    else if (missing == "truncated")
        writeOutput(forces, "# Time total_x total_y total_z pressure_x viscous_x\n19 1.0 0.01 0.2 0.9 0.1\n20 1.0\n");
    else if (missing == "stale")
        writeOutput(forces, "# Time total_x total_y total_z pressure_x viscous_x\n19 1.0 0.01 0.2 0.9 0.1\n");
    else if (missing == "missingSample")
        writeOutput(forces, "# Time total_x total_y total_z pressure_x viscous_x\n20 1.0 0.01 0.2 0.9 0.1\n");
    else if (missing == "nonfinite")
        writeOutput(forces, "# Time total_x total_y total_z pressure_x viscous_x\n20 nan 0.01 0.2 0.9 0.1\n");
    else if (missing == "trailingForce")
        std::ofstream(forces, std::ios::app) << "21 1.0\n";
    else if (missing == "trailingCoefficient")
        std::ofstream(caseA / "postProcessing/forceCoeffs/0/coefficient.dat", std::ios::app) << "21 0.8\n";
    else if (missing == "invalidForceTime")
        std::ofstream(forces, std::ios::app) << "nan 1.0 0.01 0.2 0.9 0.1\n";
    else if (missing == "invalidCoefficientTime")
        std::ofstream(caseA / "postProcessing/forceCoeffs/0/coefficient.dat", std::ios::app) << "inf 0.8 0.1 0.01\n";
    else {
        std::ostringstream enormous;
        enormous << "# Time total_x total_y total_z pressure_x viscous_x\n";
        for (int i = 1; i <= 20; ++i)
            enormous << i << " 1e308 0.01 0.2 0.9 0.1\n";
        writeOutput(forces, enormous.str());
    }
    const auto result = session.execute("simulation.results", {{"run", a}});
    REQUIRE(result.ok);
    CHECK(result.data.at("run").at("state") == "finished");
    REQUIRE(result.data.at("finalResults").at("available") == false);
    CHECK_FALSE(result.data.at("finalResults").at("reasons").empty());
    CHECK(result.data.at("summary").at("cd").is_null());
    CHECK(result.data.at("summary").at("dragN").is_null());
    const auto comparison = session.execute("simulation.compare", {{"a", a}, {"b", b}});
    REQUIRE(comparison.ok);
    CHECK(comparison.data.at("verdict") == "not comparable");
    CHECK(comparison.data.at("forces").empty());
    CHECK(comparison.data.at("coefficientsOnReferenceAreaOfA").empty());
}

TEST_CASE("importing coefficient files is not evidence that a simulation finished", "[commands][simulation]")
{
    RunsDir dir;
    commands::Session session;
    const auto run = prepareRun(session, importBox(session));
    const auto caseDir = dir.path() / run;
    writeResultFixture(caseDir);
    fs::remove(caseDir / "aerocad.status");
    const auto imported = session.execute("simulation.import", {{"path", caseDir.string()}});
    REQUIRE(imported.ok);
    const auto result = session.execute("simulation.results", {{"run", imported.data.at("id")}});
    REQUIRE(result.ok);
    CHECK(result.data.at("run").at("state") != "finished");
    CHECK(result.data.at("summary").at("cd").is_null());
}

TEST_CASE("simulation.set_engine tests the remote engine before saving it", "[commands][cfd]")
{
    RunsDir dir;
    commands::Session session;
    CHECK_FALSE(session.execute("simulation.set_engine", {{"remoteHost", "user@host"}}).ok);
    CHECK_FALSE(session.execute("simulation.set_engine", {{"remoteHost", "ssh://u@h; rm -rf ~"}}).ok);
    // Nothing answers there: not saved.
    const auto bad = session.execute("simulation.set_engine", {{"remoteHost", "unix:///nonexistent/aerocad.sock"}});
    CHECK_FALSE(bad.ok);
    CHECK(cfd::engineHost().empty());
    CHECK_FALSE(fs::exists(dir.path() / "engine.json"));

    // A saved remote engine is what the environment reports (and only that engine).
    cfd::setEngineHost("ssh://arek@build-box");
    CHECK(cfd::engineHost() == "ssh://arek@build-box");
    auto r = session.execute("simulation.environment");
    REQUIRE(r.ok);
    CHECK(r.data.at("remoteHost") == "ssh://arek@build-box");
    CHECK(r.data.at("openfoamBashrc") == "");

    // Back to this computer.
    r = session.execute("simulation.set_engine", {{"remoteHost", ""}});
    REQUIRE(r.ok);
    CHECK(cfd::engineHost().empty());
    CHECK(r.data.at("remoteHost") == "");
}
