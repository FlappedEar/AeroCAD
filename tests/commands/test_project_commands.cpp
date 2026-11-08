// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Inspect.hpp"
#include "aerocad/commands/Session.hpp"
#include "aerocad/core/Path.hpp"
#include "aerocad/geometry/Primitives.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>

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
        m_path = fs::temp_directory_path() / ("aerocad_projcmd_" + std::to_string(rd()));
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

const model::Part* bodyPart(const commands::Session& s, std::uint64_t featureId)
{
    for (const auto& p : s.document().parts())
        if (p.kind == model::PartKind::Body && p.featureId == featureId)
            return &p;
    return nullptr;
}

/// Width parameter, a sketch and an extrude: a 2w x w x 10 block.
std::uint64_t buildBlock(commands::Session& s)
{
    run(s, "parameter.set", {{"name", "w"}, {"expression", "20 mm"}});
    const Json sk =
        run(s, "sketch.create",
            {{"plane", "XY"},
             {"curves",
              Json::array({{{"type", "center_rectangle"}, {"center", {0, 0}}, {"width", "2 * w"}, {"height", "w"}}})}});
    const Json ex = run(s, "feature.extrude", {{"sketch", sk.at("id")}, {"distance", 10}});
    return ex.at("id").get<std::uint64_t>();
}

} // namespace

TEST_CASE("Project save and open restore parts, timeline, parameters and frames", "[commands][project]")
{
    TempDir dir;
    commands::Session s;
    CHECK_FALSE(run(s, "project.info").at("dirty").get<bool>());

    const std::string stl = std::string(AEROCAD_TEST_DATA_DIR) + "/tetrahedron.stl";
    const auto meshId = run(s, "document.import", {{"path", stl}, {"name", "Scan"}}).at("id").get<std::uint64_t>();
    run(s, "part.scale", {{"id", meshId}, {"factor", 2}});
    const auto bodyFeature = buildBlock(s);
    const auto bodyPartId = bodyPart(s, bodyFeature)->id;
    run(s, "part.rename", {{"id", bodyPartId}, {"name", "Wing"}});
    run(s, "frame.set", {{"name", "Datum"}, {"origin", {0, 0, 10}}});
    run(s, "sketch.create",
        {{"plane", "Datum"}, {"curves", Json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", 3}}})}});
    CHECK(run(s, "project.info").at("dirty").get<bool>());

    // No path yet.
    CHECK_FALSE(s.execute("project.save").ok);

    const std::string file = dir.file("wing"); // extension is appended
    const Json saved = run(s, "project.save", {{"path", file}, {"view", {{"pitch", -12.5}}}});
    CHECK(saved.at("savedTo").get<std::string>().ends_with("wing.aerocad"));
    CHECK_FALSE(saved.at("dirty").get<bool>());
    CHECK(fs::exists(core::pathFromUtf8(file + ".aerocad")));

    const auto meshBefore = s.document().part(meshId)->mesh->vertices();
    const double volumeBefore = s.design().bodies().at(bodyFeature).shape.volume();

    commands::Session t;
    const Json opened = run(t, "project.open", {{"path", file + ".aerocad"}});
    CHECK_FALSE(opened.at("dirty").get<bool>());
    CHECK(opened.at("view").at("pitch").get<double>() == Approx(-12.5));
    CHECK(opened.at("featureErrors").empty());
    REQUIRE(t.document().part(meshId) != nullptr);
    CHECK(t.document().part(meshId)->name == "Scan");
    CHECK(t.document().part(meshId)->mesh->vertices() == meshBefore);
    REQUIRE(bodyPart(t, bodyFeature) != nullptr);
    CHECK(bodyPart(t, bodyFeature)->id == bodyPartId);
    CHECK(bodyPart(t, bodyFeature)->name == "Wing");
    CHECK(t.design().bodies().at(bodyFeature).shape.volume() == Approx(volumeBefore));
    CHECK(t.design().frame("Datum") != nullptr);
    CHECK_FALSE(t.undoStack().canUndo());

    // Reopened projects stay editable: change the parameter, the body follows.
    run(t, "parameter.set", {{"name", "w"}, {"expression", "10 mm"}});
    CHECK(t.design().bodies().at(bodyFeature).shape.volume() == Approx(2000));
    CHECK(run(t, "project.info").at("dirty").get<bool>());
    run(t, "project.save");
    CHECK_FALSE(run(t, "project.info").at("dirty").get<bool>());

    commands::Session u;
    run(u, "project.open", {{"path", file + ".aerocad"}});
    CHECK(u.design().bodies().at(bodyFeature).shape.volume() == Approx(2000));

    // A failing open leaves the current project untouched.
    CHECK_FALSE(u.execute("project.open", {{"path", dir.file("missing.aerocad")}}).ok);
    CHECK(u.design().bodies().at(bodyFeature).shape.volume() == Approx(2000));

    run(u, "project.new");
    CHECK(u.document().partCount() == 0);
    CHECK(u.design().features().empty());
    CHECK(run(u, "project.info").at("path").get<std::string>().empty());
}

TEST_CASE("Imported B-rep parts keep their exact geometry", "[commands][project]")
{
    TempDir dir;
    const std::string brep = dir.file("box.brep");
    {
        std::ofstream out(core::pathFromUtf8(brep), std::ios::binary);
        out << geometry::toBrep(geometry::makeBox(10, 10, 10));
    }
    commands::Session s;
    const auto id = run(s, "document.import", {{"path", brep}}).at("id").get<std::uint64_t>();
    run(s, "project.save", {{"path", dir.file("b.aerocad")}});
    commands::Session t;
    run(t, "project.open", {{"path", dir.file("b.aerocad")}});
    REQUIRE(t.document().part(id)->shape.has_value());
    CHECK(t.document().part(id)->shape->volume() == Approx(1000));
}

TEST_CASE("Autosave copies keep the path; recovery reopens as unsaved", "[commands][project]")
{
    TempDir dir;
    commands::Session s;
    buildBlock(s);
    run(s, "project.save", {{"path", dir.file("main.aerocad")}});
    run(s, "parameter.set", {{"name", "w"}, {"expression", "30"}});
    const Json copy = run(s, "project.save", {{"path", dir.file("autosave.aerocad")}, {"copy", true}});
    CHECK(copy.at("copy").get<bool>());
    CHECK(copy.at("dirty").get<bool>());
    CHECK(copy.at("path").get<std::string>().ends_with("main.aerocad"));

    commands::Session t;
    const Json recovered = run(t, "project.open", {{"path", dir.file("autosave.aerocad")}, {"recover", true}});
    CHECK(recovered.at("dirty").get<bool>());
    CHECK(recovered.at("recovered").get<bool>());
    CHECK(recovered.at("path").get<std::string>().ends_with("main.aerocad"));
    CHECK(t.design().parameterValue("w").value() == Approx(30));
    run(t, "project.save"); // writes the original file
    CHECK_FALSE(run(t, "project.info").at("dirty").get<bool>());
    commands::Session u;
    run(u, "project.open", {{"path", dir.file("main.aerocad")}});
    CHECK(u.design().parameterValue("w").value() == Approx(30));
}

TEST_CASE("Variants: duplicate as B, change a parameter, toggle A/B, undo", "[commands][project][variants]")
{
    commands::Session s;
    const auto bodyFeature = buildBlock(s);
    const auto partId = bodyPart(s, bodyFeature)->id;
    CHECK(s.design().bodies().at(bodyFeature).shape.volume() == Approx(8000));

    const Json created = run(s, "variant.create");
    CHECK(created.at("variants").size() == 2);
    CHECK(created.at("variants")[1].at("name") == "B");
    CHECK(created.at("active") == created.at("created"));
    run(s, "parameter.set", {{"name", "w"}, {"expression", "10 mm"}});
    CHECK(s.design().bodies().at(bodyFeature).shape.volume() == Approx(2000));

    const Json diff = run(s, "variant.compare");
    REQUIRE(diff.at("parameters").size() == 1);
    CHECK(diff.at("parameters")[0].at("a") == "20 mm");
    CHECK(diff.at("parameters")[0].at("b") == "10 mm");
    CHECK(diff.at("features").empty());

    run(s, "variant.toggle"); // back to A
    CHECK(s.design().bodies().at(bodyFeature).shape.volume() == Approx(8000));
    CHECK(bodyPart(s, bodyFeature)->id == partId); // same part, updated in place
    run(s, "variant.toggle");                      // B again
    CHECK(s.design().bodies().at(bodyFeature).shape.volume() == Approx(2000));
    run(s, "variant.activate", {{"variant", "A"}});
    CHECK(s.design().bodies().at(bodyFeature).shape.volume() == Approx(8000));

    // Undo walks back through the switches and the edit in B.
    run(s, "edit.undo"); // activate A
    CHECK(s.design().bodies().at(bodyFeature).shape.volume() == Approx(2000));
    run(s, "edit.undo"); // toggle to B
    run(s, "edit.undo"); // toggle to A
    CHECK(s.design().bodies().at(bodyFeature).shape.volume() == Approx(2000));
    run(s, "edit.undo"); // parameter in B
    CHECK(s.design().bodies().at(bodyFeature).shape.volume() == Approx(8000));
    run(s, "edit.undo"); // create B
    CHECK(run(s, "variant.list").at("variants").size() == 1);
    run(s, "edit.redo");
    CHECK(run(s, "variant.list").at("variants").size() == 2);

    CHECK_FALSE(s.execute("variant.delete", {{"variant", "B"}}).ok); // active
    run(s, "variant.rename", {{"variant", "B"}, {"name", "Thin"}});
    run(s, "variant.activate", {{"variant", 1}});
    run(s, "variant.delete", {{"variant", "Thin"}});
    CHECK(run(s, "variant.list").at("variants").size() == 1);
    CHECK_FALSE(s.execute("variant.toggle").ok);
}

TEST_CASE("Features created independently in two variants get distinct ids", "[commands][project][variants]")
{
    commands::Session s;
    run(s, "variant.create");
    const auto inB = run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {1, 1, 1}}}).at("id").get<std::uint64_t>();
    run(s, "variant.toggle");
    const auto inA = run(s, "feature.box", {{"corner", {5, 0, 0}}, {"size", {1, 1, 1}}}).at("id").get<std::uint64_t>();
    CHECK(inA != inB);
    const Json diff = run(s, "variant.compare");
    CHECK(diff.at("features").size() == 2); // one only in A, one only in B
}

TEST_CASE("Variants are saved with the project", "[commands][project][variants]")
{
    TempDir dir;
    commands::Session s;
    const auto bodyFeature = buildBlock(s);
    run(s, "variant.create", {{"name", "Long"}});
    run(s, "parameter.set", {{"name", "w"}, {"expression", "30 mm"}});
    run(s, "variant.toggle");
    run(s, "project.save", {{"path", dir.file("v.aerocad")}});

    commands::Session t;
    const Json opened = run(t, "project.open", {{"path", dir.file("v.aerocad")}});
    CHECK(opened.at("variants") == 2);
    CHECK(opened.at("activeVariant").at("name") == "A");
    CHECK(t.design().bodies().at(bodyFeature).shape.volume() == Approx(8000));
    run(t, "variant.toggle");
    CHECK(t.design().bodies().at(bodyFeature).shape.volume() == Approx(18000));
}

TEST_CASE("Names of suppressed bodies survive save and open", "[commands][project]")
{
    TempDir dir;
    commands::Session s;
    const auto bodyFeature = buildBlock(s);
    const auto partId = bodyPart(s, bodyFeature)->id;
    run(s, "part.rename", {{"id", partId}, {"name", "Spar"}});
    run(s, "feature.suppress", {{"id", bodyFeature}, {"suppressed", true}});
    REQUIRE(bodyPart(s, bodyFeature) == nullptr);
    run(s, "project.save", {{"path", dir.file("s.aerocad")}});

    commands::Session t;
    run(t, "project.open", {{"path", dir.file("s.aerocad")}});
    run(t, "feature.suppress", {{"id", bodyFeature}, {"suppressed", false}});
    REQUIRE(bodyPart(t, bodyFeature) != nullptr);
    CHECK(bodyPart(t, bodyFeature)->name == "Spar");
    CHECK(bodyPart(t, bodyFeature)->id == partId);

    // A body created while the other one is suppressed must not take its retained id.
    commands::Session u;
    run(u, "project.open", {{"path", dir.file("s.aerocad")}});
    const auto box = run(u, "feature.box", {{"corner", {50, 0, 0}}, {"size", {1, 1, 1}}}).at("id").get<std::uint64_t>();
    REQUIRE(bodyPart(u, box) != nullptr);
    CHECK(bodyPart(u, box)->id != partId);
}

TEST_CASE("Region selections on bodies survive save and open", "[commands][project]")
{
    TempDir dir;
    commands::Session s;
    const auto bodyFeature = buildBlock(s);
    const auto partId = bodyPart(s, bodyFeature)->id;
    run(s, "selection.set", {{"id", partId}, {"triangles", {0, 1}}});
    run(s, "project.save", {{"path", dir.file("sel.aerocad")}});

    commands::Session t;
    run(t, "project.open", {{"path", dir.file("sel.aerocad")}});
    REQUIRE(bodyPart(t, bodyFeature)->selection);
    CHECK(*bodyPart(t, bodyFeature)->selection == core::TriangleSet{0, 1});
}

TEST_CASE("Imported parts can be duplicated", "[commands][project]")
{
    commands::Session s;
    const std::string stl = std::string(AEROCAD_TEST_DATA_DIR) + "/tetrahedron.stl";
    const auto id = run(s, "document.import", {{"path", stl}}).at("id").get<std::uint64_t>();
    const Json copy = run(s, "part.duplicate", {{"id", id}, {"name", "B"}});
    CHECK(copy.at("name") == "B");
    CHECK(s.document().partCount() == 2);
    run(s, "edit.undo");
    CHECK(s.document().partCount() == 1);
}

TEST_CASE("A body is duplicated as a fixed solid that survives timeline edits and saving",
          "[commands][project]")
{
    TempDir dir;
    commands::Session s;
    run(s, "feature.box", {{"size", {40, 30, 6}}});
    model::PartId body = 0;
    for (const auto& p : s.document().parts())
        if (p.kind == model::PartKind::Body)
            body = p.id;
    REQUIRE(body != 0);
    const Json copy = run(s, "part.duplicate", {{"id", body}});
    const auto copyId = copy.at("id").get<model::PartId>();
    const model::Part* c = s.document().part(copyId);
    REQUIRE(c);
    CHECK(c->kind == model::PartKind::Imported);
    CHECK(c->featureId == 0);
    CHECK(c->shape.has_value());
    CHECK(c->mesh->bounds().size().z == Approx(6));

    // Undo / redo of the box feature rebuilds the timeline; the copy stays as it is.
    run(s, "edit.undo"); // the duplicate
    CHECK(s.document().part(copyId) == nullptr);
    run(s, "edit.redo");
    REQUIRE(s.document().part(copyId));
    run(s, "project.save", {{"path", dir.file("copy.aerocad")}});
    commands::Session t;
    run(t, "project.open", {{"path", dir.file("copy.aerocad")}});
    int imported = 0;
    for (const auto& p : t.document().parts())
        if (p.kind == model::PartKind::Imported && p.shape)
            ++imported;
    CHECK(imported == 1);

    // Sketches cannot be copied.
    run(s, "sketch.create", {{"plane", "XY"}});
    model::PartId sketchPart = 0;
    for (const auto& p : s.document().parts())
        if (p.kind == model::PartKind::Sketch)
            sketchPart = p.id;
    if (sketchPart != 0)
        CHECK_FALSE(s.execute("part.duplicate", {{"id", sketchPart}}).ok);
}

TEST_CASE("A renamed timeline body keeps its name through edits, undo and saving", "[commands][project]")
{
    TempDir dir;
    commands::Session s;
    run(s, "feature.box", {{"size", {40, 30, 6}}});
    const model::Part* body = nullptr;
    for (const auto& p : s.document().parts())
        if (p.kind == model::PartKind::Body)
            body = &p;
    REQUIRE(body);
    const model::PartId id = body->id;
    const auto featureId = body->featureId;
    run(s, "part.rename", {{"id", id}, {"name", "Fin left"}});
    run(s, "feature.edit", {{"id", featureId}, {"params", {{"size", {50, 30, 6}}}}});
    REQUIRE(s.document().part(id));
    CHECK(s.document().part(id)->name == "Fin left");
    CHECK(s.document().part(id)->mesh->bounds().size().x == Approx(50));
    run(s, "edit.undo"); // the edit: the name stays
    CHECK(s.document().part(id)->name == "Fin left");
    run(s, "project.save", {{"path", dir.file("renamed.aerocad")}});
    commands::Session t;
    run(t, "project.open", {{"path", dir.file("renamed.aerocad")}});
    int named = 0;
    for (const auto& p : t.document().parts())
        if (p.kind == model::PartKind::Body && p.name == "Fin left")
            ++named;
    CHECK(named == 1);
    run(t, "feature.edit", {{"id", featureId}, {"params", {{"size", {60, 30, 6}}}}});
    named = 0;
    for (const auto& p : t.document().parts())
        if (p.kind == model::PartKind::Body && p.name == "Fin left")
            ++named;
    CHECK(named == 1);
}

TEST_CASE("Appearance colours are undoable, kept through timeline edits and saved", "[commands][project]")
{
    TempDir dir;
    commands::Session s;
    run(s, "feature.box", {{"size", {40, 30, 6}}});
    const model::Part* body = nullptr;
    for (const auto& p : s.document().parts())
        if (p.kind == model::PartKind::Body)
            body = &p;
    REQUIRE(body);
    const model::PartId id = body->id;
    const auto featureId = body->featureId;
    const std::string stl = std::string(AEROCAD_TEST_DATA_DIR) + "/tetrahedron.stl";
    const auto scan = run(s, "document.import", {{"path", stl}}).at("id").get<model::PartId>();

    CHECK(s.document().part(id)->color.empty());
    CHECK(run(s, "part.set_color", {{"id", id}, {"color", "#3F7FD9"}}).at("color") == "#3f7fd9");
    run(s, "part.set_color", {{"id", scan}, {"color", "#c94f45"}});
    CHECK_FALSE(s.execute("part.set_color", {{"id", id}, {"color", "blue"}}).ok);
    CHECK_FALSE(s.execute("part.set_color", {{"id", id}, {"color", "#12345"}}).ok);
    run(s, "edit.undo"); // the scan's colour
    CHECK(s.document().part(scan)->color.empty());
    run(s, "edit.redo");
    CHECK(s.document().part(scan)->color == "#c94f45");

    // A timeline edit regenerates the body: the colour stays, also through suppress.
    run(s, "feature.edit", {{"id", featureId}, {"params", {{"size", {50, 30, 6}}}}});
    CHECK(s.document().part(id)->color == "#3f7fd9");
    run(s, "feature.suppress", {{"id", featureId}, {"suppressed", true}});
    run(s, "feature.suppress", {{"id", featureId}, {"suppressed", false}});
    REQUIRE(s.document().part(id));
    CHECK(s.document().part(id)->color == "#3f7fd9");

    run(s, "project.save", {{"path", dir.file("colours.aerocad")}});
    commands::Session t;
    run(t, "project.open", {{"path", dir.file("colours.aerocad")}});
    REQUIRE(t.document().part(id));
    REQUIRE(t.document().part(scan));
    CHECK(t.document().part(id)->color == "#3f7fd9");
    CHECK(t.document().part(scan)->color == "#c94f45");
    // "" goes back to the default colour.
    run(t, "part.set_color", {{"id", id}, {"color", ""}});
    CHECK(t.document().part(id)->color.empty());
}

TEST_CASE("Undoing a delete puts the part back where it was in the Browser", "[commands][project]")
{
    commands::Session s;
    run(s, "feature.box", {{"size", {40, 30, 6}}});
    run(s, "feature.plane", {{"offset", 10}});
    run(s, "feature.cylinder", {{"radius", 5}, {"height", 10}, {"center", {100, 0, 0}}});
    const auto order = [&] {
        std::vector<model::PartId> ids;
        for (const auto& p : s.document().parts())
            ids.push_back(p.id);
        return ids;
    };
    const auto before = order();
    REQUIRE(before.size() == 3);
    const auto first = s.document().parts().front();
    // Deleting the first feature, suppressing it or rolling the timeline back and undoing
    // each brings its part back first in the list.
    run(s, "feature.delete", {{"id", first.featureId}});
    run(s, "edit.undo");
    CHECK(order() == before);
    run(s, "feature.suppress", {{"id", first.featureId}, {"suppressed", true}});
    run(s, "edit.undo");
    CHECK(order() == before);
    run(s, "timeline.set_marker", {{"position", 0}});
    run(s, "edit.undo");
    CHECK(order() == before);
}

TEST_CASE("Kept dimensions follow edits and are saved with the project", "[commands][project][inspect]")
{
    TempDir dir;
    commands::Session s;
    const Json box = run(s, "feature.box", {{"size", {40, 30, 6}}});
    model::PartId part = 0;
    for (const auto& p : s.document().parts())
        if (p.kind == model::PartKind::Body)
            part = p.id;
    REQUIRE(part != 0);

    // Thickness under the top face, and the distance between the end faces.
    const Json t = run(s, "inspect.add_dimension", {{"a", {{"part", part}, {"pick", "face"}, {"point", {20, 15, 6}}}}});
    CHECK(t["show"] == "thickness");
    CHECK(t["value"].get<double>() == Approx(6));
    const Json d = run(s, "inspect.add_dimension",
                       {{"a", {{"part", part}, {"pick", "face"}, {"point", {0, 15, 3}}}},
                        {"b", {{"part", part}, {"pick", "face"}, {"point", {40, 15, 3}}}}});
    CHECK(d["value"].get<double>() == Approx(40));
    CHECK(d["unit"] == "mm");
    // What a selection cannot give is refused.
    CHECK_FALSE(s.execute("inspect.add_dimension", {{"a", {{"part", part}, {"pick", "face"}, {"point", {20, 15, 6}}}},
                                                    {"show", "radius"}})
                    .ok);
    CHECK_FALSE(s.execute("inspect.add_dimension", {{"a", {{"point", {0, 0, 0}}}}, {"show", "distance"}}).ok);
    CHECK(run(s, "project.info").at("dirty").get<bool>());

    // A thicker box: the kept thickness follows.
    run(s, "feature.edit", {{"id", box.at("id")}, {"params", {{"size", {40, 30, 8}}}}});
    Json list = run(s, "inspect.dimensions")["dimensions"];
    REQUIRE(list.size() == 2);
    CHECK(list[0]["value"].get<double>() == Approx(8));
    CHECK(list[1]["value"].get<double>() == Approx(40));

    const std::string path = dir.file("dims.aerocad");
    run(s, "project.save", {{"path", path}, {"view", {{"camera", {{"distance", 100}}}}}});
    run(s, "project.new");
    CHECK(run(s, "inspect.dimensions")["dimensions"].empty());
    const Json opened = run(s, "project.open", {{"path", path}});
    CHECK(opened["view"]["camera"]["distance"] == 100);
    list = run(s, "inspect.dimensions")["dimensions"];
    REQUIRE(list.size() == 2);
    CHECK(list[0]["value"].get<double>() == Approx(8));

    // Removing one, then all; a new dimension does not reuse an id.
    run(s, "inspect.remove_dimension", {{"id", list[0]["id"]}});
    CHECK(run(s, "inspect.dimensions")["dimensions"].size() == 1);
    const Json again = run(s, "inspect.add_dimension", {{"a", {{"part", part}, {"pick", "face"}, {"point", {20, 15, 8}}}}});
    CHECK(again["id"].get<std::uint64_t>() > list[1]["id"].get<std::uint64_t>());
    CHECK(run(s, "inspect.remove_dimension", {{"all", true}})["removed"] == 2);
    CHECK_FALSE(s.execute("inspect.remove_dimension", {{"id", 99}}).ok);

    // A gone part shows an error instead of a value.
    run(s, "inspect.add_dimension", {{"a", {{"part", part}, {"pick", "face"}, {"point", {20, 15, 8}}}}});
    run(s, "feature.delete", {{"id", box.at("id")}});
    list = run(s, "inspect.dimensions")["dimensions"];
    REQUIRE(list.size() == 1);
    CHECK(list[0].contains("error"));
}

TEST_CASE("Damaged kept dimensions in a file are skipped", "[commands][project][inspect]")
{
    const auto dims = commands::dimensionsFromJson(Json::parse(
        R"([{"a":{"point":[0,0,0]},"id":"bad"},{"a":{"point":[1,0,0]},"show":3},{"a":5},7,{"a":{"point":[2,0,0]},"id":4,"show":"angle","b":"x"}])"));
    REQUIRE(dims.size() == 3);
    CHECK(dims[0].id == 0);
    CHECK(dims[1].show == "distance");
    CHECK(dims[2].id == 4);
    CHECK(dims[2].show == "angle");
    CHECK(dims[2].b.is_null());
}
