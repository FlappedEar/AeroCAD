// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/Path.hpp"
#include "aerocad/geometry/OcctShape.hpp"
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/io/Import.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <BRepTools.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESControl_Controller.hxx>
#include <IGESControl_Writer.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Writer.hxx>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace aerocad;
using Catch::Approx;

namespace {

/// Unique temporary directory, removed at scope exit.
class TempDir {
public:
    TempDir()
    {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path() /
                 ("aerocad_test_io_" + std::to_string(stamp) + "_" + std::to_string(counter++) + "_" +
                  std::to_string(std::random_device{}()));
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

private:
    fs::path m_path;
};

void writeFile(const fs::path& path, const std::string& content)
{
    std::ofstream out(path, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(out.good());
}

// --- STL helpers -----------------------------------------------------------------

using Tri = std::array<std::array<float, 3>, 3>;

std::vector<Tri> cubeTriangles(float s)
{
    const std::array<std::array<float, 3>, 8> p{{{0, 0, 0},
                                                 {s, 0, 0},
                                                 {s, s, 0},
                                                 {0, s, 0},
                                                 {0, 0, s},
                                                 {s, 0, s},
                                                 {s, s, s},
                                                 {0, s, s}}};
    const int quads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    std::vector<Tri> tris;
    for (const auto& q : quads) {
        tris.push_back({p[static_cast<std::size_t>(q[0])], p[static_cast<std::size_t>(q[1])],
                        p[static_cast<std::size_t>(q[2])]});
        tris.push_back({p[static_cast<std::size_t>(q[0])], p[static_cast<std::size_t>(q[2])],
                        p[static_cast<std::size_t>(q[3])]});
    }
    return tris;
}

void appendU32(std::string& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
}

void appendU16(std::string& out, std::uint16_t v)
{
    out.push_back(static_cast<char>(v & 0xFFU));
    out.push_back(static_cast<char>((v >> 8U) & 0xFFU));
}

void appendF32(std::string& out, float f)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    appendU32(out, bits);
}

std::string binaryStl(const std::vector<Tri>& tris, const std::string& header)
{
    std::string out = header.substr(0, 80);
    out.resize(80, ' ');
    appendU32(out, static_cast<std::uint32_t>(tris.size()));
    for (const Tri& t : tris) {
        for (int i = 0; i < 3; ++i)
            appendF32(out, 0.0F); // normal (ignored by the reader)
        for (const auto& v : t)
            for (float c : v)
                appendF32(out, c);
        appendU16(out, 0);
    }
    return out;
}

std::string asciiStl(const std::vector<Tri>& tris)
{
    std::string out = "solid cube\n";
    for (const Tri& t : tris) {
        out += "  FACET NORMAL 0 0 0\n    outer loop\n";
        for (const auto& v : t) {
            out += "      vertex   " + std::to_string(v[0]) + "\t" + std::to_string(v[1]) + " " +
                   std::to_string(v[2]) + "\r\n";
        }
        out += "    endloop\n  endfacet\n";
    }
    out += "endsolid cube\n";
    return out;
}

// --- Minimal ZIP writer (stored entries) for 3MF fixtures ------------------------

std::uint32_t crc32(const std::string& data)
{
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const char ch : data) {
        crc ^= static_cast<unsigned char>(ch);
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1U) ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
    }
    return ~crc;
}

std::string makeZip(const std::vector<std::pair<std::string, std::string>>& entries)
{
    std::string out;
    std::string central;
    for (const auto& [name, data] : entries) {
        const auto offset = static_cast<std::uint32_t>(out.size());
        const std::uint32_t crc = crc32(data);
        const auto size = static_cast<std::uint32_t>(data.size());
        const auto nameLen = static_cast<std::uint16_t>(name.size());

        appendU32(out, 0x04034b50U);
        appendU16(out, 20); // version needed
        appendU16(out, 0);  // flags
        appendU16(out, 0);  // method: stored
        appendU16(out, 0);  // time
        appendU16(out, 0x21); // date
        appendU32(out, crc);
        appendU32(out, size);
        appendU32(out, size);
        appendU16(out, nameLen);
        appendU16(out, 0);
        out += name;
        out += data;

        appendU32(central, 0x02014b50U);
        appendU16(central, 20);
        appendU16(central, 20);
        appendU16(central, 0);
        appendU16(central, 0);
        appendU16(central, 0);
        appendU16(central, 0x21);
        appendU32(central, crc);
        appendU32(central, size);
        appendU32(central, size);
        appendU16(central, nameLen);
        appendU16(central, 0); // extra
        appendU16(central, 0); // comment
        appendU16(central, 0); // disk
        appendU16(central, 0); // internal attrs
        appendU32(central, 0); // external attrs
        appendU32(central, offset);
        central += name;
    }
    const auto centralOffset = static_cast<std::uint32_t>(out.size());
    out += central;
    appendU32(out, 0x06054b50U);
    appendU16(out, 0);
    appendU16(out, 0);
    appendU16(out, static_cast<std::uint16_t>(entries.size()));
    appendU16(out, static_cast<std::uint16_t>(entries.size()));
    appendU32(out, static_cast<std::uint32_t>(central.size()));
    appendU32(out, centralOffset);
    appendU16(out, 0);
    return out;
}

const char* kRels = R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Target="/3D/model_main.model" Id="rel0"
      Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>
</Relationships>)";

const char* kContentTypes = R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>
</Types>)";

/// Unit cube mesh object (0..1) in 3MF XML with the given element prefix.
std::string cubeObject3mf(const std::string& id, const std::string& pfx)
{
    std::string xml = "<" + pfx + "object id=\"" + id + "\" type=\"model\"><" + pfx + "mesh><" + pfx + "vertices>";
    const int verts[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    for (const auto& v : verts) {
        xml += "<" + pfx + "vertex x=\"" + std::to_string(v[0]) + "\" y=\"" + std::to_string(v[1]) + "\" z=\"" +
               std::to_string(v[2]) + "\"/>";
    }
    xml += "</" + pfx + "vertices><" + pfx + "triangles>";
    const int tris[12][3] = {{0, 3, 2}, {0, 2, 1}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                             {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
    for (const auto& t : tris) {
        xml += "<" + pfx + "triangle v1=\"" + std::to_string(t[0]) + "\" v2=\"" + std::to_string(t[1]) +
               "\" v3=\"" + std::to_string(t[2]) + "\"/>";
    }
    xml += "</" + pfx + "triangles></" + pfx + "mesh></" + pfx + "object>";
    return xml;
}

std::string write3mf(const TempDir& dir, const std::string& fileName, const std::string& model,
                     const char* rels = kRels, const std::string& modelPath = "3D/model_main.model")
{
    std::vector<std::pair<std::string, std::string>> entries{{"[Content_Types].xml", kContentTypes}};
    if (rels)
        entries.emplace_back("_rels/.rels", rels);
    entries.emplace_back(modelPath, model);
    const fs::path path = dir.file(fileName);
    writeFile(path, makeZip(entries));
    return core::pathToUtf8(path);
}

// --- PLY helpers -----------------------------------------------------------------

/// Appends the bytes of a trivially copyable value in the requested byte order.
template <typename T> void appendBinary(std::string& out, T value, bool bigEndian)
{
    unsigned char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    const bool hostLittle = std::endian::native == std::endian::little;
    if (hostLittle == bigEndian) {
        for (std::size_t i = 0; i < sizeof(T) / 2; ++i)
            std::swap(bytes[i], bytes[sizeof(T) - 1 - i]);
    }
    out.append(reinterpret_cast<const char*>(bytes), sizeof(T));
}

/// Unit cube as 8 vertices and 6 quads (outward orientation).
constexpr float kCubeVertices[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                       {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
constexpr int kCubeQuads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};

/// Binary PLY cube of size 2 with extra vertex properties (colour, normals, a list),
/// quad faces with extra per-face properties and a trailing unknown element. The x
/// coordinate is stored as double, y and z as float.
std::string binaryPlyCube(bool bigEndian)
{
    std::string out = std::string("ply\nformat ") + (bigEndian ? "binary_big_endian" : "binary_little_endian") +
                      " 1.0\ncomment generated by test\nobj_info scanner\n"
                      "element vertex 8\n"
                      "property uchar red\nproperty double x\nproperty float32 y\nproperty float z\n"
                      "property float nx\nproperty float ny\nproperty float nz\n"
                      "property list uchar int extra\n"
                      "element face 6\n"
                      "property uint16 flags\nproperty list uint8 uint32 vertex_indices\nproperty float quality\n"
                      "element edge 2\nproperty int vertex1\nproperty int vertex2\n"
                      "end_header\n";
    for (const auto& v : kCubeVertices) {
        appendBinary<std::uint8_t>(out, 200, bigEndian);
        appendBinary<double>(out, static_cast<double>(v[0]) * 2.0, bigEndian);
        appendBinary<float>(out, v[1] * 2.0F, bigEndian);
        appendBinary<float>(out, v[2] * 2.0F, bigEndian);
        for (int k = 0; k < 3; ++k)
            appendBinary<float>(out, 0.5F, bigEndian);
        appendBinary<std::uint8_t>(out, 2, bigEndian);
        appendBinary<std::int32_t>(out, -7, bigEndian);
        appendBinary<std::int32_t>(out, 7, bigEndian);
    }
    for (const auto& q : kCubeQuads) {
        appendBinary<std::uint16_t>(out, 0xABCD, bigEndian);
        appendBinary<std::uint8_t>(out, 4, bigEndian);
        for (const int i : q)
            appendBinary<std::uint32_t>(out, static_cast<std::uint32_t>(i), bigEndian);
        appendBinary<float>(out, 1.0F, bigEndian);
    }
    for (int e = 0; e < 2; ++e) {
        appendBinary<std::int32_t>(out, 0, bigEndian);
        appendBinary<std::int32_t>(out, 1, bigEndian);
    }
    return out;
}

void checkPlyCube(const io::ImportResult& r, double size)
{
    CHECK(r.format == io::FileFormat::Ply);
    CHECK(r.mesh.vertexCount() == 8);
    CHECK(r.mesh.triangleCount() == 12);
    CHECK(r.mesh.isValid());
    CHECK(r.mesh.surfaceArea() == Approx(6.0 * size * size));
    CHECK(r.mesh.bounds().min.y == Approx(0.0));
    CHECK(r.mesh.bounds().max.x == Approx(size));
    CHECK(r.mesh.bounds().max.z == Approx(size));

    // Faces keep their outward orientation: signed volume is positive.
    double signedVolume = 0.0;
    const auto& v = r.mesh.vertices();
    for (const auto& t : r.mesh.triangles())
        signedVolume += v[t[0]].dot(v[t[1]].cross(v[t[2]])) / 6.0;
    CHECK(signedVolume == Approx(size * size * size));
}

// --- OCCT writers ----------------------------------------------------------------

const TopoDS_Shape& occtShape(const geometry::Shape& shape)
{
    REQUIRE(shape.impl() != nullptr);
    return shape.impl()->shape;
}

void writeStep(const geometry::Shape& shape, const fs::path& path, const char* unit)
{
    STEPControl_Writer writer;
    const std::string previous = Interface_Static::CVal("write.step.unit");
    Interface_Static::SetCVal("write.step.unit", unit);
    REQUIRE(writer.Transfer(occtShape(shape), STEPControl_AsIs) == IFSelect_RetDone);
    const IFSelect_ReturnStatus status = writer.Write(core::pathToUtf8(path).c_str());
    Interface_Static::SetCVal("write.step.unit", previous.c_str());
    REQUIRE(status == IFSelect_RetDone);
}

void checkBox(const io::ImportResult& r, double dx, double dy, double dz)
{
    REQUIRE(r.shape.has_value());
    CHECK(r.shape->solidCount() == 1);
    CHECK(r.shape->faceCount() == 6);
    CHECK(r.shape->volume() == Approx(dx * dy * dz).epsilon(1e-6));
    const core::BoundingBox b = r.mesh.bounds();
    CHECK(b.min.x == Approx(0.0).margin(1e-4));
    CHECK(b.max.x == Approx(dx).epsilon(1e-6));
    CHECK(b.max.y == Approx(dy).epsilon(1e-6));
    CHECK(b.max.z == Approx(dz).epsilon(1e-6));
    CHECK(r.mesh.isValid());
    CHECK(r.mesh.triangleCount() >= 12);
}

} // namespace

TEST_CASE("Format detection from extension", "[io]")
{
    CHECK(io::formatFromExtension("a/b/part.STL") == io::FileFormat::Stl);
    CHECK(io::formatFromExtension("part.obj") == io::FileFormat::Obj);
    CHECK(io::formatFromExtension("scan.PLY") == io::FileFormat::Ply);
    CHECK(io::formatFromExtension("part.3Mf") == io::FileFormat::ThreeMf);
    CHECK(io::formatFromExtension("part.step") == io::FileFormat::Step);
    CHECK(io::formatFromExtension("part.STP") == io::FileFormat::Step);
    CHECK(io::formatFromExtension("part.iges") == io::FileFormat::Iges);
    CHECK(io::formatFromExtension("part.igs") == io::FileFormat::Iges);
    CHECK(io::formatFromExtension("part.brep") == io::FileFormat::Brep);
    CHECK(io::formatFromExtension("part.brp") == io::FileFormat::Brep);
    CHECK(io::formatFromExtension("part.txt") == io::FileFormat::Unknown);
    CHECK(io::formatFromExtension("noextension") == io::FileFormat::Unknown);

    CHECK(io::formatName(io::FileFormat::Stl) == "STL");
    CHECK(io::formatName(io::FileFormat::ThreeMf) == "3MF");
    CHECK(io::formatName(io::FileFormat::Ply) == "PLY");
    CHECK(io::formatName(io::FileFormat::Unknown) == "Unknown");

    const auto exts = io::supportedImportExtensions();
    CHECK(exts.size() == 10);
    for (const auto& e : exts)
        CHECK(io::formatFromExtension("x." + e) != io::FileFormat::Unknown);
}

TEST_CASE("ASCII STL cube is welded", "[io][stl]")
{
    TempDir dir;
    const fs::path path = dir.file("cube_ascii.stl");
    writeFile(path, asciiStl(cubeTriangles(10.0F)));

    const io::ImportResult r = io::importFile(path);
    CHECK(r.format == io::FileFormat::Stl);
    CHECK(r.mesh.triangleCount() == 12);
    CHECK(r.mesh.vertexCount() == 8);
    CHECK(r.mesh.surfaceArea() == Approx(600.0));
    CHECK(r.mesh.bounds().max.x == Approx(10.0));
    CHECK_FALSE(r.shape.has_value());
}

TEST_CASE("Binary STL, including a header that starts with 'solid'", "[io][stl]")
{
    TempDir dir;
    const auto tris = cubeTriangles(2.5F);

    SECTION("plain header")
    {
        const fs::path path = dir.file("cube_bin.stl");
        writeFile(path, binaryStl(tris, "binary stl exported by test"));
        const io::ImportResult r = io::importFile(path);
        CHECK(r.mesh.triangleCount() == 12);
        CHECK(r.mesh.vertexCount() == 8);
        CHECK(r.mesh.bounds().max.z == Approx(2.5));
        CHECK(r.warnings.empty());
    }
    SECTION("header starting with solid")
    {
        const fs::path path = dir.file("cube_solid_header.stl");
        writeFile(path, binaryStl(tris, "solid exported by a misbehaving tool"));
        const io::ImportResult r = io::importFile(path);
        CHECK(r.mesh.triangleCount() == 12);
        CHECK(r.mesh.vertexCount() == 8);
    }
    SECTION("truncated binary file is read partially with a warning")
    {
        const fs::path path = dir.file("cube_trunc.stl");
        std::string data = binaryStl(tris, "truncated");
        data.resize(data.size() - 50 * 4 - 10);
        writeFile(path, data);
        const io::ImportResult r = io::importFile(path);
        CHECK(r.mesh.triangleCount() == 7);
        CHECK_FALSE(r.warnings.empty());
    }
}

TEST_CASE("OBJ with quads, negative indices and v/vt/vn corners", "[io][obj]")
{
    TempDir dir;
    const fs::path path = dir.file("cube.obj");
    writeFile(path, R"(# cube
mtllib cube.mtl
o Cube
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
v 0 0 1
v 1 0 1
v 1 1 1
v 0 1 1 1.0
vt 0 0
vt 1 0
vn 0 0 1
usemtl Default
s off
f 1 4 3 2
f 5/1 6/2 7/1 8/2
f 1//1 2//1 6//1 5//1
f 2/1/1 3/1/1 7/1/1 6/1/1
f -6 -5 -1 -2
f -5 -8 -4 \
  -1
f 1 2 99
)");
    const io::ImportResult r = io::importFile(path);
    CHECK(r.format == io::FileFormat::Obj);
    CHECK(r.mesh.triangleCount() == 12);
    CHECK(r.mesh.vertexCount() == 8);
    CHECK(r.mesh.surfaceArea() == Approx(6.0));
    REQUIRE(r.warnings.size() == 1); // the out-of-range face
    CHECK_THAT(r.warnings.front(), Catch::Matchers::ContainsSubstring("Line 24"));
}

TEST_CASE("3MF with components, transforms and centimeter unit", "[io][3mf]")
{
    TempDir dir;
    // Object 1: unit cube. Object 2: two components, one scaled by 2 and translated by
    // (5,0,0), with a mirror on the second one. Build item translates everything by z=1.
    const std::string model = std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<m:model unit="centimeter" xml:lang="en-US"
    xmlns:m="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
    xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06">
  <m:resources>)") + cubeObject3mf("1", "m:") +
                              R"(
    <m:object id="2" type="model">
      <m:components>
        <m:component objectid="1" transform="2 0 0 0 2 0 0 0 2 5 0 0"/>
        <m:component objectid="1" transform="-1 0 0 0 1 0 0 0 1 0 0 0"/>
        <m:component objectid="1" p:path="/3D/other.model"/>
      </m:components>
    </m:object>
  </m:resources>
  <m:build>
    <m:item objectid="2" transform="1 0 0 0 1 0 0 0 1 0 0 1"/>
  </m:build>
</m:model>)";
    const io::ImportResult r = io::importFile(write3mf(dir, "parts.3mf", model));
    CHECK(r.format == io::FileFormat::ThreeMf);
    CHECK(r.mesh.triangleCount() == 24);
    CHECK(r.mesh.vertexCount() == 16);
    const core::BoundingBox b = r.mesh.bounds();
    // In mm: mirrored cube spans x=-10..0, scaled cube x=50..70; z=10..30.
    CHECK(b.min.x == Approx(-10.0));
    CHECK(b.max.x == Approx(70.0));
    CHECK(b.min.z == Approx(10.0));
    CHECK(b.max.z == Approx(30.0));
    CHECK(r.mesh.surfaceArea() == Approx(600.0 + 2400.0));
    REQUIRE(r.warnings.size() == 1);
    CHECK_THAT(r.warnings.front(), Catch::Matchers::ContainsSubstring("production"));

    // Winding stays outward for the mirrored copy: signed volume must be positive.
    double signedVolume = 0.0;
    const auto& v = r.mesh.vertices();
    for (const auto& t : r.mesh.triangles())
        signedVolume += v[t[0]].dot(v[t[1]].cross(v[t[2]])) / 6.0;
    CHECK(signedVolume == Approx(1000.0 + 8000.0));
}

TEST_CASE("3MF fallbacks: default model path, no build items, cycles", "[io][3mf]")
{
    TempDir dir;
    SECTION("no rels and no build items")
    {
        const std::string model = R"(<?xml version="1.0"?>
<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
<resources>)" + cubeObject3mf("7", "") +
                                  "</resources></model>";
        const io::ImportResult r =
            io::importFile(write3mf(dir, "nobuild.3mf", model, nullptr, "3D/3dmodel.model"));
        CHECK(r.mesh.triangleCount() == 12);
        CHECK(r.mesh.bounds().max.x == Approx(1.0));
        CHECK_FALSE(r.warnings.empty());
    }
    SECTION("cyclic components are skipped")
    {
        const std::string model = R"(<?xml version="1.0"?>
<model unit="inch" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
<resources>)" + cubeObject3mf("1", "") +
                                  R"(<object id="2"><components><component objectid="1"/><component objectid="3"/></components></object>
<object id="3"><components><component objectid="2"/></components></object>
</resources><build><item objectid="2"/></build></model>)";
        const io::ImportResult r = io::importFile(write3mf(dir, "cycle.3mf", model));
        CHECK(r.mesh.triangleCount() == 12);
        CHECK(r.mesh.bounds().max.y == Approx(25.4));
        CHECK_FALSE(r.warnings.empty());
    }
}

TEST_CASE("STEP import of a box, in mm and from a file written in meters", "[io][step]")
{
    TempDir dir;
    const geometry::Shape box = geometry::makeBox(10.0, 20.0, 30.0);

    const fs::path mm = dir.file("box_mm.step");
    writeStep(box, mm, "MM");
    const io::ImportResult r = io::importFile(mm);
    CHECK(r.format == io::FileFormat::Step);
    checkBox(r, 10.0, 20.0, 30.0);

    // Same coordinates, but the file declares metres: the reader must scale to mm.
    // (Patch the unit entity directly; OCCT writers differ between versions in
    // whether write.step.unit rescales coordinates or only relabels them.)
    std::string text;
    {
        std::ifstream in(mm, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string milli = "SI_UNIT(.MILLI.,.METRE.)";
    const std::size_t at = text.find(milli);
    REQUIRE(at != std::string::npos);
    text.replace(at, milli.size(), "SI_UNIT($,.METRE.)");
    const fs::path m = dir.file("box_m.stp");
    writeFile(m, text);
    const io::ImportResult rm = io::importFile(m);
    checkBox(rm, 10000.0, 20000.0, 30000.0);
}

TEST_CASE("BREP and IGES import of a box", "[io][brep][iges]")
{
    TempDir dir;
    const geometry::Shape box = geometry::makeBox(4.0, 5.0, 6.0);

    const fs::path brep = dir.file("box.brep");
    REQUIRE(BRepTools::Write(occtShape(box), core::pathToUtf8(brep).c_str()));
    const io::ImportResult rb = io::importFile(brep);
    CHECK(rb.format == io::FileFormat::Brep);
    checkBox(rb, 4.0, 5.0, 6.0);

    const fs::path iges = dir.file("box.igs");
    IGESControl_Controller::Init();
    IGESControl_Writer writer("MM", 1); // mode 1: B-rep solids
    REQUIRE(writer.AddShape(occtShape(box)));
    writer.ComputeModel();
    REQUIRE(writer.Write(core::pathToUtf8(iges).c_str()));
    const io::ImportResult ri = io::importFile(iges);
    CHECK(ri.format == io::FileFormat::Iges);
    REQUIRE(ri.shape.has_value());
    CHECK(ri.shape->faceCount() == 6);
    const core::BoundingBox b = ri.mesh.bounds();
    CHECK(b.max.x == Approx(4.0).epsilon(1e-6));
    CHECK(b.max.z == Approx(6.0).epsilon(1e-6));
}

TEST_CASE("Non-ASCII file names", "[io]")
{
    TempDir dir;
    const fs::path stl = dir.file(core::pathFromUtf8("sze\xC5\x9B\xC4\x87_\xE6\xA8\xA1\xE5\x9E\x8B.stl"));
    writeFile(stl, asciiStl(cubeTriangles(1.0F)));
    CHECK(io::importFile(stl).mesh.triangleCount() == 12);

    const fs::path step = dir.file(core::pathFromUtf8("pude\xC5\x82ko.step"));
    writeStep(geometry::makeBox(1.0, 2.0, 3.0), step, "MM");
    checkBox(io::importFile(step), 1.0, 2.0, 3.0);
}

TEST_CASE("Import errors", "[io][errors]")
{
    TempDir dir;
    CHECK_THROWS_AS(io::importFile(dir.file("missing.stl")), io::ImportError);

    const fs::path unknown = dir.file("model.xyz");
    writeFile(unknown, "whatever");
    CHECK_THROWS_AS(io::importFile(unknown), io::ImportError);

    const std::string garbage = "this is not a 3D file at all\n\x01\x02\x03";
    for (const char* name : {"g.stl", "g.obj", "g.ply", "g.3mf", "g.step", "g.igs", "g.brep"}) {
        INFO(name);
        const fs::path path = dir.file(name);
        writeFile(path, garbage);
        CHECK_THROWS_AS(io::importFile(path), io::ImportError);
    }

    const fs::path empty = dir.file("empty.stl");
    writeFile(empty, "");
    CHECK_THROWS_AS(io::importFile(empty), io::ImportError);

    const fs::path bad3mf = dir.file("bad.3mf");
    writeFile(bad3mf, makeZip({{"3D/3dmodel.model", "<model><resources><object"}}));
    CHECK_THROWS_AS(io::importFile(bad3mf), io::ImportError);
}

TEST_CASE("ASCII PLY with quads, triangles and extra properties", "[io][ply]")
{
    TempDir dir;
    std::string ply = "ply\r\nformat ascii 1.0\r\ncomment made by hand\r\n"
                      "element vertex 9\r\nproperty float x\r\nproperty float y\r\nproperty float z\r\n"
                      "property uchar red\r\nproperty uchar green\r\nproperty uchar blue\r\n"
                      "element face 8\r\nproperty list uchar int vertex_index\r\nproperty int material\r\n"
                      "element material 1\r\nproperty list uchar char name\r\n"
                      "end_header\r\n";
    for (const auto& v : kCubeVertices)
        ply +=
            std::to_string(v[0] * 3) + " " + std::to_string(v[1] * 3) + " " + std::to_string(v[2] * 3) + " 255 0 0\n";
    ply += "nan 1 2 0 0 0\n"; // vertex 8 is invalid
    for (int f = 0; f < 6; ++f) {
        const auto& q = kCubeQuads[f];
        if (f == 1) {
            ply += "3 4 5 6 0\n3 4 6 7 0\n"; // the top as two triangles
            continue;
        }
        ply += "4 " + std::to_string(q[0]) + " " + std::to_string(q[1]) + " " + std::to_string(q[2]) + " " +
               std::to_string(q[3]) + " 0\n";
    }
    ply += "3 0 1 8 0\n"; // uses the invalid vertex
    ply += "2 65 66\n";   // material element
    const fs::path path = dir.file("cube_ascii.ply");
    writeFile(path, ply);

    const io::ImportResult r = io::importFile(path);
    checkPlyCube(r, 3.0);
    CHECK_FALSE(r.shape.has_value());
    REQUIRE(r.warnings.size() == 1);
    CHECK_THAT(r.warnings[0], Catch::Matchers::ContainsSubstring("invalid vertex coordinates"));
}

TEST_CASE("Binary PLY, little and big endian", "[io][ply]")
{
    TempDir dir;
    for (const bool bigEndian : {false, true}) {
        INFO("big endian: " << bigEndian);
        const fs::path path = dir.file(bigEndian ? "cube_be.ply" : "cube_le.ply");
        writeFile(path, binaryPlyCube(bigEndian));
        const io::ImportResult r = io::readPly(path);
        checkPlyCube(r, 2.0);
        CHECK(r.warnings.empty());
    }
}

TEST_CASE("PLY invalid faces are dropped with warnings", "[io][ply]")
{
    TempDir dir;
    const std::string ply = "ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\n"
                            "property float z\nelement face 5\nproperty list uchar uint vertex_indices\n"
                            "end_header\n0 0 0\n1 0 0\n0 1 0\n9 9 9\n"
                            "3 0 1 2\n"    // ok
                            "3 0 1 7\n"    // out of range
                            "3 0 0 1\n"    // repeated index
                            "2 0 1\n"      // too small
                            "4 0 1 2 2\n"; // one good triangle, one degenerate
    const fs::path path = dir.file("broken.ply");
    writeFile(path, ply);
    const io::ImportResult r = io::importFile(path);
    CHECK(r.mesh.triangleCount() == 2);
    CHECK(r.mesh.vertexCount() == 3); // the unreferenced vertex is removed
    CHECK(r.mesh.isValid());
    CHECK(r.mesh.bounds().max.x == Approx(1.0));
    CHECK(r.warnings.size() == 3);
}

TEST_CASE("PLY errors", "[io][ply][errors]")
{
    TempDir dir;
    const auto expectError = [&dir](const std::string& content, const std::string& message) {
        INFO(content);
        const fs::path path = dir.file("bad.ply");
        writeFile(path, content);
        CHECK_THROWS_AS(io::importFile(path), io::ImportError);
        CHECK_THROWS_WITH(io::importFile(path), Catch::Matchers::ContainsSubstring(message));
    };
    const std::string xyz = "element vertex 2\nproperty float x\nproperty float y\nproperty float z\n";
    expectError("ply\nformat ascii 1.0\n" + xyz + "end_header\n0 0 0\n1 1 1\n",
                "PLY file has no faces (point clouds are not supported)");
    expectError("ply\nformat binary_little_endian 1.0\n" + xyz +
                    "element face 0\nproperty list uchar int vertex_indices\nend_header\n",
                "point clouds are not supported");
    expectError("solid not a ply\n", "Not a PLY file");
    expectError("ply\nformat binary_middle_endian 1.0\nend_header\n", "Unsupported PLY format");
    expectError("ply\nformat ascii 1.0\nelement vertex 1\nproperty quad x\nend_header\n", "Unknown PLY property type");
    expectError("ply\nformat ascii 1.0\n" + xyz + "element face 1\nproperty list uchar int vertex_indices\n",
                "end_header");
    expectError("ply\nformat binary_little_endian 1.0\n" + xyz +
                    "element face 1\nproperty list uchar int vertex_indices\nend_header\nabc",
                "truncated");
    expectError("ply\nformat ascii 1.0\n" + xyz + "element face 1\nproperty list uchar int vertex_indices\n" +
                    "end_header\n0 0 0\n1 0 0\n3 0 1\n",
                "truncated");
    expectError("ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nelement face 1\n"
                "property list uchar int vertex_indices\nend_header\n0 0\n1 0\n0 1\n3 0 1 2\n",
                "x, y and z");
}

TEST_CASE("Import progress is monotonic and reaches 1", "[io][progress]")
{
    TempDir dir;
    std::vector<Tri> tris; // a grid big enough for many progress steps
    for (int i = 0; i < 100; ++i) {
        for (int j = 0; j < 100; ++j) {
            const auto x = static_cast<float>(i);
            const auto y = static_cast<float>(j);
            tris.push_back({{{x, y, 0}, {x + 1, y, 0}, {x + 1, y + 1, 0}}});
            tris.push_back({{{x, y, 0}, {x + 1, y + 1, 0}, {x, y + 1, 0}}});
        }
    }
    const fs::path stlBinary = dir.file("grid.stl");
    writeFile(stlBinary, binaryStl(tris, "grid"));
    const fs::path stlAscii = dir.file("grid_ascii.stl");
    writeFile(stlAscii, asciiStl(tris));
    const fs::path ply = dir.file("cube.ply");
    writeFile(ply, binaryPlyCube(false));
    std::string obj;
    for (const auto& v : kCubeVertices)
        obj += "v " + std::to_string(v[0]) + " " + std::to_string(v[1]) + " " + std::to_string(v[2]) + "\n";
    for (const auto& q : kCubeQuads) {
        obj += "f " + std::to_string(q[0] + 1) + " " + std::to_string(q[1] + 1) + " " + std::to_string(q[2] + 1) + " " +
               std::to_string(q[3] + 1) + "\n";
    }
    const fs::path objPath = dir.file("cube.obj");
    writeFile(objPath, obj);
    const std::string model = R"(<?xml version="1.0"?>
<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
<resources>)" + cubeObject3mf("1", "") +
                              "</resources><build><item objectid=\"1\"/></build></model>";
    const fs::path threeMf = core::pathFromUtf8(write3mf(dir, "cube.3mf", model));

    for (const fs::path& path : {stlBinary, stlAscii, ply, objPath, threeMf}) {
        INFO(core::pathToUtf8(path));
        std::vector<double> reported;
        io::ImportOptions options;
        options.progress = [&reported](double fraction) {
            reported.push_back(fraction);
            return true;
        };
        const io::ImportResult r = io::importFile(path, options);
        CHECK_FALSE(r.mesh.isEmpty());
        REQUIRE(reported.size() >= 2);
        CHECK(reported.front() == 0.0);
        CHECK(reported.back() == 1.0);
        CHECK(reported.size() <= 103); // throttled to about every 1%
        for (std::size_t i = 1; i < reported.size(); ++i)
            CHECK(reported[i] >= reported[i - 1]);
        if (path == stlBinary || path == stlAscii)
            CHECK(reported.size() > 50); // intermediate steps for big inputs
    }
}

TEST_CASE("Import can be cancelled from the progress callback", "[io][progress]")
{
    TempDir dir;
    std::vector<Tri> tris;
    for (int i = 0; i < 2000; ++i) {
        const auto x = static_cast<float>(i);
        tris.push_back({{{x, 0, 0}, {x + 1, 0, 0}, {x, 1, 0}}});
    }
    const fs::path stl = dir.file("strip.stl");
    writeFile(stl, binaryStl(tris, "strip"));
    const fs::path ply = dir.file("cube.ply");
    writeFile(ply, binaryPlyCube(true));
    const fs::path step = dir.file("box.step");
    writeStep(geometry::makeBox(1.0, 2.0, 3.0), step, "MM");

    for (const fs::path& path : {stl, ply, step}) {
        INFO(core::pathToUtf8(path));
        io::ImportOptions options;
        options.progress = [](double) { return false; };
        CHECK_THROWS_AS(io::importFile(path, options), io::ImportCancelled);
        CHECK_THROWS_WITH(io::importFile(path, options), "Import cancelled");
    }

    // Cancel in the middle of a big binary STL.
    double last = -1.0;
    io::ImportOptions options;
    options.progress = [&last](double fraction) {
        last = fraction;
        return fraction < 0.3;
    };
    CHECK_THROWS_AS(io::importFile(stl, options), io::ImportCancelled);
    CHECK(last >= 0.3);
    CHECK(last < 1.0);

    // ImportCancelled is an ImportError, so existing handlers keep working.
    CHECK_THROWS_AS(io::readStl(stl, options), io::ImportError);
}
