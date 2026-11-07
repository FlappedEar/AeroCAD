// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"
#include "aerocad/core/Path.hpp"
#include "aerocad/geometry/Primitives.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <memory>
#include <random>
#include <string>

namespace fs = std::filesystem;
using namespace aerocad;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using commands::Json;

namespace {

class TempDir {
public:
    TempDir() : m_path(fs::temp_directory_path() / ("aerocad_test_exportcmd_" + std::to_string(std::random_device{}())))
    {
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

model::PartId addMeshPart(commands::Session& s, const std::string& name, bool closed, double dx = 0.0)
{
    std::vector<core::TriangleMesh::Triangle> tris{{0, 2, 1}, {0, 1, 3}, {0, 3, 2}};
    if (closed)
        tris.push_back({1, 2, 3});
    model::Part p;
    p.name = name;
    p.mesh = std::make_shared<const core::TriangleMesh>(
        std::vector<core::Vec3>{{dx, 0, 0}, {dx + 10, 0, 0}, {dx, 10, 0}, {dx, 0, 10}}, tris);
    return s.document().addPart(std::move(p));
}

} // namespace

TEST_CASE("part.check reports mesh validity", "[commands][export]")
{
    commands::Session s;
    const auto good = addMeshPart(s, "good", true);
    const auto open = addMeshPart(s, "open", false);

    auto r = s.execute("part.check", {{"id", good}});
    REQUIRE(r.ok);
    CHECK(r.data.at("watertight") == true);
    CHECK(r.data.at("volume").get<double>() == Approx(1000.0 / 6.0));
    CHECK(r.data.at("problems").empty());

    r = s.execute("part.check", {{"id", open}});
    REQUIRE(r.ok);
    CHECK(r.data.at("watertight") == false);
    CHECK(r.data.at("closed") == false);
    CHECK(r.data.at("boundaryEdges") == 3);
    CHECK(r.data.at("volume").is_null());
    CHECK(r.data.at("problems").size() == 1);

    CHECK_FALSE(s.execute("part.check", {{"id", 99}}).ok);
}

TEST_CASE("document.export writes STL and 3MF with validity warnings", "[commands][export]")
{
    TempDir dir;
    commands::Session s;
    const auto good = addMeshPart(s, "good", true);
    const auto open = addMeshPart(s, "open", false, 50);

    // Default: all visible parts, format from the extension, warnings for the open mesh.
    auto r = s.execute("document.export", {{"path", dir.file("all.stl")}});
    REQUIRE(r.ok);
    CHECK(r.data.at("format") == "STL");
    CHECK(r.data.at("triangles") == 7);
    CHECK(r.data.at("ids") == Json::array({good, open}));
    REQUIRE(r.data.at("warnings").size() == 1);
    CHECK_THAT(r.data.at("warnings")[0].get<std::string>(), ContainsSubstring("open: not closed"));
    CHECK(fs::file_size(core::pathFromUtf8(dir.file("all.stl"))) == 84 + 7 * 50);

    // requireValid aborts without writing.
    r = s.execute("document.export", {{"path", dir.file("strict.3mf")}, {"requireValid", true}});
    CHECK_FALSE(r.ok);
    CHECK_THAT(r.error, ContainsSubstring("not watertight"));
    CHECK_FALSE(fs::exists(core::pathFromUtf8(dir.file("strict.3mf"))));

    // Selected part, explicit format overrides the extension.
    r = s.execute("document.export",
                  {{"path", dir.file("good.bin")}, {"ids", {good}}, {"format", "3mf"}, {"requireValid", true}});
    REQUIRE(r.ok);
    CHECK(r.data.at("format") == "3MF");
    CHECK(r.data.at("warnings").empty());
    CHECK(r.data.at("checks")[0].at("watertight") == true);

    // Hidden parts are skipped by default.
    REQUIRE(s.execute("part.set_visible", {{"id", open}, {"visible", false}}).ok);
    r = s.execute("document.export", {{"path", dir.file("visible.stl")}});
    REQUIRE(r.ok);
    CHECK(r.data.at("ids") == Json::array({good}));

    // Export does not touch the undo history.
    CHECK(s.execute("edit.history").data.at("undoLabel") == "Hide open");
}

TEST_CASE("document.export STEP needs B-rep parts", "[commands][export]")
{
    TempDir dir;
    commands::Session s;
    model::Part box;
    box.name = "box";
    box.shape = geometry::makeBox(10, 20, 30);
    const auto boxId = s.document().addPart(std::move(box));
    const auto meshId = addMeshPart(s, "scan", true);

    auto r = s.execute("document.export", {{"path", dir.file("box.step")}, {"ids", {boxId}}});
    REQUIRE(r.ok);
    CHECK(r.data.at("format") == "STEP");
    CHECK(fs::file_size(core::pathFromUtf8(dir.file("box.step"))) > 0);

    r = s.execute("document.export", {{"path", dir.file("both.step")}, {"ids", {boxId, meshId}}});
    CHECK_FALSE(r.ok);
    CHECK_THAT(r.error, ContainsSubstring("'scan'"));
}

TEST_CASE("document.export parameter errors", "[commands][export]")
{
    TempDir dir;
    commands::Session s;
    CHECK_FALSE(s.execute("document.export", {{"path", dir.file("x.stl")}}).ok); // no parts
    addMeshPart(s, "t", true);
    CHECK_FALSE(s.execute("document.export", {{"path", "relative.stl"}}).ok);
    CHECK_FALSE(s.execute("document.export", {{"path", dir.file("x.obj")}}).ok);
    CHECK_FALSE(s.execute("document.export", {{"path", dir.file("x.stl")}, {"format", "obj"}}).ok);
    CHECK_FALSE(s.execute("document.export", {{"path", dir.file("x.stl")}, {"ids", Json::array()}}).ok);
    CHECK_FALSE(s.execute("document.export", {{"path", dir.file("x.stl")}, {"ids", {42}}}).ok);
}

TEST_CASE("document.export skips sketches and construction planes", "[commands][export]")
{
    TempDir dir;
    commands::Session s;
    REQUIRE(s.execute("feature.box", {{"size", {10, 10, 10}}}).ok);
    REQUIRE(s.execute("feature.plane", {{"base", "XY"}, {"offset", 20}}).ok);
    REQUIRE(s.execute("sketch.create",
                      {{"plane", "XY"},
                       {"curves", Json::array({{{"type", "circle"}, {"center", {30, 0}}, {"radius", 3}}})}})
                .ok);
    const auto r = s.execute("document.export", {{"path", dir.file("bodies.stl")}});
    REQUIRE(r.ok);
    CHECK(r.data.at("ids").size() == 1);
    CHECK(r.data.at("triangles") == 12);
}

TEST_CASE("document.export_check explains what blocks or weakens an export", "[commands][export]")
{
    commands::Session s;
    REQUIRE(s.execute("feature.box", {{"size", {20, 10, 4}}}).ok);
    const auto box = s.document().parts().back().id;

    SECTION("a solid box is ready for every format")
    {
        for (const char* format : {"stl", "3mf", "step"}) {
            const auto r = s.execute("document.export_check", {{"ids", {box}}, {"format", format}});
            REQUIRE(r.ok);
            CHECK(r.data.at("passed") == true);
            CHECK(r.data.at("canExport") == true);
            const Json& part = r.data.at("parts").at(0);
            CHECK(part.at("status") == "ok");
            CHECK(part.at("watertight") == true);
            CHECK(part.at("validBrep") == true);
            CHECK(part.at("minWall").at("thinnest").get<double>() == Approx(4.0).margin(0.01));
            CHECK(part.at("minWall").at("thinAreaPercent").get<double>() == Approx(0.0));
        }
    }

    SECTION("walls thinner than the limit are a warning with the place and share")
    {
        const auto r = s.execute("document.export_check", {{"ids", {box}}, {"minWall", 5.0}});
        REQUIRE(r.ok);
        CHECK(r.data.at("passed") == false);
        CHECK(r.data.at("canExport") == true);
        const Json& part = r.data.at("parts").at(0);
        CHECK(part.at("status") == "warning");
        REQUIRE(part.at("warnings").size() == 1);
        CHECK_THAT(part.at("warnings").at(0).get<std::string>(), ContainsSubstring("thinner than 5.00 mm"));
        // Every face but the two 20 x 10 ones is less than 5 mm through.
        CHECK(part.at("minWall").at("thinAreaPercent").get<double>() > 50.0);

        const auto off = s.execute("document.export_check", {{"ids", {box}}, {"minWall", 0}});
        REQUIRE(off.ok);
        CHECK(off.data.at("passed") == true);
        CHECK_FALSE(off.data.at("parts").at(0).contains("minWall"));
    }

    SECTION("an open mesh cannot be printed; a mesh cannot be STEP")
    {
        const auto open = addMeshPart(s, "open", false, 50.0);
        auto r = s.execute("document.export_check", {{"ids", {open}}, {"format", "stl"}});
        REQUIRE(r.ok);
        CHECK(r.data.at("canExport") == false);
        CHECK(r.data.at("errors") == 1);
        const Json& part = r.data.at("parts").at(0);
        CHECK(part.at("status") == "error");
        CHECK_THAT(part.at("errors").at(0).get<std::string>(), ContainsSubstring("3 open edges"));
        CHECK_FALSE(part.contains("minWall"));

        const auto closed = addMeshPart(s, "closed", true, 80.0);
        r = s.execute("document.export_check", {{"ids", {closed}}, {"format", "step"}});
        REQUIRE(r.ok);
        CHECK(r.data.at("canExport") == false);
        CHECK_THAT(r.data.at("parts").at(0).at("errors").at(0).get<std::string>(),
                   ContainsSubstring("STEP needs exact geometry"));
    }
}
