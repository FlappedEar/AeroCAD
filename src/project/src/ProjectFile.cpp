// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/project/ProjectFile.hpp"

#include "aerocad/core/Units.hpp"
#include "aerocad/geometry/Shape.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <miniz.h>

#include <chrono>
#include <ctime>
#include <memory>
#include <set>
#include <utility>

namespace aerocad::project {

namespace {

constexpr const char* kManifest = "manifest.json";

std::string kindName(model::PartKind kind)
{
    switch (kind) {
    case model::PartKind::Imported:
        return "imported";
    case model::PartKind::Body:
        return "body";
    case model::PartKind::Sketch:
        return "sketch";
    case model::PartKind::Plane:
        return "plane";
    case model::PartKind::Axis:
        return "axis";
    }
    return "imported";
}

model::PartKind kindFromName(const std::string& name)
{
    if (name == "body")
        return model::PartKind::Body;
    if (name == "sketch")
        return model::PartKind::Sketch;
    if (name == "plane")
        return model::PartKind::Plane;
    if (name == "axis")
        return model::PartKind::Axis;
    if (name == "imported")
        return model::PartKind::Imported;
    throw ProjectError("Unknown part kind '" + name + "'");
}

/// A part's saved Appearance colour; anything that is not "#rrggbb" is ignored (default).
std::string savedColor(const Json& j)
{
    const auto it = j.find("color");
    if (it == j.end() || !it->is_string())
        return {};
    std::string c = it->get<std::string>();
    return model::isPartColor(c) ? c : std::string{};
}

std::string utcNow()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

class ZipWriter {
public:
    ZipWriter()
    {
        if (mz_zip_writer_init_heap(&m_zip, 0, 0) == 0)
            throw ProjectError("Cannot create zip archive");
    }
    ~ZipWriter() { mz_zip_writer_end(&m_zip); }
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    void add(const std::string& name, std::string_view data, mz_uint level)
    {
        if (mz_zip_writer_add_mem(&m_zip, name.c_str(), data.data(), data.size(), level) == 0)
            throw ProjectError("Cannot add " + name + " to the project archive");
    }

    std::string finish()
    {
        void* buffer = nullptr;
        size_t size = 0;
        if (mz_zip_writer_finalize_heap_archive(&m_zip, &buffer, &size) == 0)
            throw ProjectError("Cannot finalise the project archive");
        std::string out(static_cast<const char*>(buffer), size);
        mz_free(buffer);
        return out;
    }

private:
    mz_zip_archive m_zip{};
};

class ZipReader {
public:
    explicit ZipReader(std::string_view bytes)
    {
        if (mz_zip_reader_init_mem(&m_zip, bytes.data(), bytes.size(), 0) == 0)
            throw ProjectError("Not an AeroCAD project (not a zip archive)");
    }
    ~ZipReader() { mz_zip_reader_end(&m_zip); }
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    [[nodiscard]] bool contains(const std::string& name)
    {
        return mz_zip_reader_locate_file(&m_zip, name.c_str(), nullptr, 0) >= 0;
    }

    std::string read(const std::string& name)
    {
        const int index = mz_zip_reader_locate_file(&m_zip, name.c_str(), nullptr, 0);
        if (index < 0)
            throw ProjectError("Project archive has no entry " + name);
        size_t size = 0;
        void* buffer = mz_zip_reader_extract_to_heap(&m_zip, static_cast<mz_uint>(index), &size, 0);
        if (buffer == nullptr)
            throw ProjectError("Cannot read " + name + " (corrupt archive)");
        std::string out(static_cast<const char*>(buffer), size);
        mz_free(buffer);
        return out;
    }

private:
    mz_zip_archive m_zip{};
};

template <typename T> T field(const Json& j, const char* key, const std::string& what)
{
    auto it = j.find(key);
    if (it == j.end())
        throw ProjectError("Missing '" + std::string(key) + "' in " + what);
    try {
        return it->get<T>();
    } catch (const Json::exception&) {
        throw ProjectError("Invalid '" + std::string(key) + "' in " + what);
    }
}

} // namespace

std::string encodeProject(const ProjectData& data)
{
    ZipWriter zip;
    Json parts = Json::array();
    std::vector<std::pair<std::string, std::string>> blobs;
    for (const auto& p : data.parts) {
        if (p.kind != model::PartKind::Imported)
            throw ProjectError("Only imported parts are stored; '" + p.name + "' comes from the timeline");
        Json j{{"id", p.id}, {"name", p.name}, {"source", p.source}, {"visible", p.visible}, {"kind", "imported"}};
        if (!p.color.empty())
            j["color"] = p.color;
        if (!p.importUnits.empty())
            j["importUnits"] = p.importUnits;
        if (p.mesh && !p.mesh->isEmpty()) {
            const std::string entry = "meshes/" + std::to_string(p.id) + ".amesh";
            blobs.emplace_back(entry, encodeMesh(*p.mesh));
            j["mesh"] = entry;
        }
        if (p.selection && !p.selection->empty()) {
            const std::string entry = "selections/" + std::to_string(p.id) + ".u32";
            blobs.emplace_back(entry, encodeIndices(*p.selection));
            j["selection"] = entry;
        }
        if (p.shape && !p.shape->isNull()) {
            const std::string entry = "shapes/" + std::to_string(p.id) + ".brep";
            blobs.emplace_back(entry, geometry::toBrep(*p.shape));
            j["brep"] = entry;
        }
        parts.push_back(std::move(j));
    }

    Json timelineParts = Json::array();
    for (const auto& t : data.timelineParts) {
        Json j{{"feature", t.feature}, {"id", t.id}, {"name", t.name}, {"visible", t.visible}};
        if (!t.color.empty())
            j["color"] = t.color;
        if (t.kind)
            j["kind"] = kindName(*t.kind);
        if (t.selection && !t.selection->empty()) {
            const std::string entry = "selections/timeline-" + std::to_string(t.id) + ".u32";
            blobs.emplace_back(entry, encodeIndices(*t.selection));
            j["selection"] = entry;
        }
        timelineParts.push_back(std::move(j));
    }

    Json results = Json::array();
    for (const auto& r : data.timelineResults) {
        Json bodies = Json::array();
        for (std::size_t k = 0; k < r.output.bodies.size(); ++k) {
            const auto& [shape, name] = r.output.bodies[k];
            if (shape.isNull())
                continue;
            const std::string entry = "results/" + std::to_string(r.feature) + "-" + std::to_string(k) + ".brep";
            blobs.emplace_back(entry, geometry::toBrep(shape));
            Json body{{"name", name}, {"brep", entry}};
            if (k < r.output.bodySlots.size())
                body["slot"] = r.output.bodySlots[k];
            bodies.push_back(std::move(body));
        }
        results.push_back({{"feature", r.feature}, {"key", r.key}, {"report", r.output.report}, {"bodies", bodies}});
    }

    Json variants = Json::array();
    for (const auto& v : data.variants.variants()) {
        Json j{{"id", v.id}, {"name", v.name}};
        if (v.id != data.variants.active())
            j["design"] = cad::stateToJson(v.state);
        variants.push_back(std::move(j));
    }

    Json manifest{
        {"format", kFormatName},
        {"version", kFormatVersion},
        {"generator", data.generator},
        {"savedAt", utcNow()},
        {"units", "mm"},
        {"upAxis", "Z"},
        {"parts", parts},
        {"timelineParts", timelineParts},
        {"timelineResults", results},
        {"design", cad::stateToJson(data.design)},
        {"variants", {{"active", data.variants.active()}, {"previous", data.variants.previous()}, {"items", variants}}},
        {"view", data.view.is_object() ? data.view : Json::object()},
    };
    if (!data.originalPath.empty())
        manifest["originalPath"] = data.originalPath;

    // The manifest goes first so tools can sniff the format cheaply.
    zip.add(kManifest, manifest.dump(1), MZ_DEFAULT_LEVEL);
    for (const auto& [name, bytes] : blobs)
        zip.add(name, bytes, name.starts_with("meshes/") ? MZ_BEST_SPEED : MZ_DEFAULT_LEVEL);
    return zip.finish();
}

namespace {

ProjectData decodeImpl(std::string_view bytes)
{
    ZipReader zip(bytes);
    if (!zip.contains(kManifest))
        throw ProjectError("Not an AeroCAD project (no manifest.json)");
    Json m;
    try {
        m = Json::parse(zip.read(kManifest));
    } catch (const Json::parse_error& e) {
        throw ProjectError(std::string("Corrupt project manifest: ") + e.what());
    }
    if (!m.is_object() || m.value("format", "") != kFormatName)
        throw ProjectError("Not an AeroCAD project (unknown manifest format)");
    const int version = field<int>(m, "version", "manifest");
    if (version < 1)
        throw ProjectError("Invalid project format version " + std::to_string(version));
    if (version > kFormatVersion) {
        throw ProjectError("The project was saved by a newer AeroCAD (format version " + std::to_string(version) +
                           "; this version reads up to " + std::to_string(kFormatVersion) + "). Please update.");
    }
    if (m.value("units", "mm") != "mm")
        throw ProjectError("Unsupported units '" + m.value("units", "") + "'");

    ProjectData data;
    data.formatVersion = version;
    data.generator = m.value("generator", "");
    data.originalPath = m.value("originalPath", "");
    data.view = m.value("view", Json::object());
    if (!data.view.is_object())
        data.view = Json::object();

    std::set<model::PartId> ids;
    for (const auto& j : m.value("parts", Json::array())) {
        model::Part p;
        p.id = field<model::PartId>(j, "id", "part");
        p.name = field<std::string>(j, "name", "part " + std::to_string(p.id));
        p.source = j.value("source", "");
        p.visible = j.value("visible", true);
        p.color = savedColor(j);
        if (const auto u = j.value("importUnits", std::string{}); core::parseLengthUnit(u))
            p.importUnits = u;
        p.kind = kindFromName(j.value("kind", "imported"));
        if (p.kind != model::PartKind::Imported)
            throw ProjectError("Part " + p.name + ": only imported parts are stored");
        if (p.id == 0 || !ids.insert(p.id).second)
            throw ProjectError("Invalid or duplicate part id " + std::to_string(p.id));
        if (j.contains("brep")) {
            geometry::Shape shape = geometry::fromBrep(zip.read(field<std::string>(j, "brep", "part " + p.name)));
            if (shape.isNull())
                throw ProjectError("Part " + p.name + ": invalid B-rep data");
            p.shape = std::move(shape);
        }
        if (j.contains("mesh")) {
            p.mesh = std::make_shared<const core::TriangleMesh>(
                decodeMesh(zip.read(field<std::string>(j, "mesh", "part " + p.name))));
        } else if (p.shape) {
            p.mesh = std::make_shared<const core::TriangleMesh>(geometry::tessellate(*p.shape));
        } else {
            throw ProjectError("Part " + p.name + " has neither a mesh nor a B-rep");
        }
        if (j.contains("selection")) {
            auto selection = std::make_shared<core::TriangleSet>(
                decodeIndices(zip.read(field<std::string>(j, "selection", "part " + p.name))));
            for (const auto t : *selection) {
                if (t >= p.mesh->triangleCount())
                    throw ProjectError("Part " + p.name + ": selection refers to a missing triangle");
            }
            p.selection = std::move(selection);
        }
        data.parts.push_back(std::move(p));
    }

    for (const auto& j : m.value("timelineParts", Json::array())) {
        TimelinePart t;
        t.feature = field<cad::FeatureId>(j, "feature", "timeline part");
        if (j.contains("kind"))
            t.kind = kindFromName(field<std::string>(j, "kind", "timeline part"));
        t.id = field<model::PartId>(j, "id", "timeline part");
        t.name = j.value("name", "");
        t.visible = j.value("visible", true);
        t.color = savedColor(j);
        if (j.contains("selection")) {
            t.selection = std::make_shared<const core::TriangleSet>(
                decodeIndices(zip.read(field<std::string>(j, "selection", "timeline part"))));
        }
        if (t.kind == model::PartKind::Imported || t.id == 0)
            throw ProjectError("Invalid timeline part " + std::to_string(t.id));
        if (!ids.insert(t.id).second)
            continue; // id taken: the part gets a new id when it is regenerated
        data.timelineParts.push_back(std::move(t));
    }

    // Cached results are an optimisation: one that cannot be read is rebuilt instead.
    for (const auto& j : m.value("timelineResults", Json::array())) {
        try {
            cad::Design::StoredResult r;
            r.feature = j.at("feature").get<cad::FeatureId>();
            r.key = j.at("key").get<std::string>();
            r.output.report = j.value("report", Json());
            for (const auto& b : j.at("bodies")) {
                geometry::Shape shape = geometry::fromBrep(zip.read(b.at("brep").get<std::string>()));
                if (shape.isNull())
                    throw ProjectError("invalid B-rep");
                r.output.bodies.emplace_back(std::move(shape), b.value("name", ""));
                if (b.contains("slot"))
                    r.output.bodySlots.push_back(b.at("slot").get<unsigned>());
            }
            if (!r.output.bodySlots.empty() && r.output.bodySlots.size() != r.output.bodies.size())
                throw ProjectError("slots do not match bodies");
            if (!r.output.bodies.empty())
                data.timelineResults.push_back(std::move(r));
        } catch (const std::exception&) {
        }
    }

    try {
        data.design = cad::stateFromJson(m.value("design", Json::object()));
        const Json vj = m.value("variants", Json::object());
        std::vector<cad::Variant> variants;
        for (const auto& j : vj.value("items", Json::array())) {
            cad::Variant v;
            v.id = field<cad::VariantId>(j, "id", "variant");
            v.name = field<std::string>(j, "name", "variant");
            if (j.contains("design"))
                v.state = cad::stateFromJson(j.at("design"));
            variants.push_back(std::move(v));
        }
        if (!variants.empty()) {
            data.variants.assign(std::move(variants), vj.value("active", cad::VariantId{0}),
                                 vj.value("previous", cad::VariantId{0}));
        }
    } catch (const cad::DesignError& e) {
        throw ProjectError(std::string("Invalid design data: ") + e.what());
    }
    return data;
}

} // namespace

ProjectData decodeProject(std::string_view bytes)
{
    try {
        return decodeImpl(bytes);
    } catch (const Json::exception& e) {
        throw ProjectError(std::string("Corrupt project manifest: ") + e.what());
    }
}

void saveProject(const std::filesystem::path& path, const ProjectData& data)
{
    writeFileAtomically(path, encodeProject(data));
}

ProjectData loadProject(const std::filesystem::path& path)
{
    return decodeProject(readFile(path));
}

} // namespace aerocad::project
