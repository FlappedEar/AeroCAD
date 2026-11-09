// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshCheck.hpp"
#include "aerocad/core/Path.hpp"
#include "aerocad/geometry/Modeling.hpp"
#include "aerocad/geometry/OcctShape.hpp"
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/geometry/Sketch.hpp"
#include "aerocad/geometry/Tessellation.hpp"
#include "aerocad/io/Export.hpp"
#include "aerocad/io/Import.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <miniz.h>

#include <IFSelect_ReturnStatus.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TDF_Label.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopLoc_Location.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace aerocad;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

namespace {

class TempDir {
public:
    TempDir()
    {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path() / ("aerocad_test_export_" + std::to_string(stamp) + "_" +
                                              std::to_string(counter++) + "_" + std::to_string(std::random_device{}()));
        fs::create_directories(m_path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] fs::path file(const fs::path& name) const { return m_path / name; }
    [[nodiscard]] std::size_t entries() const
    {
        return static_cast<std::size_t>(std::distance(fs::directory_iterator(m_path), fs::directory_iterator{}));
    }

private:
    fs::path m_path;
};

core::TriangleMesh tetrahedron(double s = 10.0, double dx = 0.0)
{
    return core::TriangleMesh({{dx, 0, 0}, {dx + s, 0, 0}, {dx, s, 0}, {dx, 0, s}},
                              {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}});
}

std::string readBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string zipEntry(const fs::path& path, const char* name)
{
    const std::string bytes = readBytes(path);
    mz_zip_archive zip{};
    REQUIRE(mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0));
    std::size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&zip, name, &size, 0);
    std::string out;
    if (data != nullptr) {
        out.assign(static_cast<const char*>(data), size);
        mz_free(data);
    }
    mz_zip_reader_end(&zip);
    return out;
}

} // namespace

TEST_CASE("Export formats", "[io][export]")
{
    CHECK(io::canExport(io::FileFormat::Stl));
    CHECK(io::canExport(io::FileFormat::ThreeMf));
    CHECK(io::canExport(io::FileFormat::Step));
    CHECK_FALSE(io::canExport(io::FileFormat::Obj));
    CHECK_FALSE(io::canExport(io::FileFormat::Unknown));
}

TEST_CASE("Binary STL export round trip", "[io][export][stl]")
{
    TempDir dir;
    const core::TriangleMesh a = tetrahedron();
    const core::TriangleMesh b = tetrahedron(5.0, 50.0);
    const fs::path path = dir.file("out.stl");
    io::exportFile(path, {{"a", &a, nullptr}, {"b", &b, nullptr}});

    const std::string bytes = readBytes(path);
    REQUIRE(bytes.size() == 84 + 8 * 50);
    CHECK(bytes.compare(0, 6, "solid ") != 0); // must not look like ASCII STL
    std::uint32_t count = 0;
    std::memcpy(&count, bytes.data() + 80, 4);
    CHECK(count == 8);

    const io::ImportResult back = io::importFile(path);
    CHECK(back.mesh.triangleCount() == 8);
    CHECK(back.mesh.vertexCount() == 8);
    const core::MeshCheck check = core::checkMesh(back.mesh);
    CHECK(check.watertight());
    CHECK(check.shells == 2);
    CHECK(check.signedVolume == Approx(1000.0 / 6.0 + 125.0 / 6.0));
    CHECK(dir.entries() == 1); // no temporary file left behind
}

TEST_CASE("3MF export with unit, names and welded B-rep tessellation", "[io][export][3mf]")
{
    TempDir dir;
    const core::TriangleMesh tet = tetrahedron(10.0, 100.0); // apart from the box
    // Tessellated box: one vertex set per face, the writer must weld them.
    const core::TriangleMesh box = geometry::tessellate(geometry::makeBox(20, 10, 5));
    const fs::path path = dir.file("out.3mf");
    io::exportFile(path, {{"tet <1> & \"x\"", &tet, nullptr}, {"box", &box, nullptr}});

    const std::string model = zipEntry(path, "3D/3dmodel.model");
    CHECK_THAT(model, ContainsSubstring(R"(unit="millimeter")"));
    CHECK_THAT(model, ContainsSubstring(R"(name="tet &lt;1&gt; &amp; &quot;x&quot;")"));
    CHECK_THAT(zipEntry(path, "_rels/.rels"), ContainsSubstring("/3D/3dmodel.model"));
    CHECK_FALSE(zipEntry(path, "[Content_Types].xml").empty());

    const io::ImportResult back = io::importFile(path);
    CHECK(back.mesh.triangleCount() == tet.triangleCount() + box.triangleCount());
    const core::MeshCheck check = core::checkMesh(back.mesh);
    CHECK(check.watertight());
    CHECK(check.shells == 2);
    CHECK(check.signedVolume == Approx(1000.0 / 6.0 + 1000.0).epsilon(1e-9));
    CHECK(back.mesh.bounds().max.x == Approx(110.0));

    // CAD-228: one body per build item, named after its object.
    REQUIRE(back.bodies.size() == 2);
    CHECK(back.bodies[0].name == "tet <1> & \"x\"");
    CHECK(back.bodies[1].name == "box");
    CHECK(back.bodies[1].mesh.triangleCount() == box.triangleCount());
    CHECK_FALSE(back.bodies[0].shape.has_value());
}

TEST_CASE("3MF export and import keep Appearance colours (CAD-228)", "[io][export][3mf]")
{
    TempDir dir;
    const core::TriangleMesh a = tetrahedron(10.0, 0.0);
    const core::TriangleMesh b = tetrahedron(10.0, 50.0);
    const core::TriangleMesh c = tetrahedron(10.0, 100.0);
    const fs::path path = dir.file("colours.3mf");
    io::exportFile(path, {{"red", &a, nullptr, "#cc3333"}, {"plain", &b, nullptr, ""}, {"red too", &c, nullptr, "#cc3333"}});

    // One material per distinct colour, declared before the objects that use it.
    const std::string model = zipEntry(path, "3D/3dmodel.model");
    CHECK_THAT(model, ContainsSubstring(R"(<basematerials id="4">)"));
    CHECK_THAT(model, ContainsSubstring(R"(displaycolor="#CC3333FF")"));
    CHECK(model.find("<base ") == model.rfind("<base "));
    CHECK(model.find("<basematerials") < model.find("<object"));
    CHECK_THAT(model, ContainsSubstring(R"(name="red" pid="4" pindex="0")"));
    CHECK_THAT(model, ContainsSubstring(R"(name="plain">)"));

    const io::ImportResult back = io::importFile(path);
    REQUIRE(back.bodies.size() == 3);
    CHECK(back.bodies[0].color == "#cc3333");
    CHECK(back.bodies[1].color.empty());
    CHECK(back.bodies[2].name == "red too");
    CHECK(back.bodies[2].color == "#cc3333");

    // A single object is one result, with its colour.
    const fs::path one = dir.file("one.3mf");
    io::exportFile(one, {{"green", &a, nullptr, "#5aa65a"}});
    const io::ImportResult single = io::importFile(one);
    CHECK(single.bodies.empty());
    CHECK(single.color == "#5aa65a");
}

TEST_CASE("STEP export round trip keeps exact geometry in mm", "[io][export][step]")
{
    TempDir dir;
    const geometry::Shape box = geometry::makeBox(30, 20, 10);
    const geometry::Shape cyl = geometry::makeCylinder(5, 40);
    const fs::path path = dir.file(core::pathFromUtf8("bryła_ż.step"));
    io::exportFile(path, {{"box", nullptr, &box}, {"cyl", nullptr, &cyl}});

    const io::ImportResult back = io::importFile(path);
    REQUIRE(back.shape.has_value());
    CHECK(back.shape->solidCount() == 2);
    CHECK(back.shape->volume() == Approx(30.0 * 20 * 10 + 3.14159265358979 * 25 * 40).epsilon(1e-6));
    CHECK(back.shape->bounds().size().z == Approx(40.0).margin(0.1)); // bounds include the tolerance gap
    CHECK(back.shape->bounds().size().z > 39.99);
    CHECK(dir.entries() == 1);

    // CAD-227: one STEP product per body, with its name (not "Open CASCADE STEP
    // translator ..."); reading gives the bodies back separately.
    REQUIRE(back.bodies.size() == 2);
    CHECK(back.bodies[0].name == "box");
    CHECK(back.bodies[1].name == "cyl");
    REQUIRE(back.bodies[1].shape.has_value());
    CHECK(back.bodies[1].shape->volume() == Approx(3.14159265358979 * 25 * 40).epsilon(1e-6));
    CHECK(back.bodies[0].mesh.triangleCount() + back.bodies[1].mesh.triangleCount() == back.mesh.triangleCount());
}

TEST_CASE("STEP import names assembly occurrences by their own names (CAD-227)", "[io][import][step]")
{
    // An assembly "Frame" with two placed occurrences of one product "Bracket".
    const geometry::Shape box = geometry::makeBox(10, 10, 10);
    Handle(TDocStd_Document) doc;
    XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", doc);
    const Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    const TDF_Label product = shapes->AddShape(box.impl()->shape, Standard_False);
    TDataStd_Name::Set(product, "Bracket");
    const TDF_Label assembly = shapes->NewShape();
    TDataStd_Name::Set(assembly, "Frame");
    gp_Trsf right;
    right.SetTranslation(gp_Vec(50, 0, 0));
    const TDF_Label left = shapes->AddComponent(assembly, product, TopLoc_Location());
    TDataStd_Name::Set(left, "Left bracket");
    const TDF_Label other = shapes->AddComponent(assembly, product, TopLoc_Location(right));
    TDataStd_Name::Set(other, "Right bracket");
    shapes->UpdateAssemblies();

    TempDir dir;
    const fs::path path = dir.file("frame.step");
    STEPCAFControl_Writer writer;
    writer.SetNameMode(Standard_True);
    REQUIRE(writer.Transfer(doc, STEPControl_AsIs));
    REQUIRE(writer.Write(core::pathToUtf8(path).c_str()) == IFSelect_RetDone);
    XCAFApp_Application::GetApplication()->Close(doc);

    const io::ImportResult back = io::importFile(path);
    REQUIRE(back.bodies.size() == 2);
    CHECK(back.bodies[0].name == "Left bracket");
    CHECK(back.bodies[1].name == "Right bracket");
    // Each occurrence keeps its placement.
    CHECK(back.bodies[0].mesh.bounds().min.x == Approx(0.0).margin(1e-6));
    CHECK(back.bodies[1].mesh.bounds().min.x == Approx(50.0).margin(1e-6));
}

TEST_CASE("STEP export keeps body names and colours (CAD-227)", "[io][export][step]")
{
    TempDir dir;
    const geometry::Shape box = geometry::makeBox(30, 20, 10);
    const geometry::Shape cyl = geometry::makeCylinder(5, 40);
    const fs::path path = dir.file("named.step");
    io::exportFile(path, {{"Skrzydło", nullptr, &box, "#cc3333"}, {"Pin", nullptr, &cyl, ""}});

    std::ifstream in(path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK_THAT(text, ContainsSubstring("PRODUCT('Pin'"));
    CHECK_THAT(text, !ContainsSubstring("Open CASCADE STEP translator"));

    const io::ImportResult back = io::importFile(path);
    REQUIRE(back.bodies.size() == 2);
    CHECK(back.bodies[0].name == "Skrzydło");
    CHECK(back.bodies[0].color == "#cc3333");
    CHECK(back.bodies[1].name == "Pin");
    CHECK(back.bodies[1].color.empty());

    // Two parts with the same geometry (a duplicated, unmoved part shares its shape): two
    // products, each with its own name and colour (Codex on #198).
    const fs::path dup = dir.file("duplicate.step");
    io::exportFile(dup, {{"Left", nullptr, &box, "#cc3333"}, {"Right", nullptr, &box, "#3333cc"}});
    const io::ImportResult twins = io::importFile(dup);
    REQUIRE(twins.bodies.size() == 2);
    CHECK(twins.bodies[0].name == "Left");
    CHECK(twins.bodies[0].color == "#cc3333");
    CHECK(twins.bodies[1].name == "Right");
    CHECK(twins.bodies[1].color == "#3333cc");

    // A single body is one result, with its colour.
    const fs::path one = dir.file("one.step");
    io::exportFile(one, {{"box", nullptr, &box, "#5aa65a"}});
    const io::ImportResult single = io::importFile(one);
    CHECK(single.bodies.empty());
    CHECK(single.color == "#5aa65a");
    REQUIRE(single.shape.has_value());
    CHECK(single.shape->volume() == Approx(6000.0).epsilon(1e-6));
}

TEST_CASE("STEP export round trip keeps a revolved dome closed at its pole", "[io][export][step]")
{
    // An arc that ends on the axis: the revolved face closes at a pole. OCCT writes such a
    // face without the degenerated edge and loses it on reading unless it is converted.
    using geometry::SketchCurve;
    const auto sketch = geometry::buildSketch(
        geometry::Plane::xz(),
        {{SketchCurve::Kind::Line, {{0, 0}, {120, 0}}},
         {SketchCurve::Kind::Line, {{120, 0}, {120, 15}}},
         {SketchCurve::Kind::Arc, {{120, 15}, {70, 38}, {0, 45}}},
         {SketchCurve::Kind::Line, {{0, 45}, {0, 0}}}});
    REQUIRE(sketch.profiles.size() == 1);
    const geometry::Shape dome = geometry::revolve(sketch.profiles[0], {0, 0, 0}, {0, 0, 1}, 360);
    const double volume = dome.volume();
    REQUIRE(volume > 1.4e6);

    TempDir dir;
    const fs::path path = dir.file("dome.step");
    io::exportFile(path, {{"dome", nullptr, &dome}});
    const io::ImportResult back = io::importFile(path);
    REQUIRE(back.shape.has_value());
    CHECK(back.shape->solidCount() == 1);
    CHECK(back.shape->volume() == Approx(volume).epsilon(1e-4));
    // The tessellation, not the shape bounds: B-spline bounds include the control points.
    CHECK(back.mesh.bounds().size().z == Approx(45.7).margin(0.1));
    const core::MeshCheck check = core::checkMesh(back.mesh);
    CHECK(check.problems().empty());
    CHECK(check.watertight());
}

TEST_CASE("Export errors", "[io][export][errors]")
{
    TempDir dir;
    const core::TriangleMesh tet = tetrahedron();

    CHECK_THROWS_AS(io::exportFile(dir.file("a.stl"), {}), io::ExportError);
    CHECK_THROWS_AS(io::exportFile(dir.file("a.obj"), {{"t", &tet, nullptr}}), io::ExportError);
    CHECK_THROWS_AS(io::exportFile(dir.file("missing/a.stl"), {{"t", &tet, nullptr}}), io::ExportError);
    CHECK_THROWS_AS(io::exportFile(dir.file("a.3mf"), {{"t", nullptr, nullptr}}), io::ExportError);
    try {
        io::exportFile(dir.file("a.step"), {{"scan", &tet, nullptr}});
        FAIL("expected ExportError");
    } catch (const io::ExportError& e) {
        CHECK_THAT(e.what(), ContainsSubstring("'scan'"));
        CHECK_THAT(e.what(), ContainsSubstring("STL or 3MF"));
    }
    CHECK(dir.entries() == 0);
}

TEST_CASE("Export replaces an existing file", "[io][export]")
{
    TempDir dir;
    const fs::path path = dir.file("x.stl");
    {
        std::ofstream out(path, std::ios::binary);
        out << "old content";
    }
    const core::TriangleMesh tet = tetrahedron();
    io::exportFile(path, {{"t", &tet, nullptr}});
    CHECK(fs::file_size(path) == 84 + 4 * 50);
}

TEST_CASE("STL export drops triangles collapsed to an edge", "[io][export][stl]")
{
    TempDir dir;
    core::TriangleMesh tet = tetrahedron();
    const auto dup = tet.addVertex({0, 0, 0});
    tet.addTriangle(0, dup, 1);
    const fs::path path = dir.file("t.stl");
    io::exportFile(path, {{"t", &tet, nullptr}});
    CHECK(fs::file_size(path) == 84 + 4 * 50);
    CHECK(core::checkMesh(io::importFile(path).mesh).watertight());
}
