// SPDX-License-Identifier: GPL-3.0-or-later
// Wavefront OBJ reader: geometry only ("v" and "f" statements). OBJ carries no unit;
// millimetres are assumed.
#include "aerocad/io/Import.hpp"

#include "ReaderUtil.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace aerocad::io {

namespace {

/// Parses the vertex part of a face corner ("v", "v/vt", "v//vn", "v/vt/vn") and
/// resolves it to a 0-based index. Returns false when invalid or out of range.
bool resolveCorner(std::string_view corner, std::size_t vertexCount, std::size_t& index)
{
    const std::size_t slash = corner.find('/');
    const std::string_view v = corner.substr(0, slash);
    long long raw = 0;
    if (!detail::parseInt(v, raw) || raw == 0)
        return false;
    const auto count = static_cast<long long>(vertexCount);
    const long long resolved = raw > 0 ? raw - 1 : count + raw; // negative: relative to the end
    if (resolved < 0 || resolved >= count)
        return false;
    index = static_cast<std::size_t>(resolved);
    return true;
}

} // namespace

ImportResult readObj(const std::filesystem::path& path)
{
    const std::string data = detail::readFileBytes(path);
    const std::string name = detail::pathToUtf8(path);

    ImportResult result;
    result.format = FileFormat::Obj;

    std::vector<core::Vec3> positions;
    std::vector<std::size_t> face;
    core::MeshBuilder builder;
    {
        detail::WarningSink warnings(result.warnings);
        std::size_t lineNo = 0;
        std::size_t pos = 0;
        std::string logical; // accumulates lines joined with a trailing backslash

        while (pos < data.size()) {
            std::size_t end = data.find('\n', pos);
            if (end == std::string::npos)
                end = data.size();
            std::string_view line(data.data() + pos, end - pos);
            pos = end + 1;
            ++lineNo;

            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (!line.empty() && line.back() == '\\') {
                logical.append(line.substr(0, line.size() - 1));
                logical.push_back(' ');
                continue;
            }
            if (!logical.empty()) {
                logical.append(line);
                line = logical;
            }

            const std::size_t hash = line.find('#');
            if (hash != std::string_view::npos)
                line = line.substr(0, hash);

            std::string_view rest = line;
            const std::string_view keyword = detail::nextToken(rest);
            if (keyword == "v") {
                double c[3] = {0.0, 0.0, 0.0};
                bool ok = true;
                for (double& value : c)
                    ok = detail::parseDouble(detail::nextToken(rest), value) && ok;
                if (!ok || !std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2])) {
                    warnings.add("Line " + std::to_string(lineNo) + ": invalid vertex, using origin");
                    c[0] = c[1] = c[2] = 0.0;
                }
                // Keep the vertex even when invalid so that later indices stay correct.
                positions.push_back({c[0], c[1], c[2]});
            } else if (keyword == "f" || keyword == "fo") {
                face.clear();
                bool ok = true;
                for (std::string_view corner = detail::nextToken(rest); !corner.empty();
                     corner = detail::nextToken(rest)) {
                    std::size_t index = 0;
                    if (!resolveCorner(corner, positions.size(), index)) {
                        ok = false;
                        break;
                    }
                    face.push_back(index);
                }
                if (!ok) {
                    warnings.add("Line " + std::to_string(lineNo) + ": invalid face index, face skipped");
                } else if (face.size() < 3) {
                    warnings.add("Line " + std::to_string(lineNo) + ": face with fewer than 3 vertices skipped");
                } else {
                    // Fan triangulation (exact for convex polygons, which is the common case).
                    for (std::size_t i = 1; i + 1 < face.size(); ++i)
                        builder.addTriangle(positions[face[0]], positions[face[i]], positions[face[i + 1]]);
                }
            }
            // Everything else (vt, vn, g, o, s, usemtl, mtllib, l, p, ...) is ignored.
            logical.clear();
        }
    }

    result.mesh = builder.take();
    if (result.mesh.isEmpty())
        throw ImportError("OBJ file contains no faces: " + name);
    return result;
}

} // namespace aerocad::io
