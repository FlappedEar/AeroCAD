// SPDX-License-Identifier: GPL-3.0-or-later
// Export and mesh validity commands.
#include "ExportCommands.hpp"

#include "aerocad/core/MeshCheck.hpp"
#include "aerocad/core/Path.hpp"
#include "aerocad/geometry/Tessellation.hpp"
#include "aerocad/io/Export.hpp"
#include "aerocad/model/Document.hpp"

#include <deque>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace aerocad::commands {

namespace {

/// The mesh written for a part: exact (B-rep) parts are tessellated finely for the file
/// (0.02 % of their size, about 8 degrees per facet; the view uses a coarser mesh), so a
/// printed fillet or curved skin is smooth. Kept in `store`.
const core::TriangleMesh* fileMesh(const model::Part& p, std::deque<core::TriangleMesh>& store)
{
    if (!p.shape || p.shape->isNull())
        return p.mesh.get();
    store.push_back(geometry::tessellate(*p.shape, {0.0002, 0.15}));
    return &store.back();
}

Json checkToJson(const core::MeshCheck& c)
{
    return Json{{"watertight", c.watertight()},
                {"closed", c.closed()},
                {"manifold", c.manifold()},
                {"oriented", c.oriented()},
                {"vertices", c.vertices},
                {"triangles", c.triangles},
                {"boundaryEdges", c.boundaryEdges},
                {"nonManifoldEdges", c.nonManifoldEdges},
                {"inconsistentEdges", c.inconsistentEdges},
                {"degenerateTriangles", c.degenerateTriangles},
                {"shells", c.shells},
                {"volume", c.closed() ? Json(c.signedVolume) : Json(nullptr)},
                {"problems", c.problems()}};
}

const model::Part& findPart(const model::Document& doc, model::PartId id)
{
    const model::Part* p = doc.part(id);
    if (p == nullptr)
        throw CommandError("No part with id " + std::to_string(id));
    return *p;
}

Json objectSchema(Json properties, Json required = Json::array())
{
    return Json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)},
                {"additionalProperties", false}};
}

io::FileFormat parseFormat(const Json& params, const std::filesystem::path& path)
{
    if (params.contains("format")) {
        const std::string f = params["format"].get<std::string>();
        if (f == "stl")
            return io::FileFormat::Stl;
        if (f == "3mf")
            return io::FileFormat::ThreeMf;
        return io::FileFormat::Step;
    }
    const io::FileFormat detected = io::formatFromExtension(path);
    if (!io::canExport(detected))
        throw CommandError("Cannot tell the export format from '" + core::pathToUtf8(path.filename()) +
                           "'. Use a .stl, .3mf or .step extension, or pass \"format\".");
    return detected;
}

/// Parts to export: the listed ids (in the given order, duplicates ignored) or all visible
/// bodies and imported parts.
std::vector<const model::Part*> selectParts(const model::Document& doc, const Json& params)
{
    std::vector<const model::Part*> parts;
    if (params.contains("ids")) {
        std::unordered_set<model::PartId> seen;
        for (const auto& id : params["ids"]) {
            const auto partId = id.get<model::PartId>();
            if (seen.insert(partId).second)
                parts.push_back(&findPart(doc, partId));
        }
    } else {
        // Sketch profiles and construction planes are helpers, not geometry to export.
        for (const auto& p : doc.parts()) {
            if (p.visible && p.kind != model::PartKind::Sketch && p.kind != model::PartKind::Plane)
                parts.push_back(&p);
        }
        if (parts.empty())
            throw CommandError("No visible parts to export");
    }
    return parts;
}

void registerCheck(CommandRegistry& r)
{
    r.add(
        {"part.check", "Check mesh",
         "Checks whether a part's mesh is a watertight solid: closed (no open edges), manifold "
         "(no edge shared by more than two triangles), consistently and outward oriented, without "
         "degenerate triangles. Reports counts, the enclosed volume (mm^3) and a list of problems.",
         objectSchema({{"id", {{"type", "integer"}, {"minimum", 1}, {"description", "Part id (see document.parts)."}}}},
                      {"id"})},
        [](CommandContext& ctx, const Json& params) {
            const model::Part& p = findPart(ctx.document, params.at("id").get<model::PartId>());
            if (!p.mesh)
                throw CommandError("Part '" + p.name + "' has no mesh");
            Json out = checkToJson(core::checkMesh(*p.mesh));
            out["id"] = p.id;
            out["name"] = p.name;
            return out;
        });
}

void registerExport(CommandRegistry& r)
{
    r.add({"document.export", "Export",
           "Exports parts to a file in millimetres: STL (binary, all parts merged), 3MF (one object per "
           "part, unit millimeter) or STEP (exact B-rep, only parts that have it, e.g. imported from "
           "STEP/IGES). Meshes are checked first (see part.check); problems are reported as warnings, "
           "or abort the export when requireValid is true. An existing file is replaced.",
           objectSchema(
               {{"path", {{"type", "string"}, {"minLength", 1}, {"description", "Absolute path of the output file."}}},
                {"format",
                 {{"type", "string"},
                  {"enum", {"stl", "3mf", "step"}},
                  {"description", "Output format; defaults to the file extension."}}},
                {"ids",
                 {{"type", "array"},
                  {"items", {{"type", "integer"}, {"minimum", 1}}},
                  {"minItems", 1},
                  {"description", "Parts to export; defaults to all visible bodies and imported parts (not sketches or construction planes)."}}},
                {"requireValid",
                 {{"type", "boolean"},
                  {"description", "Fail instead of warning when a mesh is not watertight (STL/3MF). Default false."}}}},
               {"path"})},
          [](CommandContext& ctx, const Json& params) {
              const std::filesystem::path path = core::pathFromUtf8(params.at("path").get<std::string>());
              if (!path.is_absolute())
                  throw CommandError("Export path must be absolute: " + core::pathToUtf8(path));
              const io::FileFormat format = parseFormat(params, path);
              const bool requireValid = params.value("requireValid", false);
              const std::vector<const model::Part*> parts = selectParts(ctx.document, params);

              Json checks = Json::array();
              Json warnings = Json::array();
              std::vector<io::ExportItem> items;
              std::deque<core::TriangleMesh> meshes;
              std::size_t triangles = 0;
              for (const model::Part* p : parts) {
                  io::ExportItem item;
                  item.name = p->name;
                  item.mesh = format == io::FileFormat::Step ? p->mesh.get() : fileMesh(*p, meshes);
                  item.shape = p->shape ? &*p->shape : nullptr;
                  items.push_back(item);

                  if (format == io::FileFormat::Step || item.mesh == nullptr)
                      continue;
                  triangles += item.mesh->triangleCount();
                  // Check what is written: the writers drop triangles collapsed to an edge or
                  // a point (B-rep tessellations have them at singular points).
                  const core::MeshCheck check = core::checkMesh(core::weldVertices(*item.mesh));
                  Json c = checkToJson(check);
                  c["id"] = p->id;
                  c["name"] = p->name;
                  checks.push_back(c);
                  for (const auto& problem : check.problems())
                      warnings.push_back(p->name + ": " + problem);
              }
              if (requireValid && !warnings.empty()) {
                  std::string msg = "Export aborted, meshes are not watertight:";
                  for (const auto& w : warnings)
                      msg += "\n- " + w.get<std::string>();
                  throw CommandError(msg);
              }

              try {
                  io::exportFile(path, items, format);
              } catch (const io::ExportError& e) {
                  throw CommandError(e.what());
              }

              Json ids = Json::array();
              for (const model::Part* p : parts)
                  ids.push_back(p->id);
              Json out{{"path", core::pathToUtf8(path)},
                       {"format", std::string(io::formatName(format))},
                       {"ids", ids},
                       {"checks", checks},
                       {"warnings", warnings}};
              if (format != io::FileFormat::Step)
                  out["triangles"] = triangles;
              return out;
          });
}

/// A part name as a file name: characters file systems refuse become "-".
std::string fileStem(const std::string& name)
{
    std::string stem;
    for (const char c : name)
        stem += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
                 c == '|' || static_cast<unsigned char>(c) < 32)
                    ? '-'
                    : c;
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.'))
        stem.pop_back();
    return stem.empty() ? "part" : stem;
}

void registerExportForPrint(CommandRegistry& r)
{
    r.add({"document.export_for_print", "Export for printing",
           "Writes each part to its own files in a folder, ready to print: <name>.stl (binary, mm, finely "
           "tessellated) and <name>.step (exact B-rep, for parts that have it), e.g. a set of canards. Every "
           "STL must be a watertight solid, or nothing is written. Existing files of the same names are replaced.",
           objectSchema({{"folder", {{"type", "string"}, {"minLength", 1}, {"description", "Absolute path of the folder."}}},
                         {"ids",
                          {{"type", "array"},
                           {"items", {{"type", "integer"}, {"minimum", 1}}},
                           {"minItems", 1},
                           {"description", "Parts to write; defaults to all visible solids (not scans or surfaces)."}}},
                         {"formats",
                          {{"type", "array"},
                           {"items", {{"type", "string"}, {"enum", {"stl", "step"}}}},
                           {"minItems", 1},
                           {"description", "Default both."}}}},
                        {"folder"})},
          [](CommandContext& ctx, const Json& params) {
              const std::filesystem::path folder = core::pathFromUtf8(params.at("folder").get<std::string>());
              if (!folder.is_absolute())
                  throw CommandError("Folder must be an absolute path: " + core::pathToUtf8(folder));
              std::error_code ec;
              std::filesystem::create_directories(folder, ec);
              if (!std::filesystem::is_directory(folder))
                  throw CommandError("Cannot create the folder " + core::pathToUtf8(folder));
              bool stl = true, step = true;
              if (params.contains("formats")) {
                  stl = step = false;
                  for (const auto& f : params.at("formats")) {
                      stl = stl || f == "stl";
                      step = step || f == "step";
                  }
              }
              std::vector<const model::Part*> parts;
              if (params.contains("ids")) {
                  parts = selectParts(ctx.document, params);
              } else {
                  for (const auto& p : ctx.document.parts())
                      if (p.visible && p.shape && !p.shape->isNull() && p.mesh &&
                          core::checkMesh(core::weldVertices(*p.mesh)).closed())
                          parts.push_back(&p);
                  if (parts.empty())
                      throw CommandError("No visible solid parts to export");
              }

              // Check everything before writing anything.
              struct Planned {
                  const model::Part* part;
                  const core::TriangleMesh* mesh;
                  std::string stem;
              };
              std::deque<core::TriangleMesh> meshes;
              std::vector<Planned> planned;
              std::unordered_set<std::string> stems;
              std::vector<std::string> problems;
              for (const model::Part* p : parts) {
                  std::string stem = fileStem(p->name);
                  for (int k = 2; !stems.insert(stem).second; ++k)
                      stem = fileStem(p->name) + " " + std::to_string(k);
                  const core::TriangleMesh* mesh = stl ? fileMesh(*p, meshes) : nullptr;
                  if (stl) {
                      if (mesh == nullptr) {
                          problems.push_back(p->name + ": no mesh");
                      } else {
                          for (const auto& problem : core::checkMesh(core::weldVertices(*mesh)).problems())
                              problems.push_back(p->name + ": " + problem);
                      }
                  }
                  planned.push_back({p, mesh, stem});
              }
              if (!problems.empty()) {
                  std::string msg = "Nothing was written; not every part is a watertight solid:";
                  for (const auto& w : problems)
                      msg += "\n- " + w;
                  throw CommandError(msg);
              }

              Json files = Json::array();
              Json warnings = Json::array();
              try {
                  for (const auto& pl : planned) {
                      if (stl) {
                          const auto path = folder / core::pathFromUtf8(pl.stem + ".stl");
                          io::exportFile(path, {{pl.part->name, pl.mesh, nullptr}}, io::FileFormat::Stl);
                          files.push_back({{"id", pl.part->id}, {"format", "stl"}, {"path", core::pathToUtf8(path)},
                                           {"triangles", pl.mesh->triangleCount()}});
                      }
                      if (step) {
                          if (!pl.part->shape || pl.part->shape->isNull()) {
                              warnings.push_back(pl.part->name + ": no exact geometry, so no STEP file");
                              continue;
                          }
                          const auto path = folder / core::pathFromUtf8(pl.stem + ".step");
                          io::exportFile(path, {{pl.part->name, nullptr, &*pl.part->shape}}, io::FileFormat::Step);
                          files.push_back({{"id", pl.part->id}, {"format", "step"}, {"path", core::pathToUtf8(path)}});
                      }
                  }
              } catch (const io::ExportError& e) {
                  throw CommandError(e.what());
              }
              return Json{{"folder", core::pathToUtf8(folder)}, {"files", files}, {"warnings", warnings},
                          {"parts", planned.size()}};
          });
}

} // namespace

void registerExportCommands(CommandRegistry& registry)
{
    registerCheck(registry);
    registerExport(registry);
    registerExportForPrint(registry);
}

} // namespace aerocad::commands
