// SPDX-License-Identifier: GPL-3.0-or-later
// Canards on a scanned bumper, end to end through the commands: fins on the noisy skin,
// root fillets, several fins in one go, the mirrored pair and the files for printing.
#include "SyntheticBumper.hpp"

#include "aerocad/commands/Session.hpp"
#include "aerocad/core/MeshCheck.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>

using namespace aerocad;
using commands::Json;
using Catch::Approx;

namespace {

/// A swept canard in sketch coordinates (x out of the skin, y along the chord): root chord
/// 80 mm reaching 4 mm into the skin, tip 45 mm out.
Json canardOutline()
{
    auto line = [](double x0, double y0, double x1, double y1) {
        return Json{{"type", "line"}, {"start", {x0, y0}}, {"end", {x1, y1}}};
    };
    return Json::array({line(-4, -40, 45, -5), line(45, -5, 45, 15), line(45, 15, -4, 40), line(-4, 40, -4, -40)});
}

struct BumperScene {
    testing::SyntheticBumper bumper;
    commands::Session session;
    model::PartId scanId = 0;

    BumperScene()
    {
        model::Part scan;
        scan.name = "Bumper";
        scan.mesh = std::make_shared<const core::TriangleMesh>(bumper.mesh());
        scanId = session.document().addPart(scan);
    }

    /// A canard sketch across the skin on the right corner, `degrees` round from the front.
    Json sketchAt(double degrees, double z, double angle = 12.0)
    {
        const core::Vec3 p = bumper.onCorner(degrees, z);
        auto sk = session.execute("sketch.create",
                                  {{"surface", {{"part", scanId}, {"point", {p.x, p.y, p.z}}, {"across", true}, {"angle", angle}}},
                                   {"curves", canardOutline()}});
        INFO(sk.toJson().dump());
        REQUIRE(sk.ok);
        return sk.data;
    }

    [[nodiscard]] const model::Part& part(const Json& result) const
    {
        const auto* p = session.document().part(result.at("id").get<model::PartId>());
        REQUIRE(p != nullptr);
        return *p;
    }

    /// How far the part reaches into the clean skin (negative: into it), and how close
    /// its closest point comes to the skin from outside.
    void rootOnSkin(const model::Part& p, double& deepest, double& nearest) const
    {
        deepest = 1e9;
        nearest = 1e9;
        for (const auto& q : p.mesh->vertices()) {
            const double d = bumper.outside(q);
            deepest = std::min(deepest, d);
            nearest = std::min(nearest, std::abs(d));
        }
    }
};

} // namespace

TEST_CASE("Canard on a noisy scanned bumper corner, with a root fillet", "[commands][fit][canard]")
{
    BumperScene scene;
    const Json sk = scene.sketchAt(35.0, 110.0);

    auto plain = scene.session.execute("surface.fin_from_sketch", {{"sketch", sk.at("id")}, {"thickness", 3.0}});
    INFO(plain.toJson().dump());
    REQUIRE(plain.ok);
    double deepest = 0, nearest = 0;
    scene.rootOnSkin(scene.part(plain.data), deepest, nearest);
    // Root on the noisy skin (0.15 mm noise), never cut at the layer behind it.
    CHECK(deepest > -0.6);
    CHECK(nearest < 0.6);

    auto filleted = scene.session.execute("surface.fin_from_sketch",
                                          {{"sketch", sk.at("id")}, {"thickness", 3.0}, {"rootFillet", 3.0}});
    INFO(filleted.toJson().dump());
    REQUIRE(filleted.ok);
    CHECK(filleted.data.at("warnings").empty());
    CHECK(filleted.data.at("rootFillet").get<double>() == 3.0);
    const model::Part& fin = scene.part(filleted.data);
    CHECK(core::checkMesh(core::weldVertices(*fin.mesh)).watertight());
    scene.rootOnSkin(fin, deepest, nearest);
    CHECK(deepest > -0.6);
    CHECK(nearest < 0.6);
    // Root chord about 80 mm on both sides: (1 - pi/4) r^2 of section each.
    const double added = filleted.data.at("volume").get<double>() - plain.data.at("volume").get<double>();
    const double expected = (1.0 - 3.14159 / 4.0) * 9.0 * 2.0 * 80.0;
    CHECK(added > 0.5 * expected);
    CHECK(added < 1.6 * expected);
}
