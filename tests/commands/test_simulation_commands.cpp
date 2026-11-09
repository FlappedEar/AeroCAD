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
#include <vector>

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
void writeResultFixture(const fs::path& dir, bool coefficientsOnly = false, int timeStep = 1, int samples = 20)
{
    std::ostringstream coefficients, forces, residuals;
    coefficients << "# Time Cd Cl Cs\n";
    forces << "# Time total_x total_y total_z pressure_x viscous_x\n";
    residuals << "# Time Ux_initial Uy_initial Uz_initial p_initial k_initial omega_initial\n";
    for (int i = 1; i <= samples; ++i) {
        coefficients << i * timeStep << " 0.8 0.1 0.01\n";
        forces << i * timeStep << " 1.0 0.01 0.2 0.9 0.1\n";
        residuals << i * timeStep << " 1e-6 1e-6 1e-6 1e-6 1e-6 1e-6\n";
    }
    writeOutput(dir / "postProcessing/forceCoeffs/0/coefficient.dat", coefficients.str());
    writeOutput(dir / "log.checkMesh", "Mesh stats\n    cells: 1000\nMesh OK.\nEnd\n");
    if (!coefficientsOnly) {
        writeOutput(dir / "postProcessing/forces/0/force.dat", forces.str());
        writeOutput(dir / "postProcessing/solverInfo/0/solverInfo.dat", residuals.str());
        writeOutput(dir / "log.simpleFoam", "Time = " + std::to_string(samples * timeStep) +
                                              "\ntime step continuity errors : sum local = 1e-7, global = 1e-8\n"
                                              "SIMPLE solution converged in " + std::to_string(samples * timeStep) +
                                              " iterations\nEnd\n");
    }
}

std::string prepareRun(commands::Session& session, std::uint64_t part)
{
    const auto r = session.execute("simulation.run", {{"id", part}, {"prepareOnly", true}});
    REQUIRE(r.ok);
    return r.data.at("id");
}

} // namespace

TEST_CASE("invalid run metadata does not prevent a working command session", "[commands][simulation][run-metadata]")
{
    RunsDir dir;
    writeOutput(dir.path() / "broken" / "run.json", "{\"backend\":42}");
    commands::Session session;
    const auto listed = session.execute("simulation.list");
    REQUIRE(listed.ok);
    REQUIRE(listed.data.at("runs").size() == 1);
    const auto& broken = listed.data.at("runs").at(0);
    CHECK(broken.at("state") == "failed");
    CHECK(broken.at("message").get<std::string>().find("run.json") != std::string::npos);
    const auto results = session.execute("simulation.results", {{"run", "broken"}});
    CHECK_FALSE(results.ok);
    CHECK(results.error == broken.at("message").get<std::string>());
    const auto id = prepareRun(session, importBox(session));
    CHECK(session.execute("simulation.results", {{"run", id}}).ok);
    const auto status = session.execute("simulation.status", {{"run", "broken"}});
    REQUIRE(status.ok);
    CHECK(status.data.at("state") == "failed");
}

TEST_CASE("simulation.* commands are registered with schemas", "[commands][simulation]")
{
    RunsDir dir;
    commands::Session session;
    for (const char* name :
         {"simulation.environment", "simulation.propose_run", "simulation.run", "simulation.status", "simulation.list",
          "simulation.cancel", "simulation.delete", "simulation.results", "simulation.field", "simulation.compare",
          "simulation.import", "simulation.install_engine", "simulation.refine", "simulation.set_engine",
          "simulation.export_results", "simulation.export_comparison"}) {
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
    CHECK(r.data.at("units").at("referenceArea") == "m^2"); // solver values SI, positions mm
    CHECK(r.data.at("units").at("partModel").get<std::string>().rfind("mm", 0) == 0);
    // The tunnel and the flow in model coordinates (mm), for drawing them in the view; the
    // part box is the imported box.
    const Json& part = r.data.at("partModel");
    CHECK(std::abs(part.at("max")[0].get<double>() - part.at("min")[0].get<double>() - 200.0) < 1e-6);
    CHECK(std::abs(part.at("max")[2].get<double>() - part.at("min")[2].get<double>() - 60.0) < 1e-6);
    const Json& tunnel = r.data.at("domainModel");
    for (int i = 0; i < 3; ++i) {
        CHECK(tunnel.at("min")[i].get<double>() <= part.at("min")[i].get<double>() + 1e-6);
        CHECK(tunnel.at("max")[i].get<double>() >= part.at("max")[i].get<double>() - 1e-6);
    }
    CHECK(r.data.at("flowModel")[0] == 1.0);
    CHECK(r.data.at("warnings").empty());
    // Without the (slow) engine check, as the wind tunnel panel previews it.
    auto quick = session.execute("simulation.propose_run",
                                 {{"id", id}, {"speed", 30.0}, {"flowDirection", "+X"}, {"checkEngine", false}});
    REQUIRE(quick.ok);
    CHECK(quick.data.at("environment").is_null());
    CHECK(quick.data.at("domainModel") == r.data.at("domainModel"));
    auto turned = session.execute("simulation.propose_run", {{"id", id}, {"flowDirection", "-Y"}});
    REQUIRE(turned.ok);
    CHECK(std::abs(turned.data.at("flowModel")[1].get<double>() + 1.0) < 1e-12);

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
    // A prepared run never started, so the engine that would run it is unknown (CAD-84).
    CHECK(r.data.at("differingConditions").empty());
    CHECK(r.data.at("unknownConditions") == Json::array({"engine"}));
    CHECK(r.data.at("sameConditions") == false);
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

TEST_CASE("construction planes are not walls in the flow (CAD-130)", "[commands][simulation]")
{
    RunsDir dir;
    commands::Session session;
    const auto id = importBox(session);
    auto r = session.execute("simulation.run", {{"id", id}, {"prepareOnly", true}});
    REQUIRE(r.ok);
    const std::string alone = r.data.at("id");

    auto plane = session.execute("feature.plane", {{"base", "XZ"}, {"offset", 200}});
    REQUIRE(plane.ok);
    std::uint64_t planeId = 0;
    for (const auto& p : session.document().parts())
        if (p.kind == model::PartKind::Plane)
            planeId = p.id;
    REQUIRE(planeId != 0);

    // As other geometry in the flow it is left out: the same case as the part alone.
    r = session.execute("simulation.run", {{"id", id}, {"context", {planeId}}, {"prepareOnly", true}});
    REQUIRE(r.ok);
    const std::string withPlane = r.data.at("id");
    CHECK(session.simulations().caseInfo(withPlane)->geometryHash ==
          session.simulations().caseInfo(alone)->geometryHash);

    // As the part to simulate it is refused.
    r = session.execute("simulation.run", {{"id", planeId}, {"prepareOnly", true}});
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("construction plane") != std::string::npos);
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
    writeOutput(caseA / "aerocad.engine", "v2506\n"); // Allrun records it when it starts.
    writeResultFixture(dir.path() / b);
    writeOutput(dir.path() / b / "aerocad.status", "finished\n");
    writeOutput(dir.path() / b / "aerocad.engine", "v2506\n");
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
    // CAD-158 / CAD-221: a run without final results (running, or stopped with iterations)
    // reports its tail means so far, apart from the summary; a finished run does not.
    REQUIRE(result.data.at("current").is_object() == !finished);
    if (!finished) {
        CHECK(result.data.at("current").at("cd") == 0.8);
        CHECK(result.data.at("current").at("dragN").is_number());
        CHECK(result.data.at("current").at("iterations") == 20);
    }
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

TEST_CASE("simulation.export_results writes conditions, final values and the full history as CSV",
          "[commands][simulation]")
{
    RunsDir dir;
    commands::Session session;
    const auto part = importBox(session);
    const auto a = prepareRun(session, part);
    writeResultFixture(dir.path() / a, false, 1, 450); // More rows than results() samples.
    writeOutput(dir.path() / a / "aerocad.engine", "v2506\n");
    const auto read = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const fs::path file = dir.path() / "a results.csv";

    // Not finished: says provisional, no final values, history still written.
    writeOutput(dir.path() / a / "aerocad.status", "failed: solve\n");
    auto r = session.execute("simulation.export_results", {{"run", a}, {"path", file.string()}});
    REQUIRE(r.ok);
    CHECK(r.data.at("rows") == 450);
    std::string csv = read(file);
    CHECK(csv.rfind("Quantity,Value\r\n", 0) == 0);
    CHECK(csv.find("Results,provisional (The run has not finished successfully (failed).)") != std::string::npos);
    CHECK(csv.find("\r\nCd,") == std::string::npos);

    writeOutput(dir.path() / a / "aerocad.status", "finished\n");
    r = session.execute("simulation.export_results", {{"run", a}, {"path", file.string()}});
    REQUIRE(r.ok);
    csv = read(file);
    CHECK(csv.find("\r\nState,finished\r\n") != std::string::npos);
    CHECK(csv.find("\r\nAir speed (m/s),") != std::string::npos);
    CHECK(csv.find("\r\nYaw (deg),0\r\n") != std::string::npos); // CAD-188
    CHECK(csv.find("\r\nResults,\"final, mean of the last 20 % of the iterations\"\r\n") != std::string::npos);
    CHECK(csv.find("\r\nCd,0.8\r\n") != std::string::npos);
    CHECK(csv.find("\r\nDrag (N),1\r\n") != std::string::npos);
    CHECK(csv.find("\r\nPressure drag (N),0.9\r\n") != std::string::npos);
    // As in the Results summary (CAD-187): L/D = |0.2| / 1, Cd and Cl times the reference area.
    CHECK(csv.find("\r\nEfficiency L/D,0.2\r\n") != std::string::npos);
    {
        const double area =
            session.execute("simulation.results", {{"run", a}}).data.at("conditions").at("referenceArea").get<double>();
        std::ostringstream cdA, clA;
        cdA.precision(7);
        clA.precision(7);
        cdA << "\r\nCdA (m2)," << 0.8 * area << "\r\n";
        clA << "\r\nClA (m2)," << 0.1 * area << "\r\n";
        INFO(csv.substr(0, 1500));
        CHECK(csv.find(cdA.str()) != std::string::npos);
        CHECK(csv.find(clA.str()) != std::string::npos);
    }
    CHECK(csv.find("\r\nOpenFOAM (ran),v2506\r\n") != std::string::npos);
    CHECK(csv.find("\r\n\r\nIteration,Cd,Cl,Cs,Drag (N),Lift (N),Side force (N),Pressure drag (N),Viscous drag (N)\r\n"
                   "1,0.8,0.1,0.01,1,0.2,0.01,0.9,0.1\r\n") != std::string::npos);
    CHECK(csv.find("\r\n450,0.8,0.1,0.01,1,0.2,0.01,0.9,0.1\r\n") != std::string::npos);

    // A run that never recorded its engine does not claim one.
    fs::remove(dir.path() / a / "aerocad.engine");
    REQUIRE(session.execute("simulation.export_results", {{"run", a}, {"path", file.string()}}).ok);
    CHECK(read(file).find("\r\nOpenFOAM (ran),not recorded\r\n") != std::string::npos);

    CHECK_FALSE(session.execute("simulation.export_results", {{"run", "nope"}, {"path", file.string()}}).ok);
    CHECK_FALSE(session.execute("simulation.export_results", {{"run", a}, {"path", ""}}).ok);
    CHECK_FALSE(
        session
            .execute("simulation.export_results", {{"run", a}, {"path", (dir.path() / "no/such/dir/x.csv").string()}})
            .ok);
}

TEST_CASE("simulation.export_comparison writes the A/B verdict and table as CSV (CAD-193)", "[commands][simulation]")
{
    RunsDir dir;
    commands::Session session;
    const auto part = importBox(session);
    const auto a = prepareRun(session, part), b = prepareRun(session, part);
    for (const auto& id : {a, b}) {
        writeResultFixture(dir.path() / id);
        writeOutput(dir.path() / id / "aerocad.status", "finished\n");
        writeOutput(dir.path() / id / "aerocad.engine", "v2506\n");
    }
    REQUIRE(session.execute("simulation.rename", {{"run", b}, {"label", "B"}}).ok);
    const fs::path file = dir.path() / "a vs b.csv";
    const auto r = session.execute("simulation.export_comparison", {{"a", a}, {"b", b}, {"path", file.string()}});
    REQUIRE(r.ok);
    CHECK(r.data.at("rows") == 9); // 5 forces, L/D, Cd/Cl/Cs.
    std::ifstream in(file, std::ios::binary);
    const std::string csv(std::istreambuf_iterator<char>(in), {});
    INFO(csv);
    const auto verdict =
        session.execute("simulation.compare", {{"a", a}, {"b", b}}).data.at("verdict").get<std::string>();
    CHECK(csv.rfind("Quantity,Value\r\nRun A (baseline),\"" + a + ", part ", 0) == 0);
    CHECK(csv.find("\r\nRun B (variant),\"B (" + b + "), part ") != std::string::npos);
    CHECK(csv.find("\r\nVerdict," + verdict + "\r\n") != std::string::npos);
    CHECK(csv.find("\r\nMesh check,") != std::string::npos);
    // Constant fixture histories: no swing, so an equal value is within it.
    CHECK(csv.find("\r\n\r\nQuantity,A,B,B - A,Change (%),Swing A+B,Within swing\r\n"
                   "Drag (N),1,1,0,0,0,yes\r\n") != std::string::npos);
    CHECK(csv.find("\r\nEfficiency L/D,0.2,0.2,0,0,,\r\n") != std::string::npos);
    CHECK(csv.find("\r\nCd (on the reference area of A),0.8,0.8,0,0,0,yes\r\n") != std::string::npos);

    // The per-run CSV has the swing rows too.
    const fs::path one = dir.path() / "a.csv";
    REQUIRE(session.execute("simulation.export_results", {{"run", a}, {"path", one.string()}}).ok);
    std::ifstream in1(one, std::ios::binary);
    const std::string runCsv(std::istreambuf_iterator<char>(in1), {});
    CHECK(runCsv.find("\r\nDrag swing ± (N),0\r\n") != std::string::npos);
    CHECK(runCsv.find("\r\nCs swing ±,0\r\n") != std::string::npos);

    CHECK_FALSE(session.execute("simulation.export_comparison", {{"a", a}, {"b", "nope"}, {"path", file.string()}}).ok);
    CHECK_FALSE(session.execute("simulation.export_comparison", {{"a", a}, {"b", b}, {"path", ""}}).ok);
    CHECK_FALSE(session
                    .execute("simulation.export_comparison",
                             {{"a", a}, {"b", b}, {"path", (dir.path() / "no/such/dir/x.csv").string()}})
                    .ok);
}

TEST_CASE("simulation.rename sets, trims and clears a run's label and keeps it on reload", "[commands][simulation]")
{
    RunsDir dir;
    std::string id;
    {
        commands::Session session;
        const auto part = importBox(session);
        id = prepareRun(session, part);
        auto r = session.execute("simulation.rename", {{"run", id}, {"label", "  long splitter "}});
        REQUIRE(r.ok);
        CHECK(r.data.at("label") == "long splitter");
        r = session.execute("simulation.status", {{"run", id}});
        REQUIRE(r.ok);
        CHECK(r.data.at("source").at("label") == "long splitter");
        CHECK_FALSE(session.execute("simulation.rename", {{"run", "nope"}, {"label", "x"}}).ok);
        CHECK_FALSE(session.execute("simulation.rename", {{"run", id}, {"label", "a\nb"}}).ok);
        CHECK_FALSE(session.execute("simulation.rename", {{"run", id}, {"label", std::string(81, 'x')}}).ok);
    }
    {
        // A new session reads the runs folder again: the label was saved with the run.
        commands::Session session;
        auto r = session.execute("simulation.status", {{"run", id}});
        REQUIRE(r.ok);
        CHECK(r.data.at("source").at("label") == "long splitter");
        CHECK(r.data.at("source").at("partName").get<std::string>().size() > 0);
        REQUIRE(session.execute("simulation.rename", {{"run", id}, {"label", ""}}).ok);
        r = session.execute("simulation.list");
        REQUIRE(r.ok);
        CHECK(r.data.at("runs")[0].at("source").at("label") == "");
    }
}

TEST_CASE("simulation.list and simulation.status name each run's conditions", "[commands][simulation]")
{
    RunsDir dir;
    commands::Session session;
    const auto part = importBox(session);
    auto r = session.execute("simulation.run", {{"id", part},
                                                {"speed", 35},
                                                {"flowDirection", "+X"},
                                                {"mesh", "medium"},
                                                {"ground", true},
                                                {"prepareOnly", true}});
    REQUIRE(r.ok);
    const std::string id = r.data.at("id");
    r = session.execute("simulation.status", {{"run", id}});
    REQUIRE(r.ok);
    const auto& st = r.data.at("settings");
    CHECK(st.at("speed") == 35.0);
    CHECK(st.at("flowDirection") == "+X");
    CHECK(st.at("mesh") == "medium");
    CHECK(st.at("ground") == true);
    r = session.execute("simulation.list");
    REQUIRE(r.ok);
    REQUIRE(r.data.at("runs").size() == 1);
    CHECK(r.data.at("runs")[0].at("settings").at("speed") == 35.0);
    // A run folder without case information lists no settings.
    std::filesystem::remove(dir.path() / id / "case.json");
    r = session.execute("simulation.list");
    REQUIRE(r.ok);
    CHECK(r.data.at("runs")[0].at("settings").is_null());
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
    writeOutput(caseA / "aerocad.engine", "v2506\n");
    writeOutput(dir.path() / b / "aerocad.status", "finished\n");
    writeOutput(dir.path() / b / "aerocad.engine", "v2506\n");
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

TEST_CASE("simulation final results reject nonfinite samples before the averaging window", "[commands][simulation]")
{
    const std::string column = GENERATE("Cd", "Cl", "Cs", "total_x", "total_y", "total_z", "pressure_x", "viscous_x");
    const int badIteration = GENERATE(1, 10);
    const std::string badValue = GENERATE("nan", "inf", "-inf");
    CAPTURE(column, badIteration, badValue);
    RunsDir dir;
    commands::Session session;
    const auto part = importBox(session);
    const auto a = prepareRun(session, part), b = prepareRun(session, part);
    const auto caseA = dir.path() / a;
    writeResultFixture(caseA);
    writeResultFixture(dir.path() / b);
    writeOutput(caseA / "aerocad.status", "finished\n");
    writeOutput(caseA / "aerocad.engine", "v2506\n");
    writeOutput(dir.path() / b / "aerocad.status", "finished\n");
    writeOutput(dir.path() / b / "aerocad.engine", "v2506\n");

    const bool coefficient = column[0] == 'C';
    const std::vector<std::string> columns = coefficient ? std::vector<std::string>{"Cd", "Cl", "Cs"}
        : std::vector<std::string>{"total_x", "total_y", "total_z", "pressure_x", "viscous_x"};
    const std::vector<std::string> values = coefficient ? std::vector<std::string>{"0.8", "0.1", "0.01"}
        : std::vector<std::string>{"1.0", "0.01", "0.2", "0.9", "0.1"};
    std::ostringstream history;
    history << "# Time";
    for (const auto& name : columns)
        history << ' ' << name;
    history << '\n';
    for (int i = 1; i <= 20; ++i) {
        history << i;
        for (std::size_t c = 0; c < columns.size(); ++c)
            history << ' ' << (i == badIteration && columns[c] == column ? badValue : values[c]);
        history << '\n';
    }
    writeOutput(caseA / (coefficient ? "postProcessing/forceCoeffs/0/coefficient.dat"
                                    : "postProcessing/forces/0/force.dat"), history.str());
    const auto result = session.execute("simulation.results", {{"run", a}});
    REQUIRE(result.ok);
    CHECK(result.data.at("finalResults").at("available") == false);
    CHECK_FALSE(result.data.at("finalResults").at("reasons").empty());
    CHECK(result.data.at("summary").at("cd").is_null());
    CHECK(result.data.at("summary").at("dragN").is_null());
    CHECK(result.data.at("history").at("iteration").size() == 20);
    CHECK(result.data.at("provisional") == true);
    if (!coefficient)
        CHECK(result.data.at("quality").at("status") == "ok"); // The final-data gate is independent of quality.
    const auto comparison = session.execute("simulation.compare", {{"a", a}, {"b", b}});
    REQUIRE(comparison.ok);
    CHECK(comparison.data.at("verdict") == "not comparable");
    CHECK(comparison.data.at("forces").empty());
    CHECK(comparison.data.at("coefficientsOnReferenceAreaOfA").empty());

    // Correcting the file restores finite final results, without replacing the run.
    writeResultFixture(caseA);
    const auto repaired = session.execute("simulation.results", {{"run", a}});
    REQUIRE(repaired.ok);
    CHECK(repaired.data.at("finalResults").at("available") == true);
    CHECK(repaired.data.at("summary").at("dragN") == 1.0);
    CHECK(session.execute("simulation.compare", {{"a", a}, {"b", b}}).data.at("verdict") == "comparable");
}

TEST_CASE("simulation histories retain malformed middle rows until explicitly replaced", "[commands][simulation]")
{
    const std::string repair = GENERATE("none", "repeat", "restart", "sparse");
    const std::string corruption = GENERATE("truncated", "nonfinite");
    CAPTURE(repair, corruption);
    RunsDir dir;
    commands::Session session;
    const auto part = importBox(session);
    const auto a = prepareRun(session, part), b = prepareRun(session, part);
    const auto caseA = dir.path() / a;
    writeResultFixture(caseA, false, repair == "sparse" ? 2 : 1, 100);
    writeResultFixture(dir.path() / b, false, repair == "sparse" ? 2 : 1, 100);
    writeOutput(caseA / "aerocad.status", "finished\n");
    writeOutput(caseA / "aerocad.engine", "v2506\n");
    writeOutput(dir.path() / b / "aerocad.status", "finished\n");
    writeOutput(dir.path() / b / "aerocad.engine", "v2506\n");
    if (repair != "sparse") {
        for (const bool coefficient : {true, false}) {
            std::ostringstream history;
            history << (coefficient ? "# Time Cd Cl Cs\n" : "# Time total_x total_y total_z pressure_x viscous_x\n");
            const std::string values = coefficient ? " 0.8 0.1 0.01\n" : " 1.0 0.01 0.2 0.9 0.1\n";
            const std::string bad = corruption == "truncated" ? " 0.8\n"
                : coefficient ? " nan 0.1 0.01\n" : " nan 0.01 0.2 0.9 0.1\n";
            for (int i = 1; i <= 100; ++i)
                history << i << (i == 50 ? bad : values);
            if (repair != "none")
                for (int i = repair == "repeat" ? 50 : 25; i <= 100; ++i)
                    history << i << values;
            writeOutput(caseA / (coefficient ? "postProcessing/forceCoeffs/0/coefficient.dat"
                                            : "postProcessing/forces/0/force.dat"), history.str());
        }
    }
    const auto result = session.execute("simulation.results", {{"run", a}});
    REQUIRE(result.ok);
    const bool available = repair != "none";
    CHECK(result.data.at("finalResults").at("available") == available);
    CHECK(result.data.at("summary").at("dragN").is_number() == available);
    const auto comparison = session.execute("simulation.compare", {{"a", a}, {"b", b}});
    REQUIRE(comparison.ok);
    CHECK(comparison.data.at("verdict") == (available ? "comparable" : "not comparable"));
}

TEST_CASE("simulation progress survives an uncommitted timestamp prefix", "[commands][simulation]")
{
    RunsDir dir;
    commands::Session session;
    const auto run = prepareRun(session, importBox(session));
    const auto caseDir = dir.path() / run;
    writeResultFixture(caseDir, false, 1, 120);
    writeOutput(caseDir / "aerocad.status", "finished\n");
    writeOutput(caseDir / "aerocad.engine", "v2506\n");
    const auto coefficients = caseDir / "postProcessing/forceCoeffs/0/coefficient.dat";
    {
        std::ofstream append(coefficients, std::ios::app);
        append << "12";
    }
    const auto result = session.execute("simulation.results", {{"run", run}});
    REQUIRE(result.ok);
    CHECK(result.data.at("run").at("iteration") == 120);
    CHECK(result.data.at("history").at("iteration").size() == 120);
    CHECK(result.data.at("history").at("iteration").back() == 120);
    CHECK(result.data.at("finalResults").at("available") == false);
    CHECK(result.data.at("summary").at("cd").is_null());

    // Completing that same write publishes iteration 121 on the next refresh.
    {
        std::ofstream append(coefficients, std::ios::app);
        append << "1 0.8 0.1 0.01\n";
    }
    const auto refreshed = session.execute("simulation.results", {{"run", run}});
    REQUIRE(refreshed.ok);
    CHECK(refreshed.data.at("run").at("iteration") == 121);
    CHECK(refreshed.data.at("history").at("iteration").size() == 121);
    CHECK(refreshed.data.at("history").at("iteration").back() == 121);
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

TEST_CASE("A run warns about a part at an implausible size (CAD-109)", "[commands][simulation]")
{
    RunsDir dir;
    commands::Session session;
    const auto id = importBox(session);

    // 200 mm: plausible, no warning.
    auto r = session.execute("simulation.propose_run", {{"id", id}});
    REQUIRE(r.ok);
    CHECK(r.data.at("warnings").empty());

    // The same box read from a file in metres as millimetres: 0.2 mm long.
    REQUIRE(session.execute("part.convert_units", {{"id", id}, {"from", "mm"}, {"to", "m"}}).ok);
    r = session.execute("simulation.propose_run", {{"id", id}});
    REQUIRE(r.ok);
    REQUIRE(r.data.at("warnings").size() == 1);
    const std::string warning = r.data.at("warnings")[0];
    CHECK(warning.find("only 0.2 mm long") != std::string::npos);
    CHECK(warning.find("Scan Tools") != std::string::npos);

    // simulation.run reports it too and still prepares the case.
    r = session.execute("simulation.run", {{"id", id}, {"prepareOnly", true}});
    REQUIRE(r.ok);
    REQUIRE(r.data.at("warnings").size() == 1);
    CHECK(r.data.at("warnings")[0] == warning);
}

TEST_CASE("A run warns about a part that is not closed (CAD-145)", "[commands][simulation]")
{
    RunsDir dir;
    commands::Session session;
    const auto warningsOf = [&](std::uint64_t id) {
        const auto r = session.execute("simulation.propose_run", {{"id", id}});
        INFO(r.error);
        REQUIRE(r.ok);
        return r.data.at("warnings");
    };

    // A closed mesh: no warning.
    CHECK(warningsOf(importBox(session)).empty());

    // Solids from the timeline, curved faces and a fillet included: their tessellation is closed.
    auto r = session.execute(
        "sketch.create",
        {{"plane", "XY"},
         {"curves", Json::array({{{"type", "center_rectangle"}, {"center", {0, 0}}, {"width", 60}, {"height", 40}},
                                 {{"type", "circle"}, {"center", {100, 0}}, {"radius", 20}}})}});
    REQUIRE(r.ok);
    r = session.execute("feature.extrude", {{"sketch", r.data.at("id")}, {"distance", 30}});
    REQUIRE(r.ok);
    REQUIRE_FALSE(r.data.at("bodies").empty());
    const auto bodyId = r.data.at("id").get<std::uint64_t>();
    REQUIRE(session
                .execute("feature.fillet",
                         {{"body", bodyId}, {"edges", Json::array({{{"point", {30, 20, 15}}}})}, {"radius", 5}})
                .ok);
    const auto parts = session.execute("document.parts"); // Keeps the result alive while iterating.
    int bodies = 0;
    for (const auto& p : parts.data.at("parts"))
        if (p.at("kind") == "body") {
            ++bodies;
            CHECK(warningsOf(p.at("id").get<std::uint64_t>()).empty());
        }
    CHECK(bodies > 0);

    // A raw scan shell, open underneath: warned, with the number of open edges and the fix.
    r = session.execute("document.import", {{"path", AEROCAD_TEST_DATA_DIR "/models/dome_scan.stl"}});
    REQUIRE(r.ok);
    const auto scan = r.data.at("id").get<std::uint64_t>();
    const Json w = warningsOf(scan);
    REQUIRE(w.size() == 1);
    const std::string warning = w[0];
    CHECK(warning.find("not closed (329 open edges") != std::string::npos);
    CHECK(warning.find("Repair") != std::string::npos);
    r = session.execute("simulation.run", {{"id", scan}, {"prepareOnly", true}});
    REQUIRE(r.ok);
    REQUIRE(r.data.at("warnings").size() == 1);
    CHECK(r.data.at("warnings")[0] == warning);

    // Filling the holes as the warning says removes it.
    REQUIRE(session.execute("mesh.repair", {{"id", scan}, {"maxHoleEdges", 329}}).ok);
    CHECK(warningsOf(scan).empty());
}
