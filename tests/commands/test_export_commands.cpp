// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"
#include "aerocad/core/Path.hpp"
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <variant>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#endif

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

std::string readFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeFile(const fs::path& path, const std::string& bytes)
{
    std::ofstream output(path, std::ios::binary);
    output << bytes;
    REQUIRE(output.good());
}

model::PartId addPrintBox(commands::Session& session, const std::string& name = "test")
{
    REQUIRE(session.execute("feature.box", {{"size", {20, 10, 4}}}).ok);
    const auto id = session.document().parts().back().id;
    REQUIRE(session.execute("part.rename", {{"id", id}, {"name", name}}).ok);
    return id;
}

} // namespace

TEST_CASE("Export approval is rejected after its document or request changes", "[commands][export][preflight]")
{
    TempDir dir;
    commands::Session session;
    const auto first = addPrintBox(session, "first");
    const auto second = addPrintBox(session, "second");
    const bool print = GENERATE(false, true);
    const auto check = session.execute("document.export_check", {{"ids", {first}}, {"print", print}, {"minWall", 0}});
    REQUIRE(check.ok);
    REQUIRE(check.data.at("passed") == true);
    const std::string path = dir.file("first.stl");
    writeFile(path, "previous STL");
    Json params{{"ids", {first}}, {"expectedState", check.data.at("state")}};
    if (print) {
        params["folder"] = dir.file("");
    } else {
        params["path"] = path;
        params["requireValid"] = true;
    }

    SECTION("different selected part without a document change") { params["ids"] = {second}; }
    SECTION("different visibility") {
        REQUIRE(session.execute("part.set_visible", {{"id", second}, {"visible", false}}).ok);
    }
    SECTION("deleted selected part") {
        REQUIRE(session.execute("feature.delete", {{"id", session.document().part(first)->featureId}}).ok);
    }
    SECTION("new project reusing part ids") {
        REQUIRE(session.execute("project.new").ok);
        addPrintBox(session, "first");
    }
    SECTION("different active variant") { REQUIRE(session.execute("variant.create").ok); }
    SECTION("new geometry") { addMeshPart(session, "open surface", false); }

    const auto result = session.execute(print ? "document.export_for_print" : "document.export", params);
    CHECK_FALSE(result.ok);
    CHECK(readFile(path) == "previous STL");
    CHECK(std::distance(fs::directory_iterator(core::pathFromUtf8(dir.file(""))), fs::directory_iterator{}) == 1);
}

TEST_CASE("Export All approval follows its current visible part set", "[commands][export][preflight]")
{
    TempDir dir;
    commands::Session session;
    addPrintBox(session);
    const bool print = GENERATE(false, true);
    const auto check = session.execute("document.export_check", {{"print", print}, {"minWall", 0}});
    REQUIRE(check.ok);
    addPrintBox(session, "added after check");
    const Json params = print ? Json{{"folder", dir.file("new")}, {"expectedState", check.data.at("state")}}
                              : Json{{"path", dir.file("new.stl")}, {"expectedState", check.data.at("state")}};
    const auto result = session.execute(print ? "document.export_for_print" : "document.export", params);
    CHECK_FALSE(result.ok);
    CHECK_THAT(result.error, ContainsSubstring("changed after the check"));
    CHECK(fs::is_empty(core::pathFromUtf8(dir.file(""))));
}

TEST_CASE("Current export approvals work and cannot be reused for another format", "[commands][export][preflight]")
{
    TempDir dir;
    commands::Session session;
    addPrintBox(session);
    const std::string format = GENERATE("stl", "3mf", "step");
    const auto check = session.execute("document.export_check", {{"format", format}, {"minWall", 0}});
    REQUIRE(check.ok);
    REQUIRE(session.execute("document.parts").ok); // a read does not invalidate approval
    const std::string path = dir.file("part." + format);
    REQUIRE(session.execute("document.export",
                            {{"path", path}, {"requireValid", true}, {"expectedState", check.data.at("state")}}).ok);
    CHECK(fs::file_size(core::pathFromUtf8(path)) > 0);
    const auto changed = session.execute("document.export", {{"path", dir.file("different.stl")},
                                                              {"format", format == "stl" ? "3mf" : "stl"},
                                                              {"expectedState", check.data.at("state")}});
    CHECK_FALSE(changed.ok);
    CHECK_FALSE(fs::exists(core::pathFromUtf8(dir.file("different.stl"))));

    const auto printCheck = session.execute("document.export_check", {{"print", true}, {"minWall", 0}});
    REQUIRE(printCheck.ok);
    CHECK(session.execute("document.export_for_print",
                          {{"folder", dir.file("print")}, {"expectedState", printCheck.data.at("state")}}).ok);
}

TEST_CASE("Print export preserves old files when a later destination is invalid", "[commands][export][transaction]")
{
    TempDir dir;
    commands::Session session;
    const auto id = addPrintBox(session);
    const fs::path folder = core::pathFromUtf8(dir.file(""));
    writeFile(folder / "test.stl", "previous STL");
    fs::create_directory(folder / "test.step");
    writeFile(folder / "test.step" / "keep.txt", "do not remove");

    const auto result = session.execute("document.export_for_print", {{"folder", dir.file("")}, {"ids", {id}}});
    CHECK_FALSE(result.ok);
    CHECK(readFile(folder / "test.stl") == "previous STL");
    CHECK(readFile(folder / "test.step" / "keep.txt") == "do not remove");
    CHECK(std::distance(fs::directory_iterator(folder), fs::directory_iterator{}) == 2);
}

#ifndef _WIN32
TEST_CASE("Print export preserves the complete old set when a later writer fails", "[commands][export][transaction]")
{
    TempDir dir;
    commands::Session session;
    const auto id = addPrintBox(session);
    const fs::path folder = core::pathFromUtf8(dir.file(""));
    writeFile(folder / "test.stl", "previous STL");
    writeFile(folder / "test.step", "previous STEP");

    // Measure this kernel's output first, then allow the STL to finish and fail
    // the larger STEP at the OS boundary, without a production fault-injection hook.
    TempDir probe;
    REQUIRE(session.execute("document.export_for_print", {{"folder", probe.file("")}, {"ids", {id}}}).ok);
    const auto stlBytes = fs::file_size(core::pathFromUtf8(probe.file("test.stl")));
    REQUIRE(fs::file_size(core::pathFromUtf8(probe.file("test.step"))) > stlBytes);
    struct FileSizeLimit {
        rlimit previous{};
        void (*previousSignal)(int) = SIG_DFL;
        bool active = false;
        explicit FileSizeLimit(rlim_t maxBytes)
        {
            if (getrlimit(RLIMIT_FSIZE, &previous) != 0)
                return;
            rlimit limited = previous;
            limited.rlim_cur = maxBytes;
            previousSignal = std::signal(SIGXFSZ, SIG_IGN);
            if (previousSignal == SIG_ERR)
                return;
            active = setrlimit(RLIMIT_FSIZE, &limited) == 0;
            if (!active)
                std::signal(SIGXFSZ, previousSignal);
        }
        ~FileSizeLimit()
        {
            if (active) {
                setrlimit(RLIMIT_FSIZE, &previous);
                std::signal(SIGXFSZ, previousSignal);
            }
        }
    };
    const auto result = [&] {
        FileSizeLimit limit(static_cast<rlim_t>(stlBytes));
        REQUIRE(limit.active);
        return session.execute("document.export_for_print", {{"folder", dir.file("")}, {"ids", {id}}});
    }();
    CHECK_FALSE(result.ok);
    CHECK(readFile(folder / "test.stl") == "previous STL");
    CHECK(readFile(folder / "test.step") == "previous STEP");
    CHECK(std::distance(fs::directory_iterator(folder), fs::directory_iterator{}) == 2);
}
#endif

TEST_CASE("Print export refuses symbolic-link destinations without replacing them", "[commands][export][transaction]")
{
    TempDir dir;
    commands::Session session;
    const auto id = addPrintBox(session);
    const fs::path folder = core::pathFromUtf8(dir.file(""));
    writeFile(folder / "test.stl", "previous STL");
    const fs::path linked = folder / "external.step";
    SECTION("existing target") { writeFile(linked, "external STEP"); }
    SECTION("dangling target") {}
    std::error_code ec;
    fs::create_symlink(linked, folder / "test.step", ec);
    if (ec)
        SKIP("Symbolic links unavailable on this filesystem: " << ec.message());
    const bool existed = fs::exists(linked);

    const auto result = session.execute("document.export_for_print", {{"folder", dir.file("")}, {"ids", {id}}});
    CHECK_FALSE(result.ok);
    CHECK(readFile(folder / "test.stl") == "previous STL");
    REQUIRE(fs::is_symlink(fs::symlink_status(folder / "test.step")));
    CHECK(fs::read_symlink(folder / "test.step") == linked);
    CHECK(fs::exists(linked) == existed);
    if (existed)
        CHECK(readFile(linked) == "external STEP");
}

TEST_CASE("Print export replaces complete pairs and disambiguates sanitized names", "[commands][export][transaction]")
{
    TempDir dir;
    commands::Session session;
    const auto a = addPrintBox(session, "Fin:1");
    const auto b = addPrintBox(session, "fin?1");
    const fs::path folder = core::pathFromUtf8(dir.file(""));
    const std::set<std::string> expected{"Fin-1.stl", "Fin-1.step", "fin-1 2.stl", "fin-1 2.step", "unrelated.txt"};
    for (const auto& name : expected)
        writeFile(folder / name, "previous " + name);

    const auto result = session.execute("document.export_for_print", {{"folder", dir.file("")}, {"ids", {a, b}}});
    INFO(result.toJson().dump());
    REQUIRE(result.ok);
    REQUIRE(result.data.at("files").size() == 4);
    CHECK(result.data.at("warnings").empty());
    std::set<std::string> actual;
    for (const auto& entry : fs::directory_iterator(folder))
        actual.insert(core::pathToUtf8(entry.path().filename()));
    CHECK(actual == expected);
    CHECK(readFile(folder / "unrelated.txt") == "previous unrelated.txt");
    for (const auto& file : result.data.at("files")) {
        const fs::path path = core::pathFromUtf8(file.at("path").get<std::string>());
        CHECK(expected.contains(core::pathToUtf8(path.filename())));
        CHECK(fs::file_size(path) > 100);
        CHECK(readFile(path) != "previous " + core::pathToUtf8(path.filename()));
        commands::Session reopened;
        CHECK(reopened.execute("document.import", {{"path", core::pathToUtf8(path)}}).ok);
    }
}

TEST_CASE("Print export does not silently publish filesystem-aliased Unicode names", "[commands][export][transaction]")
{
    TempDir dir;
    std::string first;
    std::string second;
    SECTION("non-ASCII case")
    {
        first = "Fín";
        second = "FÍN";
    }
    SECTION("Unicode normalization")
    {
        first = "caf\xc3\xa9";
        second = "cafe\xcc\x81";
    }
    const fs::path folder = core::pathFromUtf8(dir.file(""));
    const fs::path original = folder / core::pathFromUtf8(first + ".stl");
    writeFile(original, "previous STL");
    const bool aliases = fs::exists(folder / core::pathFromUtf8(second + ".stl"));
    commands::Session session;
    const auto a = addPrintBox(session, first);
    const auto b = addPrintBox(session, second);

    const auto result = session.execute("document.export_for_print",
                                        {{"folder", dir.file("")}, {"ids", {a, b}}, {"formats", {"stl"}}});
    INFO(result.toJson().dump());
    if (aliases) {
        // Typical macOS volumes: publication must reject the alias and restore the
        // original even though the two planned UTF-8 strings are different.
        CHECK_FALSE(result.ok);
        CHECK(readFile(original) == "previous STL");
        CHECK(std::distance(fs::directory_iterator(folder), fs::directory_iterator{}) == 1);
    } else {
        REQUIRE(result.ok);
        CHECK(result.data.at("files").size() == 2);
        CHECK(std::distance(fs::directory_iterator(folder), fs::directory_iterator{}) == 2);
    }
}

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

    SECTION("an open surface may be exported with a warning but cannot be printed")
    {
        TempDir dir;
        const auto open = addMeshPart(s, "open", false, 50.0);
        for (const char* format : {"stl", "3mf"}) {
            const auto check = s.execute("document.export_check", {{"ids", {open}}, {"format", format}});
            REQUIRE(check.ok);
            CHECK(check.data.at("passed") == false);
            CHECK(check.data.at("canExport") == true);
            CHECK(check.data.at("errors") == 0);
            const Json& part = check.data.at("parts").at(0);
            CHECK(part.at("status") == "warning");
            CHECK_THAT(part.at("warnings").at(0).get<std::string>(), ContainsSubstring("3 open edges"));
            CHECK_FALSE(part.contains("minWall"));
            const std::string path = dir.file(std::string("surface.") + format);
            REQUIRE(s.execute("document.export", {{"path", path}, {"ids", {open}},
                                                   {"expectedState", check.data.at("state")}}).ok);
            CHECK(fs::file_size(core::pathFromUtf8(path)) > 0);
        }
        // ... but not exported for printing.
        auto r = s.execute("document.export_check", {{"ids", {open}}, {"print", true}});
        REQUIRE(r.ok);
        CHECK(r.data.at("canExport") == false);
        CHECK(r.data.at("errors") == 1);
        const auto print = s.execute("document.export_for_print", {{"ids", {open}}, {"folder", dir.file("print")}});
        CHECK_FALSE(print.ok);
        CHECK_THAT(print.error, ContainsSubstring("not every part is a watertight solid"));
    }

    SECTION("a mesh without exact geometry cannot be STEP")
    {
        const auto closed = addMeshPart(s, "closed", true, 80.0);
        const auto r = s.execute("document.export_check", {{"ids", {closed}}, {"format", "step"}});
        REQUIRE(r.ok);
        CHECK(r.data.at("canExport") == false);
        CHECK_THAT(r.data.at("parts").at(0).at("errors").at(0).get<std::string>(),
                   ContainsSubstring("STEP needs exact geometry"));
    }

    SECTION("a non-manifold mesh is a warning for a plain export (CAD-115) and an error for printing")
    {
        model::Part part;
        part.name = "non-manifold";
        part.mesh = std::make_shared<const core::TriangleMesh>(
            std::vector<core::Vec3>{{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {0, -10, 0}, {0, 0, 10}},
            std::vector<core::TriangleMesh::Triangle>{{0, 1, 2}, {1, 0, 3}, {0, 1, 4}});
        const auto id = s.document().addPart(std::move(part));
        const auto r = s.execute("document.export_check", {{"ids", {id}}, {"format", "stl"}});
        REQUIRE(r.ok);
        CHECK(r.data.at("canExport") == true);
        CHECK(r.data.at("parts").at(0).at("status") == "warning");
        const auto p = s.execute("document.export_check", {{"ids", {id}}, {"print", true}});
        REQUIRE(p.ok);
        CHECK(p.data.at("canExport") == false);
        CHECK(p.data.at("parts").at(0).at("status") == "error");
    }
}

namespace {

/// A mesh of closed cubes, one for each shift (edge length 10 mm, outward normals).
model::PartId addCubes(commands::Session& s, const std::string& name, const std::vector<core::Vec3>& shifts)
{
    std::vector<core::Vec3> verts;
    std::vector<core::TriangleMesh::Triangle> tris{{0, 2, 3}, {0, 3, 1}, {4, 5, 7}, {4, 7, 6}, {0, 1, 5}, {0, 5, 4},
                                                   {2, 6, 7}, {2, 7, 3}, {0, 4, 6}, {0, 6, 2}, {1, 3, 7}, {1, 7, 5}};
    std::vector<core::TriangleMesh::Triangle> all;
    for (const auto& shift : shifts) {
        const auto base = static_cast<std::uint32_t>(verts.size());
        for (int i = 0; i < 8; ++i)
            verts.push_back(core::Vec3{i & 1 ? 10.0 : 0.0, i & 2 ? 10.0 : 0.0, i & 4 ? 10.0 : 0.0} + shift);
        for (auto t : tris)
            all.push_back({base + t[0], base + t[1], base + t[2]});
    }
    model::Part p;
    p.name = name;
    p.mesh = std::make_shared<const core::TriangleMesh>(std::move(verts), std::move(all));
    return s.document().addPart(std::move(p));
}

Json partReport(const commands::Json& result)
{
    return result.at("parts").at(0);
}

bool mentions(const Json& list, const std::string& text)
{
    for (const auto& item : list)
        if (item.get<std::string>().find(text) != std::string::npos)
            return true;
    return false;
}

} // namespace

TEST_CASE("The export check refuses meshes whose shells cut through each other (CAD-32)", "[commands][export]")
{
    // Every edge is used twice and the orientation is right, so the old check said "Ready to export".
    commands::Session s;
    const auto id = addCubes(s, "Overlap", {{0, 0, 0}, {5, 2, 3}});
    const auto print = s.execute("document.export_check", {{"ids", {id}}, {"print", true}, {"minWall", 0}});
    REQUIRE(print.ok);
    CHECK_FALSE(print.data.at("canExport").get<bool>());
    CHECK_FALSE(print.data.at("passed").get<bool>());
    const Json report = partReport(print.data);
    CHECK(report.at("selfIntersections").get<std::size_t>() > 0);
    CHECK(mentions(report.at("errors"), "through each other"));

    // Not for printing: a warning, and the strict export writes nothing.
    const auto plain = s.execute("document.export_check", {{"ids", {id}}, {"minWall", 0}});
    REQUIRE(plain.ok);
    CHECK_FALSE(plain.data.at("passed").get<bool>());
    TempDir dir;
    const auto strict = s.execute("document.export", {{"path", dir.file("overlap.stl")}, {"ids", {id}}, {"requireValid", true}});
    CHECK_FALSE(strict.ok);
    CHECK_THAT(strict.error, ContainsSubstring("through each other"));
    CHECK_FALSE(fs::exists(dir.file("overlap.stl")));
    const auto printed = s.execute("document.export_for_print", {{"folder", dir.file("print")}, {"ids", {id}}});
    CHECK_FALSE(printed.ok);
    CHECK_THAT(printed.error, ContainsSubstring("through each other"));
}

TEST_CASE("The export check refuses bodies that touch in a point and tells about separate shells (CAD-32)",
          "[commands][export]")
{
    commands::Session s;
    const auto pinched = addCubes(s, "Pinched", {{0, 0, 0}, {10, 10, 10}});
    const auto check = s.execute("document.export_check", {{"ids", {pinched}}, {"print", true}, {"minWall", 0}});
    REQUIRE(check.ok);
    CHECK_FALSE(check.data.at("canExport").get<bool>());
    CHECK_FALSE(partReport(check.data).at("manifold").get<bool>());
    CHECK(mentions(partReport(check.data).at("errors"), "touch"));

    // Two cubes apart: nothing cuts through anything, but it is not one body.
    const auto apart = addCubes(s, "Apart", {{0, 0, 0}, {30, 0, 0}});
    const auto two = s.execute("document.export_check", {{"ids", {apart}}, {"print", true}, {"minWall", 0}});
    REQUIRE(two.ok);
    CHECK(two.data.at("canExport").get<bool>());
    CHECK(partReport(two.data).at("selfIntersections").get<std::size_t>() == 0);
    CHECK(mentions(partReport(two.data).at("warnings"), "separate shells"));
}

TEST_CASE("A single clean solid passes the intersection check (CAD-32)", "[commands][export]")
{
    commands::Session s;
    const auto cube = addCubes(s, "Cube", {{0, 0, 0}});
    const auto check = s.execute("document.export_check", {{"ids", {cube}}, {"format", "stl"}, {"minWall", 0}});
    REQUIRE(check.ok);
    INFO(check.toJson().dump());
    CHECK(check.data.at("passed").get<bool>());
    CHECK(partReport(check.data).at("selfIntersections").get<std::size_t>() == 0);
}

TEST_CASE("Export commands run as background work (CAD-30)", "[commands][export][background]")
{
    commands::Session s;
    const auto overlap = addCubes(s, "Overlap", {{0, 0, 0}, {5, 2, 3}});
    const auto clean = addCubes(s, "Clean", {{50, 0, 0}});
    TempDir dir;

    SECTION("Only the export commands can be planned")
    {
        CHECK(s.registry().isPlanned("document.export_check"));
        CHECK(s.registry().isPlanned("document.export"));
        CHECK(s.registry().isPlanned("document.export_for_print"));
        CHECK_FALSE(s.registry().isPlanned("part.check"));
        const auto plan = s.plan("part.check", {{"id", clean}});
        REQUIRE(std::holds_alternative<commands::CommandResult>(plan));
        CHECK_FALSE(std::get<commands::CommandResult>(plan).ok);
    }

    SECTION("An exact body weighs more than its view mesh: it is meshed again for the file")
    {
        model::Part box;
        box.name = "Box";
        box.shape = geometry::makeBox(10, 20, 30);
        box.mesh = std::make_shared<const core::TriangleMesh>(geometry::tessellate(*box.shape));
        const std::size_t view = box.mesh->triangleCount();
        const auto boxId = s.document().addPart(std::move(box));
        auto plan = s.plan("document.export_check", {{"ids", {boxId}}, {"minWall", 0}});
        REQUIRE(std::holds_alternative<commands::BackgroundWork>(plan));
        CHECK(std::get<commands::BackgroundWork>(plan).weight == 20 * view);
    }

    SECTION("The planned work gives the result of the command, with progress")
    {
        const Json params{{"ids", {overlap, clean}}, {"print", true}, {"minWall", 0}};
        const auto direct = s.execute("document.export_check", params);
        REQUIRE(direct.ok);
        auto plan = s.plan("document.export_check", params);
        REQUIRE(std::holds_alternative<commands::BackgroundWork>(plan));
        const auto& work = std::get<commands::BackgroundWork>(plan);
        CHECK(work.weight == 36);

        std::vector<double> fractions;
        std::string lastText;
        commands::JobControl control;
        control.onProgress = [&](double f, const std::string& text) {
            fractions.push_back(f);
            lastText = text;
        };
        const auto outcome = commands::runWork("document.export_check", work, control);
        REQUIRE(outcome.result.ok);
        CHECK_FALSE(outcome.cancelled);
        CHECK(outcome.result.data == direct.data);
        REQUIRE_FALSE(fractions.empty());
        CHECK(std::is_sorted(fractions.begin(), fractions.end()));
        CHECK(fractions.back() <= 1.0);
        CHECK_THAT(lastText, ContainsSubstring("Clean"));
    }

    SECTION("The work belongs to the document as it was planned")
    {
        auto plan = s.plan("document.export_check", {{"ids", {clean}}, {"minWall", 0}});
        REQUIRE(std::holds_alternative<commands::BackgroundWork>(plan));
        // The user keeps editing while the check runs.
        REQUIRE(s.execute("part.rename", {{"id", clean}, {"name", "Renamed"}}).ok);
        REQUIRE(s.execute("part.remove", {{"id", overlap}}).ok);
        const auto outcome = commands::runWork("document.export_check", std::get<commands::BackgroundWork>(plan), {});
        REQUIRE(outcome.result.ok);
        CHECK(partReport(outcome.result.data).at("name") == "Clean");
    }

    SECTION("Bad parameters and a stale approval are refused when planning")
    {
        const auto missing = s.plan("document.export", {{"path", dir.file("a.stl")}, {"ids", {9999}}});
        REQUIRE(std::holds_alternative<commands::CommandResult>(missing));
        CHECK_FALSE(std::get<commands::CommandResult>(missing).ok);
        const auto stale = s.plan("document.export", {{"path", dir.file("a.stl")}, {"ids", {clean}}, {"expectedState", "old"}});
        REQUIRE(std::holds_alternative<commands::CommandResult>(stale));
        CHECK_THAT(std::get<commands::CommandResult>(stale).error, ContainsSubstring("changed after the check"));
        const auto invalid = s.plan("document.export", {{"ids", {clean}}});
        REQUIRE(std::holds_alternative<commands::CommandResult>(invalid));
        CHECK_THAT(std::get<commands::CommandResult>(invalid).error, ContainsSubstring("Invalid parameters"));
        CHECK_FALSE(fs::exists(dir.file("a.stl")));
    }

    SECTION("Cancel stops an export before it writes anything")
    {
        auto plan = s.plan("document.export", {{"path", dir.file("c.stl")}, {"ids", {clean}}});
        REQUIRE(std::holds_alternative<commands::BackgroundWork>(plan));
        commands::JobControl control;
        control.cancelled = [] { return true; };
        const auto outcome = commands::runWork("document.export", std::get<commands::BackgroundWork>(plan), control);
        CHECK(outcome.cancelled);
        CHECK_FALSE(outcome.result.ok);
        CHECK_FALSE(fs::exists(dir.file("c.stl")));

        auto printPlan = s.plan("document.export_for_print", {{"folder", dir.file("print")}, {"ids", {clean}}});
        REQUIRE(std::holds_alternative<commands::BackgroundWork>(printPlan));
        const auto printed = commands::runWork("document.export_for_print", std::get<commands::BackgroundWork>(printPlan), control);
        CHECK(printed.cancelled);
        CHECK_FALSE(fs::exists(dir.file("print") + "/Clean.stl"));
    }

    SECTION("Cancel stops the check between its steps")
    {
        auto plan = s.plan("document.export_check", {{"ids", {clean}}, {"minWall", 0}});
        REQUIRE(std::holds_alternative<commands::BackgroundWork>(plan));
        int polls = 0;
        commands::JobControl control;
        control.cancelled = [&] { return ++polls > 1; };
        const auto outcome = commands::runWork("document.export_check", std::get<commands::BackgroundWork>(plan), control);
        CHECK(outcome.cancelled);
    }

    SECTION("The planned work writes the same file as the command")
    {
        auto plan = s.plan("document.export", {{"path", dir.file("w.stl")}, {"ids", {clean}}});
        REQUIRE(std::holds_alternative<commands::BackgroundWork>(plan));
        const auto outcome = commands::runWork("document.export", std::get<commands::BackgroundWork>(plan), {});
        REQUIRE(outcome.result.ok);
        const auto direct = s.execute("document.export", {{"path", dir.file("d.stl")}, {"ids", {clean}}});
        REQUIRE(direct.ok);
        CHECK(fs::file_size(dir.file("w.stl")) == fs::file_size(dir.file("d.stl")));
        CHECK(outcome.result.data.at("triangles") == direct.data.at("triangles"));
    }
}
