// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cfd/RunManager.hpp"
#include "aerocad/commands/Session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <random>
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

} // namespace

TEST_CASE("simulation.* commands are registered with schemas", "[commands][simulation]")
{
    RunsDir dir;
    commands::Session session;
    for (const char* name : {"simulation.environment", "simulation.propose_run", "simulation.run", "simulation.status",
                             "simulation.list", "simulation.cancel", "simulation.delete", "simulation.results",
                             "simulation.field", "simulation.compare", "simulation.import",
                             "simulation.install_engine", "simulation.refine"}) {
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
