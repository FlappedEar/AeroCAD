// SPDX-License-Identifier: GPL-3.0-or-later
// PLY (Stanford polygon file) reader: ASCII, binary little endian and binary big endian.
// Only vertex positions and face vertex lists are used; every other property and element
// (normals, colours, confidence, edges, materials, ...) is skipped. PLY carries no unit;
// millimetres are assumed.
#include "aerocad/io/Import.hpp"

#include "ReaderUtil.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace aerocad::io {

namespace {

enum class Encoding { Ascii, BinaryLittleEndian, BinaryBigEndian };

enum class ScalarType { Int8, UInt8, Int16, UInt16, Int32, UInt32, Float32, Float64 };

bool scalarTypeFromName(std::string_view name, ScalarType& out)
{
    static constexpr std::pair<std::string_view, ScalarType> kTypes[] = {
        {"char", ScalarType::Int8},      {"int8", ScalarType::Int8},       {"uchar", ScalarType::UInt8},
        {"uint8", ScalarType::UInt8},    {"short", ScalarType::Int16},     {"int16", ScalarType::Int16},
        {"ushort", ScalarType::UInt16},  {"uint16", ScalarType::UInt16},   {"int", ScalarType::Int32},
        {"int32", ScalarType::Int32},    {"uint", ScalarType::UInt32},     {"uint32", ScalarType::UInt32},
        {"float", ScalarType::Float32},  {"float32", ScalarType::Float32}, {"double", ScalarType::Float64},
        {"float64", ScalarType::Float64}};
    for (const auto& [typeName, type] : kTypes) {
        if (name == typeName) {
            out = type;
            return true;
        }
    }
    return false;
}

constexpr std::size_t scalarSize(ScalarType type)
{
    switch (type) {
    case ScalarType::Int8:
    case ScalarType::UInt8:
        return 1;
    case ScalarType::Int16:
    case ScalarType::UInt16:
        return 2;
    case ScalarType::Int32:
    case ScalarType::UInt32:
    case ScalarType::Float32:
        return 4;
    case ScalarType::Float64:
        return 8;
    }
    return 0;
}

struct Property {
    std::string name;
    ScalarType type = ScalarType::Float32;
    bool isList = false;
    ScalarType countType = ScalarType::UInt8; ///< list properties only
};

struct Element {
    std::string name;
    std::size_t count = 0;
    std::vector<Property> properties;
};

struct Header {
    Encoding encoding = Encoding::Ascii;
    std::vector<Element> elements;
    std::size_t dataOffset = 0; ///< first byte after "end_header" and its line break
};

Header parseHeader(const std::string& data)
{
    Header header;
    std::size_t pos = 0;
    bool first = true;
    bool sawFormat = false;
    while (true) {
        if (pos >= data.size())
            throw ImportError("PLY header has no end_header line");
        std::size_t end = data.find('\n', pos);
        if (end == std::string::npos)
            end = data.size();
        std::string_view line(data.data() + pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        std::string_view rest = line;
        const std::string_view keyword = detail::nextToken(rest);
        if (first) {
            if (keyword != "ply")
                throw ImportError("Not a PLY file (missing 'ply' magic line)");
            first = false;
            continue;
        }
        if (keyword.empty() || keyword == "comment" || keyword == "obj_info")
            continue;
        if (keyword == "end_header") {
            header.dataOffset = std::min(pos, data.size());
            break;
        }
        if (keyword == "format") {
            const std::string_view encoding = detail::nextToken(rest);
            if (encoding == "ascii")
                header.encoding = Encoding::Ascii;
            else if (encoding == "binary_little_endian")
                header.encoding = Encoding::BinaryLittleEndian;
            else if (encoding == "binary_big_endian")
                header.encoding = Encoding::BinaryBigEndian;
            else
                throw ImportError("Unsupported PLY format '" + std::string(encoding) + "'");
            sawFormat = true;
        } else if (keyword == "element") {
            Element element;
            element.name = std::string(detail::nextToken(rest));
            long long count = 0;
            if (element.name.empty() || !detail::parseInt(detail::nextToken(rest), count) || count < 0)
                throw ImportError("Invalid PLY element declaration: " + std::string(line));
            element.count = static_cast<std::size_t>(count);
            header.elements.push_back(std::move(element));
        } else if (keyword == "property") {
            if (header.elements.empty())
                throw ImportError("PLY property declared before any element");
            Property property;
            std::string_view type = detail::nextToken(rest);
            if (type == "list") {
                property.isList = true;
                if (!scalarTypeFromName(detail::nextToken(rest), property.countType))
                    throw ImportError("Invalid PLY list property: " + std::string(line));
                type = detail::nextToken(rest);
            }
            if (!scalarTypeFromName(type, property.type))
                throw ImportError("Unknown PLY property type in: " + std::string(line));
            property.name = std::string(detail::nextToken(rest));
            header.elements.back().properties.push_back(std::move(property));
        } else {
            throw ImportError("Unexpected PLY header line: " + std::string(line));
        }
    }
    if (!sawFormat)
        throw ImportError("PLY header has no format line");
    return header;
}

/// Sequential reader of one encoded scalar after another.
class DataReader {
public:
    DataReader(const std::string& data, std::size_t offset, Encoding encoding)
        : m_data(data), m_pos(offset), m_encoding(encoding)
    {
    }

    [[nodiscard]] std::size_t position() const { return m_pos; }
    [[nodiscard]] std::size_t size() const { return m_data.size(); }

    double readDouble(ScalarType type)
    {
        if (m_encoding == Encoding::Ascii)
            return readAsciiNumber();
        return readBinary(type);
    }

    /// Reads an integral value (list counts and indices). Returns false for values
    /// that are not whole numbers.
    bool readInteger(ScalarType type, long long& out)
    {
        const double value = readDouble(type);
        if (!std::isfinite(value) || value != std::floor(value) || std::abs(value) > 9.0e15)
            return false;
        out = static_cast<long long>(value);
        return true;
    }

    void skip(ScalarType type)
    {
        if (m_encoding == Encoding::Ascii) {
            readAsciiNumber();
            return;
        }
        const std::size_t n = scalarSize(type);
        if (m_data.size() - m_pos < n)
            throw ImportError("PLY file is truncated");
        m_pos += n;
    }

    /// Skips `count` binary scalars (or ASCII tokens) at once.
    void skip(ScalarType type, std::size_t count)
    {
        if (m_encoding == Encoding::Ascii) {
            for (std::size_t i = 0; i < count; ++i)
                readAsciiNumber();
            return;
        }
        const std::size_t n = scalarSize(type);
        if ((m_data.size() - m_pos) / n < count)
            throw ImportError("PLY file is truncated");
        m_pos += n * count;
    }

private:
    double readAsciiNumber()
    {
        std::size_t i = m_pos;
        while (i < m_data.size() && detail::isSpace(m_data[i]))
            ++i;
        std::size_t j = i;
        while (j < m_data.size() && !detail::isSpace(m_data[j]))
            ++j;
        if (i == j)
            throw ImportError("PLY file is truncated");
        m_pos = j;
        double value = 0.0;
        if (!detail::parseDouble(std::string_view(m_data.data() + i, j - i), value)) {
            // "nan", "inf" and friends are accepted by parseDouble; anything else is garbage.
            throw ImportError("Invalid number '" + m_data.substr(i, std::min<std::size_t>(j - i, 32)) +
                              "' in PLY data");
        }
        return value;
    }

    double readBinary(ScalarType type)
    {
        const std::size_t n = scalarSize(type);
        if (m_data.size() - m_pos < n)
            throw ImportError("PLY file is truncated");
        unsigned char b[8];
        std::memcpy(b, m_data.data() + m_pos, n);
        m_pos += n;
        if (m_encoding == Encoding::BinaryBigEndian) {
            for (std::size_t k = 0; k < n / 2; ++k)
                std::swap(b[k], b[n - 1 - k]);
        }
        // b now holds the little-endian representation.
        std::uint64_t u = 0;
        for (std::size_t k = 0; k < n; ++k)
            u |= static_cast<std::uint64_t>(b[k]) << (8U * k);
        switch (type) {
        case ScalarType::Int8:
            return static_cast<double>(static_cast<std::int8_t>(static_cast<std::uint8_t>(u)));
        case ScalarType::UInt8:
            return static_cast<double>(static_cast<std::uint8_t>(u));
        case ScalarType::Int16:
            return static_cast<double>(static_cast<std::int16_t>(static_cast<std::uint16_t>(u)));
        case ScalarType::UInt16:
            return static_cast<double>(static_cast<std::uint16_t>(u));
        case ScalarType::Int32:
            return static_cast<double>(static_cast<std::int32_t>(static_cast<std::uint32_t>(u)));
        case ScalarType::UInt32:
            return static_cast<double>(static_cast<std::uint32_t>(u));
        case ScalarType::Float32: {
            const auto bits = static_cast<std::uint32_t>(u);
            float f = 0.0F;
            static_assert(sizeof(f) == sizeof(bits));
            std::memcpy(&f, &bits, sizeof(f));
            return static_cast<double>(f);
        }
        case ScalarType::Float64: {
            double d = 0.0;
            static_assert(sizeof(d) == sizeof(u));
            std::memcpy(&d, &u, sizeof(d));
            return d;
        }
        }
        return 0.0;
    }

    const std::string& m_data;
    std::size_t m_pos;
    Encoding m_encoding;
};

void skipProperty(DataReader& in, const Property& property)
{
    if (!property.isList) {
        in.skip(property.type);
        return;
    }
    long long count = 0;
    if (!in.readInteger(property.countType, count) || count < 0)
        throw ImportError("Invalid PLY list length");
    in.skip(property.type, static_cast<std::size_t>(count));
}

int findProperty(const Element& element, std::string_view name)
{
    for (std::size_t i = 0; i < element.properties.size(); ++i) {
        if (element.properties[i].name == name)
            return static_cast<int>(i);
    }
    return -1;
}

bool isFinite(const core::Vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

void readVertices(DataReader& in, const Element& element, std::vector<core::Vec3>& vertices,
                  detail::ProgressReporter& progress)
{
    const int ix = findProperty(element, "x");
    const int iy = findProperty(element, "y");
    const int iz = findProperty(element, "z");
    if (ix < 0 || iy < 0 || iz < 0)
        throw ImportError("PLY vertex element has no x, y and z properties");
    for (const int i : {ix, iy, iz}) {
        if (element.properties[static_cast<std::size_t>(i)].isList)
            throw ImportError("PLY vertex coordinates must be scalar properties");
    }

    vertices.resize(element.count);
    for (std::size_t v = 0; v < element.count; ++v) {
        progress.update(in.position(), in.size());
        double c[3] = {0.0, 0.0, 0.0};
        for (std::size_t p = 0; p < element.properties.size(); ++p) {
            const Property& property = element.properties[p];
            const auto index = static_cast<int>(p);
            if (index == ix)
                c[0] = in.readDouble(property.type);
            else if (index == iy)
                c[1] = in.readDouble(property.type);
            else if (index == iz)
                c[2] = in.readDouble(property.type);
            else
                skipProperty(in, property);
        }
        vertices[v] = {c[0], c[1], c[2]};
    }
}

struct FaceStats {
    std::size_t outOfRange = 0;
    std::size_t degenerate = 0;
    std::size_t tooSmall = 0;
};

void readFaces(DataReader& in, const Element& element, std::size_t vertexCount,
               std::vector<core::TriangleMesh::Triangle>& triangles, FaceStats& stats,
               detail::ProgressReporter& progress)
{
    int list = findProperty(element, "vertex_indices");
    if (list < 0)
        list = findProperty(element, "vertex_index");
    if (list < 0 || !element.properties[static_cast<std::size_t>(list)].isList)
        throw ImportError("PLY face element has no vertex_indices list property");

    triangles.reserve(triangles.size() + element.count);
    std::vector<std::uint32_t> polygon;
    for (std::size_t f = 0; f < element.count; ++f) {
        progress.update(in.position(), in.size());
        for (std::size_t p = 0; p < element.properties.size(); ++p) {
            const Property& property = element.properties[p];
            if (static_cast<int>(p) != list) {
                skipProperty(in, property);
                continue;
            }
            long long count = 0;
            if (!in.readInteger(property.countType, count) || count < 0)
                throw ImportError("Invalid PLY face vertex count");
            polygon.clear();
            bool valid = true;
            for (long long k = 0; k < count; ++k) {
                long long index = 0;
                if (!in.readInteger(property.type, index) || index < 0 ||
                    static_cast<unsigned long long>(index) >= vertexCount) {
                    valid = false;
                    continue; // keep reading so that the stream stays in sync
                }
                polygon.push_back(static_cast<std::uint32_t>(index));
            }
            if (!valid) {
                ++stats.outOfRange;
                continue;
            }
            if (polygon.size() < 3) {
                ++stats.tooSmall;
                continue;
            }
            // Fan triangulation (exact for convex polygons, which is the common case).
            for (std::size_t k = 1; k + 1 < polygon.size(); ++k) {
                const std::uint32_t a = polygon[0];
                const std::uint32_t b = polygon[k];
                const std::uint32_t c = polygon[k + 1];
                if (a == b || b == c || a == c) {
                    ++stats.degenerate;
                    continue;
                }
                triangles.push_back({a, b, c});
            }
        }
    }
}

/// Drops triangles that use non-finite vertices and removes unreferenced vertices, so
/// that stray points or broken coordinates do not affect the bounds.
core::TriangleMesh buildMesh(std::vector<core::Vec3> vertices, std::vector<core::TriangleMesh::Triangle> triangles,
                             detail::WarningSink& warnings)
{
    std::vector<bool> finite(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i)
        finite[i] = isFinite(vertices[i]);

    std::size_t nonFinite = 0;
    std::size_t kept = 0;
    for (const auto& t : triangles) {
        if (!finite[t[0]] || !finite[t[1]] || !finite[t[2]]) {
            ++nonFinite;
            continue;
        }
        triangles[kept++] = t;
    }
    triangles.resize(kept);
    if (nonFinite > 0)
        warnings.add("Skipped " + std::to_string(nonFinite) + " triangle(s) with invalid vertex coordinates");

    constexpr std::uint32_t kUnused = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> remap(vertices.size(), kUnused);
    for (const auto& t : triangles) {
        for (const std::uint32_t i : t)
            remap[i] = 0;
    }
    std::uint32_t next = 0;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if (remap[i] == kUnused)
            continue;
        remap[i] = next;
        vertices[next++] = vertices[i];
    }
    if (next != vertices.size()) {
        vertices.resize(next);
        for (auto& t : triangles)
            t = {remap[t[0]], remap[t[1]], remap[t[2]]};
    }
    return core::TriangleMesh(std::move(vertices), std::move(triangles));
}

} // namespace

ImportResult readPly(const std::filesystem::path& path, const ImportOptions& options)
{
    detail::ProgressReporter progress(options);
    const std::string data = detail::readFileBytes(path);
    const std::string name = detail::pathToUtf8(path);

    ImportResult result;
    result.format = FileFormat::Ply;

    try {
        const Header header = parseHeader(data);

        const Element* vertexElement = nullptr;
        bool hasFaces = false;
        for (const Element& element : header.elements) {
            if (element.name == "vertex" && !vertexElement)
                vertexElement = &element;
            else if (element.name == "face" && element.count > 0)
                hasFaces = true;
        }
        if (!vertexElement || vertexElement->count == 0)
            throw ImportError("PLY file has no vertices");
        if (!hasFaces)
            throw ImportError("PLY file has no faces (point clouds are not supported)");
        if (vertexElement->count > std::numeric_limits<std::uint32_t>::max())
            throw ImportError("PLY file has too many vertices");

        std::vector<core::Vec3> vertices;
        std::vector<core::TriangleMesh::Triangle> triangles;
        FaceStats stats;
        DataReader in(data, header.dataOffset, header.encoding);
        bool facesRead = false;
        for (const Element& element : header.elements) {
            if (&element == vertexElement) {
                readVertices(in, element, vertices, progress);
            } else if (element.name == "face" && !facesRead) {
                // Indices are checked against the declared vertex count, so faces may
                // also precede the vertices.
                readFaces(in, element, vertexElement->count, triangles, stats, progress);
                facesRead = true;
            } else {
                // Unknown element (edge, material, ...) or a repeated one: skip by size.
                for (std::size_t i = 0; i < element.count; ++i) {
                    progress.update(in.position(), in.size());
                    for (const Property& property : element.properties)
                        skipProperty(in, property);
                }
            }
        }
        detail::WarningSink warnings(result.warnings);
        if (stats.outOfRange > 0)
            warnings.add("Skipped " + std::to_string(stats.outOfRange) + " face(s) with invalid vertex indices");
        if (stats.tooSmall > 0)
            warnings.add("Skipped " + std::to_string(stats.tooSmall) + " face(s) with fewer than 3 vertices");
        if (stats.degenerate > 0)
            warnings.add("Dropped " + std::to_string(stats.degenerate) +
                         " degenerate triangle(s) with repeated vertex indices");
        result.mesh = buildMesh(std::move(vertices), std::move(triangles), warnings);
    } catch (const ImportCancelled&) {
        throw;
    } catch (const ImportError& e) {
        throw ImportError(name + ": " + e.what());
    }

    if (result.mesh.isEmpty())
        throw ImportError("PLY file contains no valid triangles: " + name);
    progress.finish();
    return result;
}

} // namespace aerocad::io
