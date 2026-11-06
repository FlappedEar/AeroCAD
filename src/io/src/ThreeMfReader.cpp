// SPDX-License-Identifier: GPL-3.0-or-later
// 3MF reader (core specification: mesh objects, components, build items, units).
#include "aerocad/io/Import.hpp"

#include "ReaderUtil.hpp"

#include <miniz.h>
#include <pugixml.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aerocad::io {

namespace {

constexpr const char* kModelRelType = "http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel";
constexpr const char* kDefaultModelPath = "3D/3dmodel.model";
constexpr int kMaxComponentDepth = 64;

/// Affine transform in 3MF convention: p' = [x y z 1] * M, M stored row-major as 4x3.
struct Affine {
    std::array<double, 12> m{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

    [[nodiscard]] core::Vec3 apply(const core::Vec3& p) const
    {
        return {p.x * m[0] + p.y * m[3] + p.z * m[6] + m[9], p.x * m[1] + p.y * m[4] + p.z * m[7] + m[10],
                p.x * m[2] + p.y * m[5] + p.z * m[8] + m[11]};
    }

    /// Transform that applies `this` first and then `next`.
    [[nodiscard]] Affine then(const Affine& next) const
    {
        Affine r;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 3; ++col) {
                double v = row == 3 ? next.m[static_cast<std::size_t>(9 + col)] : 0.0;
                for (int k = 0; k < 3; ++k)
                    v += m[static_cast<std::size_t>(row * 3 + k)] * next.m[static_cast<std::size_t>(k * 3 + col)];
                r.m[static_cast<std::size_t>(row * 3 + col)] = v;
            }
        }
        return r;
    }

    [[nodiscard]] double determinant() const
    {
        return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
               m[2] * (m[3] * m[7] - m[4] * m[6]);
    }
};

struct MeshObject {
    std::vector<core::Vec3> vertices;
    std::vector<std::array<std::size_t, 3>> triangles;
};

struct Component {
    std::string objectId;
    Affine transform;
};

struct Object {
    std::unique_ptr<MeshObject> mesh;
    std::vector<Component> components;
};

std::string_view localName(std::string_view qualified)
{
    const std::size_t colon = qualified.find(':');
    return colon == std::string_view::npos ? qualified : qualified.substr(colon + 1);
}

pugi::xml_node child(const pugi::xml_node& parent, std::string_view name)
{
    for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling()) {
        if (c.type() == pugi::node_element && localName(c.name()) == name)
            return c;
    }
    return {};
}

template <typename F>
void forEachChild(const pugi::xml_node& parent, std::string_view name, F&& f)
{
    for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling()) {
        if (c.type() == pugi::node_element && localName(c.name()) == name)
            f(c);
    }
}

/// Attribute lookup by local name (namespace prefixes ignored). Unprefixed names are
/// preferred so that "p:path" does not shadow a core attribute of the same local name.
pugi::xml_attribute attribute(const pugi::xml_node& node, std::string_view name)
{
    pugi::xml_attribute prefixed;
    for (pugi::xml_attribute a = node.first_attribute(); a; a = a.next_attribute()) {
        const std::string_view qualified = a.name();
        if (qualified == name)
            return a;
        if (!prefixed && localName(qualified) == name)
            prefixed = a;
    }
    return prefixed;
}

/// True for a namespaced attribute with the given local name (e.g. production "p:path").
bool hasPrefixedAttribute(const pugi::xml_node& node, std::string_view name)
{
    for (pugi::xml_attribute a = node.first_attribute(); a; a = a.next_attribute()) {
        const std::string_view qualified = a.name();
        if (qualified != name && localName(qualified) == name && !std::string_view(a.value()).empty())
            return true;
    }
    return false;
}

bool parseTransform(std::string_view text, Affine& out)
{
    Affine t;
    for (double& v : t.m) {
        if (!detail::parseDouble(detail::nextToken(text), v) || !std::isfinite(v))
            return false;
    }
    if (!detail::nextToken(text).empty())
        return false;
    out = t;
    return true;
}

double unitScale(std::string_view unit, detail::WarningSink& warnings)
{
    if (unit.empty() || unit == "millimeter")
        return 1.0;
    if (unit == "micron")
        return 0.001;
    if (unit == "centimeter")
        return 10.0;
    if (unit == "inch")
        return 25.4;
    if (unit == "foot")
        return 304.8;
    if (unit == "meter")
        return 1000.0;
    warnings.add("Unknown 3MF unit '" + std::string(unit) + "', assuming millimeters");
    return 1.0;
}

class ZipArchive {
public:
    explicit ZipArchive(const std::string& data)
    {
        std::memset(&m_zip, 0, sizeof(m_zip));
        m_open = mz_zip_reader_init_mem(&m_zip, data.data(), data.size(), 0) != 0;
    }
    ~ZipArchive()
    {
        if (m_open)
            mz_zip_reader_end(&m_zip);
    }
    ZipArchive(const ZipArchive&) = delete;
    ZipArchive& operator=(const ZipArchive&) = delete;

    [[nodiscard]] bool isOpen() const { return m_open; }

    /// Extracts an entry (case-insensitive name). Returns false if not present.
    bool extract(const std::string& entry, std::string& out)
    {
        const int index = mz_zip_reader_locate_file(&m_zip, entry.c_str(), nullptr, 0);
        if (index < 0)
            return false;
        std::size_t size = 0;
        void* buffer = mz_zip_reader_extract_to_heap(&m_zip, static_cast<mz_uint>(index), &size, 0);
        if (!buffer)
            throw ImportError("Corrupt 3MF archive entry: " + entry);
        out.assign(static_cast<const char*>(buffer), size);
        mz_free(buffer);
        return true;
    }

private:
    mz_zip_archive m_zip{};
    bool m_open = false;
};

std::string normalizePartName(std::string_view target)
{
    while (!target.empty() && (target.front() == '/' || target.front() == '\\'))
        target.remove_prefix(1);
    return std::string(target);
}

std::string findRootModel(ZipArchive& zip)
{
    std::string rels;
    if (zip.extract("_rels/.rels", rels)) {
        pugi::xml_document doc;
        if (doc.load_buffer(rels.data(), rels.size())) {
            const pugi::xml_node root = child(doc, "Relationships");
            std::string found;
            forEachChild(root, "Relationship", [&](const pugi::xml_node& rel) {
                if (found.empty() && std::string_view(attribute(rel, "Type").value()) == kModelRelType)
                    found = normalizePartName(attribute(rel, "Target").value());
            });
            if (!found.empty())
                return found;
        }
    }
    return kDefaultModelPath;
}

class ModelBuilder {
public:
    ModelBuilder(std::unordered_map<std::string, Object>& objects, detail::WarningSink& warnings, double scale)
        : m_objects(objects), m_warnings(warnings), m_scale(scale)
    {
    }

    void emit(const std::string& id, const Affine& transform, int depth = 0)
    {
        const auto it = m_objects.find(id);
        if (it == m_objects.end()) {
            m_warnings.add("Reference to unknown 3MF object id " + id + " skipped");
            return;
        }
        if (depth > kMaxComponentDepth || m_active.count(id) != 0) {
            m_warnings.add("Cyclic 3MF component reference at object " + id + " skipped");
            return;
        }
        m_active.insert(id);
        const Object& object = it->second;
        if (object.mesh)
            emitMesh(*object.mesh, transform);
        for (const Component& c : object.components)
            emit(c.objectId, c.transform.then(transform), depth + 1);
        m_active.erase(id);
    }

    core::MeshBuilder& builder() { return m_builder; }

private:
    void emitMesh(const MeshObject& mesh, const Affine& transform)
    {
        // Mirroring transforms flip the winding; swap two corners to keep normals outward.
        const bool mirrored = transform.determinant() < 0.0;
        std::vector<core::Vec3> world;
        world.reserve(mesh.vertices.size());
        for (const core::Vec3& v : mesh.vertices)
            world.push_back(transform.apply(v) * m_scale);
        for (const auto& t : mesh.triangles) {
            if (mirrored)
                m_builder.addTriangle(world[t[0]], world[t[2]], world[t[1]]);
            else
                m_builder.addTriangle(world[t[0]], world[t[1]], world[t[2]]);
        }
    }

    std::unordered_map<std::string, Object>& m_objects;
    detail::WarningSink& m_warnings;
    double m_scale;
    std::unordered_set<std::string> m_active;
    core::MeshBuilder m_builder;
};

std::unique_ptr<MeshObject> parseMesh(const pugi::xml_node& meshNode, const std::string& id,
                                      detail::WarningSink& warnings)
{
    auto mesh = std::make_unique<MeshObject>();
    const pugi::xml_node vertices = child(meshNode, "vertices");
    forEachChild(vertices, "vertex", [&](const pugi::xml_node& v) {
        double c[3] = {0.0, 0.0, 0.0};
        const char* names[3] = {"x", "y", "z"};
        bool ok = true;
        for (int k = 0; k < 3; ++k)
            ok = detail::parseDouble(detail::trim(attribute(v, names[k]).value()), c[k]) && std::isfinite(c[k]) && ok;
        if (!ok) {
            warnings.add("Object " + id + ": invalid vertex, using origin");
            c[0] = c[1] = c[2] = 0.0;
        }
        mesh->vertices.push_back({c[0], c[1], c[2]});
    });

    const pugi::xml_node triangles = child(meshNode, "triangles");
    const auto count = static_cast<long long>(mesh->vertices.size());
    std::size_t bad = 0;
    forEachChild(triangles, "triangle", [&](const pugi::xml_node& t) {
        std::array<std::size_t, 3> idx{};
        const char* names[3] = {"v1", "v2", "v3"};
        for (std::size_t k = 0; k < 3; ++k) {
            long long value = 0;
            if (!detail::parseInt(detail::trim(attribute(t, names[k]).value()), value) || value < 0 ||
                value >= count) {
                ++bad;
                return;
            }
            idx[k] = static_cast<std::size_t>(value);
        }
        mesh->triangles.push_back(idx);
    });
    if (bad > 0)
        warnings.add("Object " + id + ": " + std::to_string(bad) + " triangle(s) with invalid indices skipped");
    return mesh;
}

} // namespace

ImportResult read3mf(const std::filesystem::path& path)
{
    const std::string name = detail::pathToUtf8(path);
    // Read into memory: miniz's file API takes a narrow path, which is not portable
    // for non-ASCII names on Windows.
    const std::string data = detail::readFileBytes(path);

    ZipArchive zip(data);
    if (!zip.isOpen())
        throw ImportError("Not a valid 3MF file (cannot open ZIP container): " + name);

    const std::string modelPath = findRootModel(zip);
    std::string modelXml;
    if (!zip.extract(modelPath, modelXml))
        throw ImportError("3MF model part '" + modelPath + "' not found in " + name);

    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(modelXml.data(), modelXml.size());
    if (!parsed)
        throw ImportError("Invalid 3MF model XML in " + name + ": " + parsed.description());
    const pugi::xml_node model = child(doc, "model");
    if (!model)
        throw ImportError("3MF model part has no <model> element: " + name);

    ImportResult result;
    result.format = FileFormat::ThreeMf;
    {
        detail::WarningSink warnings(result.warnings);
        const double scale = unitScale(attribute(model, "unit").value(), warnings);

        std::unordered_map<std::string, Object> objects;
        std::vector<std::string> objectOrder;
        forEachChild(child(model, "resources"), "object", [&](const pugi::xml_node& o) {
            const std::string id(detail::trim(attribute(o, "id").value()));
            if (id.empty()) {
                warnings.add("3MF object without id skipped");
                return;
            }
            Object object;
            if (const pugi::xml_node meshNode = child(o, "mesh"))
                object.mesh = parseMesh(meshNode, id, warnings);
            forEachChild(child(o, "components"), "component", [&](const pugi::xml_node& c) {
                if (hasPrefixedAttribute(c, "path")) {
                    warnings.add("Object " + id +
                                 ": component referencing another model part (production extension) skipped");
                    return;
                }
                Component component;
                component.objectId = std::string(detail::trim(attribute(c, "objectid").value()));
                const pugi::xml_attribute tr = attribute(c, "transform");
                if (tr && !parseTransform(tr.value(), component.transform))
                    warnings.add("Object " + id + ": invalid component transform, using identity");
                object.components.push_back(std::move(component));
            });
            if (objects.count(id) != 0)
                warnings.add("Duplicate 3MF object id " + id + ", last definition used");
            else
                objectOrder.push_back(id);
            objects[id] = std::move(object);
        });

        ModelBuilder builder(objects, warnings, scale);
        bool anyItem = false;
        forEachChild(child(model, "build"), "item", [&](const pugi::xml_node& item) {
            anyItem = true;
            if (hasPrefixedAttribute(item, "path")) {
                warnings.add("Build item referencing another model part (production extension) skipped");
                return;
            }
            Affine transform;
            const pugi::xml_attribute tr = attribute(item, "transform");
            if (tr && !parseTransform(tr.value(), transform))
                warnings.add("Invalid build item transform, using identity");
            builder.emit(std::string(detail::trim(attribute(item, "objectid").value())), transform);
        });
        if (!anyItem) {
            warnings.add("3MF file has no build items; importing all mesh objects");
            for (const std::string& id : objectOrder) {
                if (objects[id].mesh)
                    builder.emit(id, Affine{});
            }
        }
        result.mesh = builder.builder().take();
    }

    if (result.mesh.isEmpty())
        throw ImportError("3MF file contains no triangles: " + name);
    return result;
}

} // namespace aerocad::io
