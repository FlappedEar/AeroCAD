// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>

using namespace aerocad;
using commands::Json;
using Catch::Approx;

namespace {

Json run(commands::Session& s, const std::string& name, const Json& params = Json::object())
{
    const auto r = s.execute(name, params);
    INFO(name << " " << params.dump() << " -> " << r.error);
    REQUIRE(r.ok);
    return r.data;
}

std::size_t countKind(const commands::Session& s, model::PartKind kind)
{
    std::size_t n = 0;
    for (const auto& p : s.document().parts())
        n += p.kind == kind ? 1 : 0;
    return n;
}

} // namespace

TEST_CASE("Design commands: sketch, extrude, fillet with undo/redo", "[commands][cad]")
{
    commands::Session s;
    run(s, "parameter.set", {{"name", "w"}, {"expression", "30 mm"}});
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "center_rectangle"},
                                                  {"center", {0, 0}},
                                                  {"width", "w"},
                                                  {"height", "w"}}})}});
    CHECK(sk.at("profiles") == 1);
    CHECK(sk.at("name") == "Sketch1");
    const auto sketchId = sk.at("id").get<std::uint64_t>();

    const Json ex = run(s, "feature.extrude", {{"sketch", sketchId}, {"distance", 10}});
    const auto bodyId = ex.at("id").get<std::uint64_t>();
    REQUIRE(ex.at("bodies").size() == 1);
    CHECK(ex.at("bodies")[0].at("volume").get<double>() == Approx(9000));
    CHECK(ex.at("bodies")[0].at("partId").get<std::uint64_t>() != 0);
    CHECK(countKind(s, model::PartKind::Body) == 1);
    CHECK(countKind(s, model::PartKind::Sketch) == 1);

    const Json edges = run(s, "body.edges", {{"body", bodyId}});
    CHECK(edges.at("edges").size() == 12);
    const Json faces = run(s, "body.faces", {{"body", "Body1"}});
    CHECK(faces.at("faces").size() == 6);

    run(s, "feature.fillet", {{"body", bodyId}, {"edges", Json::array({{{"point", {15, 15, 5}}}})}, {"radius", 3}});
    const double pi = std::numbers::pi;
    const double filleted = 9000 - (9 - 9 * pi / 4) * 10;
    CHECK(s.design().bodies().at(bodyId).shape.volume() == Approx(filleted).epsilon(1e-6));

    run(s, "parameter.set", {{"name", "w"}, {"expression", "40"}});
    CHECK(s.design().bodies().at(bodyId).shape.volume() == Approx(16000 - (9 - 9 * pi / 4) * 10).epsilon(1e-6));

    run(s, "edit.undo"); // w back to 30
    CHECK(s.design().bodies().at(bodyId).shape.volume() == Approx(filleted).epsilon(1e-6));
    run(s, "edit.undo"); // remove fillet
    CHECK(s.design().bodies().at(bodyId).shape.volume() == Approx(9000));
    run(s, "edit.undo"); // remove extrude
    CHECK(countKind(s, model::PartKind::Body) == 0);
    CHECK(s.document().part(ex.at("bodies")[0].at("partId").get<std::uint64_t>()) == nullptr);
    run(s, "edit.redo");
    CHECK(countKind(s, model::PartKind::Body) == 1);
    CHECK(s.document().part(ex.at("bodies")[0].at("partId").get<std::uint64_t>()) != nullptr);

    const Json tl = run(s, "timeline.list");
    CHECK(tl.at("features").size() == 2);
    CHECK(tl.at("parameters")[0].at("value").get<double>() == Approx(30));
}

TEST_CASE("Design commands: errors are reported and nothing is recorded", "[commands][cad]")
{
    commands::Session s;
    const auto before = s.undoStack().size();
    auto r = s.execute("feature.extrude", {{"sketch", 42}, {"distance", 5}});
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("No sketch") != std::string::npos);
    r = s.execute("sketch.create", {{"curves", Json::array({{{"type", "hexagon"}}})}});
    CHECK_FALSE(r.ok); // rejected by the schema
    r = s.execute("sketch.create", {{"curves", Json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", "r"}}})}});
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("unknown parameter 'r'") != std::string::npos);
    CHECK(s.undoStack().size() == before);
    CHECK(s.design().features().empty());
}

TEST_CASE("Design commands: body parts cannot be scaled or removed directly", "[commands][cad]")
{
    commands::Session s;
    run(s, "feature.box", {{"size", {10, 10, 10}}});
    const auto partId = s.document().parts().at(0).id;
    CHECK_FALSE(s.execute("part.remove", {{"id", partId}}).ok);
    CHECK_FALSE(s.execute("part.scale", {{"id", partId}, {"factor", 2}}).ok);
    CHECK(run(s, "part.rename", {{"id", partId}, {"name", "Block"}}).at("name") == "Block");
    const Json parts = run(s, "document.parts");
    CHECK(parts.at("parts")[0].at("kind") == "body");
}

TEST_CASE("Design commands: timeline marker, suppress, delete, edit", "[commands][cad]")
{
    commands::Session s;
    const auto box = run(s, "feature.box", {{"size", {10, 10, 10}}}).at("id").get<std::uint64_t>();
    const auto hole =
        run(s, "feature.cylinder", {{"center", {5, 5, -1}}, {"radius", 2}, {"height", 12}, {"operation", "cut"}})
            .at("id")
            .get<std::uint64_t>();
    const double pi = std::numbers::pi;
    CHECK(s.design().bodies().at(box).shape.volume() == Approx(1000 - pi * 40).epsilon(1e-6));

    run(s, "feature.edit", {{"id", hole}, {"params", {{"radius", 3}}}});
    CHECK(s.design().bodies().at(box).shape.volume() == Approx(1000 - pi * 90).epsilon(1e-6));
    CHECK_FALSE(s.execute("feature.edit", {{"id", hole}, {"params", {{"radius", -1}}}}).ok);

    run(s, "feature.suppress", {{"id", hole}, {"suppressed", true}});
    CHECK(s.design().bodies().at(box).shape.volume() == Approx(1000));
    run(s, "feature.suppress", {{"id", hole}, {"suppressed", false}});

    const Json tl = run(s, "timeline.set_marker", {{"position", 1}});
    CHECK(tl.at("features")[1].at("status") == "rolled_back");
    run(s, "timeline.set_marker");
    run(s, "feature.delete", {{"id", hole}});
    CHECK(s.design().bodies().at(box).shape.volume() == Approx(1000));
    run(s, "edit.undo");
    CHECK(s.design().features().size() == 2);

    const Json list = run(s, "commands.list");
    bool hasExtrude = false;
    for (const auto& c : list.at("commands"))
        hasExtrude = hasExtrude || c.at("name") == "feature.extrude";
    CHECK(hasExtrude);
}

TEST_CASE("Design commands: imported mesh becomes a body that can be cut", "[commands][cad]")
{
    commands::Session s;
    const std::string path = std::string(AEROCAD_TEST_DATA_DIR) + "/tetrahedron.stl";
    const Json imported = run(s, "document.import", {{"path", path}});
    const auto partId = imported.at("id").get<std::uint64_t>();
    const Json body = run(s, "feature.base_body", {{"part", partId}});
    const auto bodyId = body.at("id").get<std::uint64_t>();
    REQUIRE(s.design().bodies().contains(bodyId));
    const double tetra = s.design().bodies().at(bodyId).shape.volume();
    CHECK(tetra > 0);
    CHECK_FALSE(s.document().part(partId)->visible);

    run(s, "feature.box", {{"corner", {-1, -1, -1}}, {"size", {3, 3, 3}}, {"operation", "cut"}});
    CHECK(s.design().bodies().at(bodyId).shape.volume() < tetra);

    run(s, "edit.undo");
    run(s, "edit.undo");
    CHECK(s.design().bodies().empty());
    CHECK(s.document().part(partId)->visible);
}
