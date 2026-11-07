// SPDX-License-Identifier: GPL-3.0-or-later
// Canards on a scanned bumper, end to end through the commands: fins on the noisy skin,
// root fillets, several fins in one go, the mirrored pair and the files for printing.
#include "SyntheticBumper.hpp"

#include "aerocad/commands/Session.hpp"
#include "aerocad/core/Align.hpp"
#include "aerocad/core/MeshCheck.hpp"
#include "aerocad/core/Path.hpp"
#include "aerocad/core/Symmetry.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <random>
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
                                          {{"sketch", sk.at("id")}, {"thickness", 3.0}, {"rootFillet", 2.0}});
    INFO(filleted.toJson().dump());
    REQUIRE(filleted.ok);
    // The full radius, or (on a noisy skin, a little less) a smaller one and a warning.
    const double r = filleted.data.at("rootFillet").get<double>();
    CHECK(r >= 1.2);
    CHECK(filleted.data.at("warnings").empty() == (r == 2.0));
    const model::Part& fin = scene.part(filleted.data);
    CHECK(core::checkMesh(core::weldVertices(*fin.mesh)).watertight());
    scene.rootOnSkin(fin, deepest, nearest);
    CHECK(deepest > -0.6);
    CHECK(nearest < 0.6);
    // Root chord about 80 mm on both sides: (1 - pi/4) r^2 of section each.
    const double added = filleted.data.at("volume").get<double>() - plain.data.at("volume").get<double>();
    const double expected = (1.0 - 3.14159 / 4.0) * r * r * 2.0 * 80.0;
    CHECK(added > 0.5 * expected);
    CHECK(added < 1.6 * expected);
}

TEST_CASE("A stacked pair of canards in one go, each root on the skin, one undo", "[commands][fit][canard]")
{
    BumperScene scene;
    const Json sk = scene.sketchAt(40.0, 170.0);
    // The bumper leans in towards the bottom: the second fin, 60 mm down the sketch normal,
    // has to move in to reach the skin.
    auto pair = scene.session.execute("surface.fin_from_sketch", {{"sketch", sk.at("id")},
                                                                  {"thickness", 3.0},
                                                                  {"rootFillet", 2.0},
                                                                  {"count", 2},
                                                                  {"spacing", -60.0}});
    INFO(pair.toJson().dump());
    REQUIRE(pair.ok);
    CHECK(pair.data.at("warnings").empty());
    const auto& fins = pair.data.at("fins");
    REQUIRE(fins.size() == 2);
    const double z0 = (fins[0].at("bounds").at("min").at(2).get<double>() + fins[0].at("bounds").at("max").at(2).get<double>()) / 2;
    const double z1 = (fins[1].at("bounds").at("min").at(2).get<double>() + fins[1].at("bounds").at("max").at(2).get<double>()) / 2;
    CHECK(z0 - z1 == Approx(60.0 * sk.at("params").at("plane").at("normal").at(2).get<double>()).margin(3.0));
    for (const auto& f : fins) {
        CHECK(f.at("rootFillet").get<double>() == 2.0);
        const model::Part& fin = scene.part(f);
        CHECK(core::checkMesh(core::weldVertices(*fin.mesh)).watertight());
        double deepest = 0, nearest = 0;
        scene.rootOnSkin(fin, deepest, nearest);
        CHECK(deepest > -0.6);
        CHECK(nearest < 0.6);
    }
    // Same fin, same size: the copy is the same outline.
    CHECK(fins[1].at("volume").get<double>() == Approx(fins[0].at("volume").get<double>()).epsilon(0.05));

    const auto parts = scene.session.document().parts().size();
    REQUIRE(scene.session.execute("edit.undo", Json::object()).ok);
    CHECK(scene.session.document().parts().size() == parts - 2);
    REQUIRE(scene.session.execute("edit.redo", Json::object()).ok);
    CHECK(scene.session.document().parts().size() == parts);
}

TEST_CASE("The centre plane of a scanned bumper is found, also turned and moved", "[canard][symmetry]")
{
    const testing::SyntheticBumper bumper;
    core::TriangleMesh mesh = bumper.mesh();
    auto plane = core::findSymmetryPlane(mesh);
    REQUIRE(plane);
    CHECK(std::abs(plane->normal.x) > 0.999);
    CHECK(std::abs(plane->point.dot(plane->normal)) < 1.5);
    CHECK(plane->match > 0.8);

    // Turned 10 degrees about Z and moved: the plane goes with it.
    const core::Affine pose{core::Mat3::rotation({0, 0, 1}, 10.0 * 3.14159265358979 / 180.0), {120, -40, 15}};
    core::transformMesh(mesh, pose);
    plane = core::findSymmetryPlane(mesh);
    REQUIRE(plane);
    const core::Vec3 expected = pose.linear * core::Vec3{1, 0, 0};
    CHECK(std::abs(plane->normal.dot(expected)) > 0.999);
    CHECK(std::abs((pose.translation - plane->point).dot(plane->normal)) < 1.5);
}

TEST_CASE("Canards on both sides: mirrored in the scan's centre plane, each on its own skin", "[commands][fit][canard]")
{
    BumperScene scene;
    const Json sk = scene.sketchAt(30.0, 120.0);
    auto both = scene.session.execute("surface.fin_from_sketch", {{"sketch", sk.at("id")},
                                                                  {"thickness", 3.0},
                                                                  {"rootFillet", 2.0},
                                                                  {"count", 2},
                                                                  {"spacing", -55.0},
                                                                  {"mirror", true}});
    INFO(both.toJson().dump());
    REQUIRE(both.ok);
    CHECK(both.data.at("warnings").empty());
    CHECK(both.data.at("mirror").at("source") == "scan");
    CHECK(std::abs(both.data.at("mirror").at("normal").at(0).get<double>()) > 0.999);
    const auto& fins = both.data.at("fins");
    REQUIRE(fins.size() == 4);
    for (std::size_t k = 0; k < 4; ++k) {
        const model::Part& fin = scene.part(fins[k]);
        CHECK(core::checkMesh(core::weldVertices(*fin.mesh)).watertight());
        double deepest = 0, nearest = 0;
        scene.rootOnSkin(fin, deepest, nearest);
        CHECK(deepest > -0.6);
        CHECK(nearest < 0.6);
        // Right side first, then the left.
        const double cx = (fins[k].at("bounds").at("min").at(0).get<double>() + fins[k].at("bounds").at("max").at(0).get<double>()) / 2;
        CHECK((k < 2 ? cx > 300.0 : cx < -300.0));
    }
    // Mirror images: the same x extent with the sign turned, the same volume.
    for (std::size_t k = 0; k < 2; ++k) {
        CHECK(fins[k + 2].at("bounds").at("min").at(0).get<double>() ==
              Approx(-fins[k].at("bounds").at("max").at(0).get<double>()).margin(2.0));
        CHECK(fins[k + 2].at("volume").get<double>() == Approx(fins[k].at("volume").get<double>()).epsilon(0.05));
        CHECK(fins[k + 2].at("name").get<std::string>().find("(mirror)") != std::string::npos);
    }
}

TEST_CASE("Canards exported for printing: one STL and one STEP per fin", "[commands][fit][canard][export]")
{
    BumperScene scene;
    const Json sk = scene.sketchAt(30.0, 120.0);
    auto fins = scene.session.execute("surface.fin_from_sketch",
                                      {{"sketch", sk.at("id")}, {"thickness", 3.0}, {"rootFillet", 2.0}, {"count", 2}, {"spacing", -55.0}});
    REQUIRE(fins.ok);
    const auto dir = std::filesystem::temp_directory_path() / ("aerocad_print_" + std::to_string(std::random_device{}()));
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{dir};

    // The scan is not a solid: asked for, it stops the export before anything is written.
    auto refused = scene.session.execute("document.export_for_print",
                                         {{"folder", core::pathToUtf8(dir)}, {"ids", {scene.scanId, fins.data.at("id")}}});
    CHECK_FALSE(refused.ok);
    CHECK((!std::filesystem::exists(dir) || std::filesystem::is_empty(dir)));

    // By default: the visible solids, i.e. the two fins (not the scan).
    auto out = scene.session.execute("document.export_for_print", {{"folder", core::pathToUtf8(dir)}});
    INFO(out.toJson().dump());
    REQUIRE(out.ok);
    CHECK(out.data.at("parts") == 2);
    REQUIRE(out.data.at("files").size() == 4);
    for (const auto& f : out.data.at("files")) {
        const std::string path = f.at("path").get<std::string>();
        REQUIRE(std::filesystem::exists(core::pathFromUtf8(path)));
        auto back = scene.session.execute("document.import", {{"path", path}});
        INFO(path);
        REQUIRE(back.ok);
        const auto* part = scene.session.document().part(back.data.at("id").get<model::PartId>());
        REQUIRE(part != nullptr);
        CHECK(core::checkMesh(core::weldVertices(*part->mesh)).watertight());
        const auto& fin = fins.data.at("fins")[f.at("id") == fins.data.at("fins")[0].at("id") ? 0 : 1];
        CHECK(core::checkMesh(core::weldVertices(*part->mesh)).signedVolume ==
              Approx(fin.at("volume").get<double>()).epsilon(0.02));
        if (f.at("format") == "stl")  // finer than the view's mesh, for a smooth print
            CHECK(f.at("triangles").get<std::size_t>() > fin.at("triangles").get<std::size_t>());
    }
}
