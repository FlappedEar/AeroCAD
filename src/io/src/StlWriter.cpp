// SPDX-License-Identifier: GPL-3.0-or-later
// Binary STL writer.
#include "aerocad/io/Export.hpp"

#include "WriterUtil.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace aerocad::io {

namespace {

void appendU32(std::string& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out += static_cast<char>((v >> (8 * i)) & 0xFFu);
}

void appendFloat(std::string& out, double value)
{
    const auto f = static_cast<float>(value);
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(f));
    std::memcpy(&bits, &f, sizeof(bits));
    appendU32(out, bits); // little endian, independent of the host byte order
}

void appendVec(std::string& out, const core::Vec3& v)
{
    appendFloat(out, v.x);
    appendFloat(out, v.y);
    appendFloat(out, v.z);
}

} // namespace

void writeStl(const std::filesystem::path& path, const std::vector<ExportItem>& items)
{
    detail::requireMeshes(items, "STL");

    // Triangles collapsed to an edge or a point carry no surface; slicers report them as
    // errors, so they are not written.
    const auto collapsed = [](const core::TriangleMesh& mesh, std::size_t t) {
        const auto& tri = mesh.triangles()[t];
        const auto& v = mesh.vertices();
        return v[tri[0]] == v[tri[1]] || v[tri[1]] == v[tri[2]] || v[tri[0]] == v[tri[2]];
    };
    std::size_t total = 0;
    for (const auto& item : items) {
        for (std::size_t t = 0; t < item.mesh->triangleCount(); ++t)
            total += collapsed(*item.mesh, t) ? 0 : 1;
    }
    if (total == 0)
        throw ExportError("Nothing to export: every triangle is degenerate");
    if (total > std::numeric_limits<std::uint32_t>::max())
        throw ExportError("Too many triangles for STL");

    std::string data;
    data.reserve(84 + total * 50);
    std::string header = "AeroCAD binary STL, units: mm";
    header.resize(80, ' ');
    data += header;
    appendU32(data, static_cast<std::uint32_t>(total));

    for (const auto& item : items) {
        const core::TriangleMesh& mesh = *item.mesh;
        const auto& verts = mesh.vertices();
        const auto& tris = mesh.triangles();
        for (std::size_t t = 0; t < tris.size(); ++t) {
            if (collapsed(mesh, t))
                continue;
            const core::Vec3 n = mesh.faceNormal(t);
            appendVec(data, n);
            for (const std::uint32_t idx : tris[t]) {
                const core::Vec3& v = verts[idx];
                if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
                    throw ExportError("'" + item.name + "' has a non-finite vertex");
                appendVec(data, v);
            }
            data += '\0';
            data += '\0';
        }
    }
    detail::writeFileAtomic(path, data);
}

} // namespace aerocad::io
