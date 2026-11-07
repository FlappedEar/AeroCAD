// SPDX-License-Identifier: GPL-3.0-or-later
// Validation of the wind tunnel preset on the 25 degree Ahmed body (CAD-52). The case,
// the experimental values and the acceptance criteria are in docs/cfd-validation.md;
// they were set before the first run. The OpenFOAM part is hidden ([.validation]):
// ctest -L validation runs it (needs Docker or a local OpenFOAM v2506, tens of minutes).
#include "aerocad/cfd/RunManager.hpp"
#include "aerocad/geometry/Modeling.hpp"
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/geometry/Tessellation.hpp"
#include "aerocad/geometry/Transform.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace aerocad;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

constexpr double kLength = 1044.0;
constexpr double kWidth = 389.0;
constexpr double kHeight = 288.0;
constexpr double kFrontRadius = 100.0;
constexpr double kSlantLength = 222.0;
constexpr double kSlantDegrees = 25.0;
constexpr double kClearance = 50.0;
constexpr double kPi = 3.14159265358979323846;

/// The 25 degree Ahmed body in mm: front at x = 0 (air flows along +X), centred on y = 0,
/// underside kClearance above z = 0 (the floor). No stilts (see docs/cfd-validation.md).
geometry::Shape ahmedBody()
{
    geometry::Shape body =
        geometry::translated(geometry::makeBox(kLength, kWidth, kHeight), {0.0, -kWidth / 2, kClearance});
    // Round the four edges of the front face.
    std::vector<std::size_t> front;
    for (const auto& e : geometry::edges(body))
        if (std::abs(e.start.x) < 1e-6 && std::abs(e.end.x) < 1e-6)
            front.push_back(e.index);
    REQUIRE(front.size() == 4);
    body = geometry::fillet(body, front, kFrontRadius);
    // Slant: cut away everything above a plane through the rear top edge, dropping at
    // 25 degrees towards the rear over a slant face of kSlantLength.
    const double a = kSlantDegrees * kPi / 180.0;
    const double top = kClearance + kHeight;
    const core::Vec3 start{kLength - kSlantLength * std::cos(a), 0.0, top};
    geometry::Shape cutter = geometry::makeBox(2 * kSlantLength, 2 * kWidth, kHeight);
    cutter = geometry::rotated(cutter, {0, 0, 0}, {0, 1, 0}, kSlantDegrees); // +X -> (cos a, 0, -sin a)
    cutter = geometry::translated(cutter, {start.x, -kWidth, start.z});
    return geometry::boolean(body, cutter, geometry::BooleanOp::Cut);
}

core::TriangleMesh ahmedMesh()
{
    geometry::TessellationParams p;
    p.relativeLinearDeflection = 0.0002; // About 0.2 mm on the 1.1 m body.
    p.angularDeflection = 0.15;
    return geometry::tessellate(ahmedBody(), p);
}

class TempDir {
public:
    TempDir()
    {
        std::random_device rd;
        m_path = fs::temp_directory_path() / ("aerocad_ahmed_" + std::to_string(rd()));
        fs::create_directories(m_path);
    }
    ~TempDir()
    {
        std::error_code ec;
        if (std::getenv("AEROCAD_KEEP_VALIDATION") == nullptr)
            fs::remove_all(m_path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

} // namespace

TEST_CASE("Ahmed body geometry", "[cfd][validation-geometry]")
{
    const geometry::Shape body = ahmedBody();
    REQUIRE(geometry::isValid(body));
    const core::TriangleMesh mesh = ahmedMesh();
    const auto b = mesh.bounds();
    CHECK(b.min.x == Approx(0.0).margin(0.01));
    CHECK(b.max.x == Approx(kLength).margin(0.01));
    CHECK(b.size().y == Approx(kWidth).margin(0.01));
    CHECK(b.min.z == Approx(kClearance).margin(0.01));
    CHECK(b.max.z == Approx(kClearance + kHeight).margin(0.01));

    // Volume: box, minus the slant wedge, minus what the four front fillets remove
    // (each a (1 - pi/4) r^2 strip along its edge; the corners, where two strips meet,
    // are counted twice by this estimate, so allow a small difference).
    const double a = kSlantDegrees * kPi / 180.0;
    const double wedge = 0.5 * kSlantLength * std::cos(a) * kSlantLength * std::sin(a) * kWidth;
    const double strip = (1.0 - kPi / 4.0) * kFrontRadius * kFrontRadius;
    const double estimate = kLength * kWidth * kHeight - wedge - strip * (2 * kWidth + 2 * kHeight);
    CHECK(body.volume() == Approx(estimate).epsilon(0.01));

    // Slant face: the rear top corner sits kSlantLength sin(a) below the roof.
    double rearTop = -1e9;
    for (const auto& v : mesh.vertices())
        if (std::abs(v.x - kLength) < 1e-6)
            rearTop = std::max(rearTop, v.z);
    CHECK(rearTop == Approx(kClearance + kHeight - kSlantLength * std::sin(a)).margin(0.01));
}

TEST_CASE("OpenFOAM: Ahmed body validation", "[.validation]")
{
    const auto env = cfd::detectEnvironment();
    if (!env.recommended)
        SKIP("No simulation engine: " + env.message);

    const char* levelsEnv = std::getenv("AEROCAD_VALIDATION_MESH");
    std::vector<std::string> levels;
    std::stringstream list(levelsEnv != nullptr ? levelsEnv : "coarse,medium");
    for (std::string level; std::getline(list, level, ',');)
        levels.push_back(level);

    TempDir tmp;
    cfd::RunManager runs(tmp.path());
    cfd::CaseGeometry g;
    g.part = ahmedMesh();

    for (const std::string& level : levels) {
        cfd::Settings s = cfd::settingsFromJson({{"speed", 40.0},
                                                 {"flowDirection", "+X"},
                                                 {"mesh", level},
                                                 {"ground", true},
                                                 {"groundMoving", false},
                                                 {"groundHeight", 0.0},
                                                 {"referenceArea", 0.112}});
        cfd::RunSource source;
        source.partName = "ahmed-25";
        source.label = level;
        const auto started = std::chrono::steady_clock::now();
        const std::string id = runs.start(g, s, source);
        auto st = runs.status(id);
        while (st->state == cfd::RunState::Running &&
               std::chrono::steady_clock::now() - started < std::chrono::hours(6)) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            st = runs.status(id);
        }
        INFO(level << ": " << st->message);
        REQUIRE(st->state == cfd::RunState::Finished);
        const auto minutes =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() / 60.0;
        const auto r = runs.results(id);
        const auto& sum = r.at("summary");
        const auto& q = r.at("quality");
        const double cd = sum.at("cd").get<double>();
        const double cl = sum.at("cl").get<double>();
        std::cout << "Ahmed 25 deg, " << level << " mesh: Cd " << cd << ", Cl " << cl << ", drag "
                  << sum.at("dragN").get<double>() << " N, cells " << q.value("cells", nlohmann::json()).dump()
                  << ", quality " << q.at("status").get<std::string>() << ", iterations "
                  << sum.value("iterations", 0) << ", " << minutes << " min, run " << tmp.path() / id << "\n"
                  << "  quality: " << q.dump() << "\n";

        // Acceptance criteria (docs/cfd-validation.md), medium mesh only.
        if (level == "medium") {
            CHECK(cd >= 0.251);
            CHECK(cd <= 0.339);
            CHECK(q.at("status").get<std::string>() != "warning");
            CHECK(cl > 0.0);
        }
    }
}
