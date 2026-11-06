// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/project/ProjectFile.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace aerocad::project {

namespace {

constexpr std::string_view kMagic = "AEROMESH";
constexpr std::uint32_t kMeshVersion = 1;
constexpr std::size_t kHeaderSize = 8 + 4 + 8 + 8;

template <typename T> void put(std::string& out, T value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    unsigned char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    if constexpr (std::endian::native == std::endian::big) {
        for (std::size_t i = 0; i < sizeof(T) / 2; ++i)
            std::swap(bytes[i], bytes[sizeof(T) - 1 - i]);
    }
    out.append(reinterpret_cast<const char*>(bytes), sizeof(T));
}

template <typename T> T get(std::string_view data, std::size_t& pos)
{
    unsigned char bytes[sizeof(T)];
    std::memcpy(bytes, data.data() + pos, sizeof(T));
    pos += sizeof(T);
    if constexpr (std::endian::native == std::endian::big) {
        for (std::size_t i = 0; i < sizeof(T) / 2; ++i)
            std::swap(bytes[i], bytes[sizeof(T) - 1 - i]);
    }
    T value;
    std::memcpy(&value, bytes, sizeof(T));
    return value;
}

} // namespace

std::string encodeMesh(const core::TriangleMesh& mesh)
{
    std::string out;
    out.reserve(kHeaderSize + mesh.vertexCount() * 24 + mesh.triangleCount() * 12);
    out.append(kMagic);
    put<std::uint32_t>(out, kMeshVersion);
    put<std::uint64_t>(out, mesh.vertexCount());
    put<std::uint64_t>(out, mesh.triangleCount());
    for (const auto& v : mesh.vertices()) {
        put(out, v.x);
        put(out, v.y);
        put(out, v.z);
    }
    for (const auto& t : mesh.triangles()) {
        put(out, t[0]);
        put(out, t[1]);
        put(out, t[2]);
    }
    return out;
}

core::TriangleMesh decodeMesh(std::string_view data)
{
    if (data.size() < kHeaderSize || data.substr(0, kMagic.size()) != kMagic)
        throw ProjectError("Not an AeroCAD mesh");
    std::size_t pos = kMagic.size();
    const auto version = get<std::uint32_t>(data, pos);
    if (version != kMeshVersion)
        throw ProjectError("Unsupported mesh version " + std::to_string(version));
    const auto vertexCount = get<std::uint64_t>(data, pos);
    const auto triangleCount = get<std::uint64_t>(data, pos);
    const std::uint64_t available = data.size() - kHeaderSize;
    // Overflow-safe size check before allocating.
    if (vertexCount > available / 24 || triangleCount > available / 12 ||
        vertexCount * 24 + triangleCount * 12 != available || vertexCount > std::numeric_limits<std::uint32_t>::max())
        throw ProjectError("Truncated or corrupt mesh data");
    std::vector<core::Vec3> vertices;
    vertices.reserve(static_cast<std::size_t>(vertexCount));
    for (std::uint64_t i = 0; i < vertexCount; ++i) {
        core::Vec3 v;
        v.x = get<double>(data, pos);
        v.y = get<double>(data, pos);
        v.z = get<double>(data, pos);
        vertices.push_back(v);
    }
    std::vector<core::TriangleMesh::Triangle> triangles;
    triangles.reserve(static_cast<std::size_t>(triangleCount));
    for (std::uint64_t i = 0; i < triangleCount; ++i) {
        core::TriangleMesh::Triangle t;
        t[0] = get<std::uint32_t>(data, pos);
        t[1] = get<std::uint32_t>(data, pos);
        t[2] = get<std::uint32_t>(data, pos);
        triangles.push_back(t);
    }
    for (const auto& t : triangles) {
        if (t[0] >= vertexCount || t[1] >= vertexCount || t[2] >= vertexCount)
            throw ProjectError("Mesh has out-of-range vertex indices");
    }
    return core::TriangleMesh(std::move(vertices), std::move(triangles));
}

std::string encodeIndices(const std::vector<std::uint32_t>& indices)
{
    std::string out;
    out.reserve(indices.size() * 4);
    for (const auto i : indices)
        put(out, i);
    return out;
}

std::vector<std::uint32_t> decodeIndices(std::string_view data)
{
    if (data.size() % 4 != 0)
        throw ProjectError("Truncated index data");
    std::vector<std::uint32_t> out;
    out.reserve(data.size() / 4);
    std::size_t pos = 0;
    while (pos < data.size())
        out.push_back(get<std::uint32_t>(data, pos));
    return out;
}

} // namespace aerocad::project
