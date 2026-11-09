// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"
#include "aerocad/core/Path.hpp"
#include "aerocad/core/Units.hpp"
#include "aerocad/geometry/OcctShape.hpp"
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/io/Export.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <BRepTools.hxx>
#include <IGESControl_Controller.hxx>
#include <IGESControl_Writer.hxx>
#include <miniz.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <utility>

using namespace aerocad;
using Catch::Approx;
using commands::Json;

namespace {

class UnitFiles {
public:
    UnitFiles()
        : m_path(std::filesystem::temp_directory_path() /
                 ("aerocad_import_units_" + std::to_string(std::random_device{}())))
    {
        std::filesystem::create_directories(m_path);
    }
    ~UnitFiles()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    UnitFiles(const UnitFiles&) = delete;
    UnitFiles& operator=(const UnitFiles&) = delete;

    std::filesystem::path file(const std::string& name) const { return m_path / name; }

    std::filesystem::path threeMf(const char* unit, double millimetres) const
    {
        // The same physical 10 x 20 x 30 mm tetrahedron in each declared file unit.
        std::ostringstream xml;
        xml << std::setprecision(17)
            << "<model xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\" unit=\""
            << unit << "\"><resources><object id=\"1\" type=\"model\"><mesh><vertices>"
            << "<vertex x=\"0\" y=\"0\" z=\"0\"/><vertex x=\"" << 10 / millimetres
            << "\" y=\"0\" z=\"0\"/><vertex x=\"0\" y=\"" << 20 / millimetres
            << "\" z=\"0\"/><vertex x=\"0\" y=\"0\" z=\"" << 30 / millimetres
            << "\"/></vertices><triangles>"
            << "<triangle v1=\"0\" v2=\"2\" v3=\"1\"/><triangle v1=\"0\" v2=\"1\" v3=\"3\"/>"
            << "<triangle v1=\"0\" v2=\"3\" v3=\"2\"/><triangle v1=\"1\" v2=\"2\" v3=\"3\"/>"
            << "</triangles></mesh></object></resources><build><item objectid=\"1\"/></build></model>";
        const auto path = file(std::string(unit) + ".3mf");
        const std::string text = xml.str();
        mz_zip_archive zip{};
        REQUIRE(mz_zip_writer_init_file(&zip, core::pathToUtf8(path).c_str(), 0));
        REQUIRE(mz_zip_writer_add_mem(&zip, "3D/3dmodel.model", text.data(), text.size(), MZ_DEFAULT_COMPRESSION));
        REQUIRE(mz_zip_writer_finalize_archive(&zip));
        REQUIRE(mz_zip_writer_end(&zip));
        return path;
    }

private:
    std::filesystem::path m_path;
};

Json importPart(commands::Session& session, const std::filesystem::path& path, const char* units, bool staged)
{
    if (staged)
        session.importStage().put(core::pathToUtf8(path), io::importFile(path));
    const auto result = session.execute("document.import", {{"path", core::pathToUtf8(path)}, {"units", units}});
    INFO(result.error);
    REQUIRE(result.ok);
    return result.data;
}

void checkSize(const Json& part, double x, double y, double z)
{
    CHECK(part["bounds"]["size"][0].get<double>() == Approx(x).epsilon(1e-6));
    CHECK(part["bounds"]["size"][1].get<double>() == Approx(y).epsilon(1e-6));
    CHECK(part["bounds"]["size"][2].get<double>() == Approx(z).epsilon(1e-6));
}

constexpr std::array<const char*, 4> settings{"mm", "cm", "in", "m"};

} // namespace

TEST_CASE("document.import preserves 3MF physical units independently of unitless settings", "[commands][scan][units]")
{
    UnitFiles files;
    const std::array<std::pair<const char*, double>, 4> fileUnits{{
        {"millimeter", 1}, {"centimeter", 10}, {"inch", 25.4}, {"meter", 1000}}};
    for (const auto& [unit, scale] : fileUnits) {
        const auto path = files.threeMf(unit, scale);
        for (const char* setting : settings) {
            for (const bool staged : {false, true}) {
                CAPTURE(unit, setting, staged);
                commands::Session session;
                const auto part = importPart(session, path, setting, staged);
                checkSize(part, 10, 20, 30);
                CHECK(part.value("unitSource", "") == "file");
                CHECK(part["units"] == "mm");
                CHECK(part.value("outputUnits", "") == "mm");
            }
        }
    }
}

TEST_CASE("document.import preserves STEP and IGES mesh and solid physical units", "[commands][scan][units]")
{
    UnitFiles files;
    const auto box = geometry::makeBox(10, 20, 30);
    const auto step = files.file("box.step");
    io::writeStep(step, {{"box", nullptr, &box}});
    const auto iges = files.file("box.iges");
    IGESControl_Controller::Init();
    IGESControl_Writer writer("MM", 1);
    REQUIRE(writer.AddShape(box.impl()->shape));
    writer.ComputeModel();
    REQUIRE(writer.Write(core::pathToUtf8(iges).c_str()));
    for (const auto& path : {step, iges}) {
        for (const char* setting : settings) {
            for (const bool staged : {false, true}) {
                CAPTURE(path, setting, staged);
                commands::Session session;
                const auto part = importPart(session, path, setting, staged);
                checkSize(part, 10, 20, 30);
                CHECK(part.value("unitSource", "") == "file");
                CHECK(part["units"] == "mm");
                CHECK(part.value("outputUnits", "") == "mm");
                const auto* stored = session.document().part(part["id"].get<model::PartId>());
                REQUIRE(stored != nullptr);
                REQUIRE(stored->shape);
                CHECK(stored->shape->volume() == Approx(6000).epsilon(1e-6));
            }
        }
    }
}

TEST_CASE("document.import still scales unitless STL OBJ PLY and BREP", "[commands][scan][units]")
{
    UnitFiles files;
    const auto shape = geometry::makeBox(1, 2, 3);
    const core::TriangleMesh mesh({{0, 0, 0}, {1, 0, 0}, {0, 2, 0}, {0, 0, 3}},
                                  {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}});
    io::writeStl(files.file("box.stl"), {{"box", &mesh, nullptr}});
    REQUIRE(BRepTools::Write(shape.impl()->shape, core::pathToUtf8(files.file("box.brep")).c_str()));
    std::ofstream(files.file("box.obj")) << "v 0 0 0\nv 1 0 0\nv 0 2 0\nv 0 0 3\nf 1 3 2\nf 1 2 4\nf 1 4 3\nf 2 3 4\n";
    std::ofstream(files.file("box.ply")) << "ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\n"
        "property float y\nproperty float z\nelement face 4\nproperty list uchar int vertex_indices\nend_header\n"
        "0 0 0\n1 0 0\n0 2 0\n0 0 3\n3 0 2 1\n3 0 1 3\n3 0 3 2\n3 1 2 3\n";
    for (const char* extension : {"stl", "obj", "ply", "brep"}) {
        for (const char* setting : settings) {
            for (const bool staged : {false, true}) {
                CAPTURE(extension, setting, staged);
                commands::Session session;
                const auto part = importPart(session, files.file(std::string("box.") + extension), setting, staged);
                const double scale = core::toMillimetres(*core::parseLengthUnit(setting));
                checkSize(part, scale, 2 * scale, 3 * scale);
                CHECK(part.value("unitSource", "") == "importSetting");
                CHECK(part["units"] == setting);
                CHECK(part.value("outputUnits", "") == "mm");
                const auto* stored = session.document().part(part["id"].get<model::PartId>());
                REQUIRE(stored != nullptr);
                if (stored->shape)
                    CHECK(stored->shape->volume() == Approx(6 * scale * scale * scale).epsilon(1e-6));
            }
        }
    }
}

TEST_CASE("document.import gives one part per STEP body with its name and colour (CAD-227)", "[commands][import]")
{
    UnitFiles files;
    const auto box = geometry::makeBox(10, 20, 30);
    const auto cyl = geometry::makeCylinder(2, 5);
    const auto step = files.file("two.step");
    io::writeStep(step, {{"Wing", nullptr, &box, "#cc3333"}, {"", nullptr, &cyl, ""}});

    commands::Session session;
    const auto first = importPart(session, step, "mm", false);
    CHECK(first["name"] == "Wing");
    CHECK(first["color"] == "#cc3333");
    REQUIRE(first.contains("parts"));
    REQUIRE(first["parts"].size() == 2);
    // No name in the file: the file name and a number.
    CHECK(first["parts"][1]["name"] == "two 2");
    CHECK(first["parts"][1]["color"] == "");
    checkSize(first["parts"][1], 4, 4, 5);
    REQUIRE(session.document().parts().size() == 2);
    for (const auto& p : session.document().parts()) {
        REQUIRE(p.shape);
        CHECK(p.shape->solidCount() == 1);
    }

    // One undo step for the whole file.
    REQUIRE(session.execute("edit.undo", Json::object()).ok);
    CHECK(session.document().parts().empty());
    REQUIRE(session.execute("edit.redo", Json::object()).ok);
    CHECK(session.document().parts().size() == 2);
}
