// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/project/ProjectFile.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <miniz.h>

#include <filesystem>
#include <memory>
#include <random>
#include <stdexcept>

using namespace aerocad;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir()
    {
        std::random_device rd;
        m_path = fs::temp_directory_path() / ("aerocad_project_" + std::to_string(rd()));
        fs::create_directories(m_path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

core::TriangleMesh tetra()
{
    // Coordinates that do not survive float32, to prove the codec is lossless.
    return core::TriangleMesh({{0, 0, 0}, {10.123456789012345, 0, 0}, {0, 1e-9, 0}, {0, 0, 123456.78901234}},
                              {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}});
}

std::string zipWithManifest(const std::string& manifest)
{
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    REQUIRE(mz_zip_writer_add_mem(&zip, "manifest.json", manifest.data(), manifest.size(), MZ_DEFAULT_LEVEL));
    void* buf = nullptr;
    size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buf, &size));
    std::string out(static_cast<const char*>(buf), size);
    mz_free(buf);
    mz_zip_writer_end(&zip);
    return out;
}

std::size_t filesIn(const fs::path& dir)
{
    std::size_t n = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(dir))
        ++n;
    return n;
}

} // namespace

TEST_CASE("Mesh codec is lossless and rejects corrupt data", "[project]")
{
    const core::TriangleMesh mesh = tetra();
    const std::string bytes = project::encodeMesh(mesh);
    const core::TriangleMesh back = project::decodeMesh(bytes);
    CHECK(back.vertices() == mesh.vertices());
    CHECK(back.triangles() == mesh.triangles());

    CHECK_THROWS_AS(project::decodeMesh(bytes.substr(0, bytes.size() - 1)), project::ProjectError);
    CHECK_THROWS_AS(project::decodeMesh("NOTAMESH"), project::ProjectError);
    std::string badIndex = bytes;
    badIndex[badIndex.size() - 1] = '\x7f'; // last index far out of range
    CHECK_THROWS_AS(project::decodeMesh(badIndex), project::ProjectError);
}

TEST_CASE("Project data round-trips through the file format", "[project]")
{
    project::ProjectData data;
    model::Part part;
    part.id = 7;
    part.name = "Scan";
    part.source = "/tmp/scan.stl";
    part.visible = false;
    part.color = "#2ea89a";
    part.importUnits = "cm";
    part.mesh = std::make_shared<const core::TriangleMesh>(tetra());
    part.shape = geometry::makeBox(10, 20, 30);
    part.selection = std::make_shared<const core::TriangleSet>(core::TriangleSet{1, 3});
    data.parts.push_back(part);
    data.timelineParts.push_back({3, model::PartKind::Body, 9, "Wing", true, nullptr, "#c94f45"});
    data.design.features.push_back({3, "box", "Box1", {{"corner", {0, 0, 0}}, {"size", {"w", 1, 1}}}, false});
    data.design.parameters.push_back({"w", "12 mm", "span"});
    data.design.frames.push_back({"Scan frame", {1, 2, 3}, {0, 0, 1}, {1, 0, 0}, "from scan"});
    data.design.nextId = 4;
    data.design.marker = 1;
    cad::Design::State b = data.design;
    b.parameters[0].expression = "20 mm";
    const auto bId = data.variants.add("", b);
    data.view = {{"pitch", -30}};
    data.generator = "AeroCAD test";

    const project::ProjectData back = project::decodeProject(project::encodeProject(data));
    CHECK(back.formatVersion == project::kFormatVersion);
    CHECK(back.generator == "AeroCAD test");
    REQUIRE(back.parts.size() == 1);
    CHECK(back.parts[0].id == 7);
    CHECK(back.parts[0].name == "Scan");
    CHECK(back.parts[0].source == "/tmp/scan.stl");
    CHECK_FALSE(back.parts[0].visible);
    CHECK(back.parts[0].color == "#2ea89a");
    CHECK(back.parts[0].importUnits == "cm"); // CAD-239
    CHECK(back.parts[0].mesh->vertices() == part.mesh->vertices());
    REQUIRE(back.parts[0].selection);
    CHECK(*back.parts[0].selection == core::TriangleSet{1, 3});
    REQUIRE(back.parts[0].shape.has_value());
    CHECK(back.parts[0].shape->volume() == Approx(6000));
    REQUIRE(back.timelineParts.size() == 1);
    CHECK(back.timelineParts[0].id == 9);
    CHECK(back.timelineParts[0].name == "Wing");
    CHECK(back.timelineParts[0].color == "#c94f45");
    REQUIRE(back.design.features.size() == 1);
    CHECK(back.design.features[0].params == data.design.features[0].params);
    CHECK(back.design.parameters[0].expression == "12 mm");
    CHECK(back.design.parameters[0].comment == "span");
    REQUIRE(back.design.frames.size() == 1);
    CHECK(back.design.frames[0].origin == core::Vec3{1, 2, 3});
    CHECK(back.design.nextId == 4);
    CHECK(back.design.marker == 1);
    REQUIRE(back.variants.size() == 2);
    CHECK(back.variants.active() == 1);
    CHECK(back.variants.find(bId)->name == "B");
    CHECK(back.variants.find(bId)->state.parameters[0].expression == "20 mm");
    CHECK(back.view == data.view);
}

TEST_CASE("Newer or foreign files are rejected with a clear message", "[project]")
{
    CHECK_THROWS_AS(project::decodeProject("not a zip"), project::ProjectError);
    CHECK_THROWS_AS(project::decodeProject(zipWithManifest(R"({"format": "other", "version": 1})")),
                    project::ProjectError);
    try {
        (void)project::decodeProject(zipWithManifest(R"({"format": "aerocad.project", "version": 2})"));
        FAIL("expected an error");
    } catch (const project::ProjectError& e) {
        CHECK(std::string(e.what()).find("newer") != std::string::npos);
    }
    CHECK_THROWS_AS(project::decodeProject(zipWithManifest(R"({"format": "aerocad.project", "version": "x"})")),
                    project::ProjectError);
    CHECK_THROWS_AS(project::decodeProject(zipWithManifest(R"({"format": "aerocad.project", "version": 1,
        "parts": [{"id": 1, "name": "x"}]})")),
                    project::ProjectError); // no mesh, no B-rep
    // Minimal valid version 1 file.
    const project::ProjectData empty =
        project::decodeProject(zipWithManifest(R"({"format": "aerocad.project", "version": 1})"));
    CHECK(empty.parts.empty());
    CHECK(empty.variants.size() == 1);
}

TEST_CASE("Atomic write: an interrupted save leaves the old file intact", "[project]")
{
    TempDir dir;
    const fs::path file = dir.path() / "p.aerocad";
    project::writeFileAtomically(file, "old");
    CHECK(project::readFile(file) == "old");

    CHECK_THROWS_AS(project::writeFileAtomically(file, "new contents", [] { throw std::runtime_error("power cut"); }),
                    std::runtime_error);
    CHECK(project::readFile(file) == "old");
    CHECK(filesIn(dir.path()) == 1); // temporary file removed

    project::writeFileAtomically(file, "new");
    CHECK(project::readFile(file) == "new");
    CHECK(filesIn(dir.path()) == 1);

    CHECK_THROWS_AS(project::writeFileAtomically(dir.path() / "missing" / "p.aerocad", "x"), project::ProjectError);
}
