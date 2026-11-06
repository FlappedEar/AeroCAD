// SPDX-License-Identifier: GPL-3.0-or-later
// STL reader (binary and ASCII). STL carries no unit; millimetres are assumed.
#include "aerocad/io/Import.hpp"

#include "ReaderUtil.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace aerocad::io {

namespace {

constexpr std::size_t kHeaderSize = 80;
constexpr std::size_t kBinaryPrefix = kHeaderSize + 4;
constexpr std::size_t kFacetSize = 50;

std::uint32_t readU32le(const unsigned char* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U);
}

float readF32le(const unsigned char* p)
{
    const std::uint32_t bits = readU32le(p);
    float value = 0.0F;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool isFinite(const core::Vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool startsWithSolid(const std::string& data)
{
    std::string_view view(data);
    view = detail::trim(view.substr(0, std::min<std::size_t>(view.size(), 512)));
    return view.size() >= 5 && detail::iequals(view.substr(0, 5), "solid");
}

void parseBinary(const std::string& data, std::size_t count, ImportResult& result)
{
    detail::WarningSink warnings(result.warnings);
    core::MeshBuilder builder;
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t skipped = 0;
    for (std::size_t i = 0; i < count; ++i) {
        // Facet layout: normal (3 floats), 3 vertices (9 floats), attribute byte count (u16).
        const unsigned char* facet = bytes + kBinaryPrefix + i * kFacetSize + 12;
        core::Vec3 v[3];
        for (int k = 0; k < 3; ++k) {
            const unsigned char* p = facet + 12 * k;
            v[k] = {static_cast<double>(readF32le(p)), static_cast<double>(readF32le(p + 4)),
                    static_cast<double>(readF32le(p + 8))};
        }
        if (!isFinite(v[0]) || !isFinite(v[1]) || !isFinite(v[2])) {
            ++skipped;
            continue;
        }
        builder.addTriangle(v[0], v[1], v[2]);
    }
    if (skipped > 0)
        warnings.add("Skipped " + std::to_string(skipped) + " facet(s) with invalid coordinates");
    result.mesh = builder.take();
}

void parseAscii(const std::string& data, ImportResult& result)
{
    // Tolerant parser: every "vertex x y z" statement contributes one corner and every
    // three corners form a triangle. Other keywords (facet normal, outer loop, ...) are
    // structural only and are skipped, which also accepts files with several solids.
    detail::WarningSink warnings(result.warnings);
    core::MeshBuilder builder;
    std::string_view rest(data);
    core::Vec3 corners[3];
    int cornerCount = 0;
    std::size_t badVertices = 0;
    bool sawFacet = false;
    bool facetBad = false;

    while (true) {
        const std::string_view token = detail::nextToken(rest);
        if (token.empty())
            break;
        if (detail::iequals(token, "facet")) {
            sawFacet = true;
            if (cornerCount != 0) {
                warnings.add("Facet with " + std::to_string(cornerCount) + " vertices ignored");
                cornerCount = 0;
            }
            facetBad = false;
            continue;
        }
        if (!detail::iequals(token, "vertex"))
            continue;

        double c[3] = {0.0, 0.0, 0.0};
        bool ok = true;
        for (double& value : c)
            ok = detail::parseDouble(detail::nextToken(rest), value) && ok;
        const core::Vec3 p{c[0], c[1], c[2]};
        if (!ok || !isFinite(p)) {
            ++badVertices;
            facetBad = true;
        }
        corners[cornerCount++] = p;
        if (cornerCount == 3) {
            if (!facetBad)
                builder.addTriangle(corners[0], corners[1], corners[2]);
            cornerCount = 0;
            facetBad = false;
        }
    }

    if (badVertices > 0)
        warnings.add("Skipped facet(s) with " + std::to_string(badVertices) + " unparsable vertex line(s)");
    if (!sawFacet && builder.triangleCount() == 0)
        throw ImportError("Not a valid STL file (no binary header match and no ASCII facets)");
    result.mesh = builder.take();
}

} // namespace

ImportResult readStl(const std::filesystem::path& path)
{
    const std::string data = detail::readFileBytes(path);
    const std::string name = detail::pathToUtf8(path);

    ImportResult result;
    result.format = FileFormat::Stl;

    bool binary = false;
    std::size_t count = 0;
    if (data.size() >= kBinaryPrefix) {
        count = readU32le(reinterpret_cast<const unsigned char*>(data.data()) + kHeaderSize);
        const std::size_t available = (data.size() - kBinaryPrefix) / kFacetSize;
        if (data.size() == kBinaryPrefix + count * kFacetSize) {
            // Exact size match: binary, even when the header starts with "solid".
            binary = count > 0 || !startsWithSolid(data);
        } else if (!startsWithSolid(data)) {
            binary = true;
            if (count > available) {
                result.warnings.push_back("Binary STL is truncated: header declares " + std::to_string(count) +
                                          " facets, file holds " + std::to_string(available));
                count = available;
            } else {
                result.warnings.push_back("Binary STL has trailing data after the declared facets");
            }
        }
    }

    try {
        if (binary)
            parseBinary(data, count, result);
        else
            parseAscii(data, result);
    } catch (const ImportError& e) {
        throw ImportError(name + ": " + e.what());
    }

    if (result.mesh.isEmpty())
        throw ImportError("STL file contains no triangles: " + name);
    return result;
}

} // namespace aerocad::io
