// SPDX-License-Identifier: GPL-3.0-or-later
// Export and mesh validity commands.
#include "ExportCommands.hpp"

#include "aerocad/core/MeshCheck.hpp"
#include "aerocad/core/Path.hpp"
#include "aerocad/io/Export.hpp"
#include "aerocad/model/Document.hpp"

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace aerocad::commands {

namespace {

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

/// Parts to export: the listed ids (in the given order, duplicates ignored) or all visible parts.
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
        for (const auto& p : doc.parts()) {
            if (p.visible)
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
                  {"description", "Parts to export; defaults to all visible parts."}}},
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
              std::size_t triangles = 0;
              for (const model::Part* p : parts) {
                  io::ExportItem item;
                  item.name = p->name;
                  item.mesh = p->mesh.get();
                  item.shape = p->shape ? &*p->shape : nullptr;
                  items.push_back(item);

                  if (format == io::FileFormat::Step || !p->mesh)
                      continue;
                  triangles += p->mesh->triangleCount();
                  // Check what is written: the writers drop triangles collapsed to an edge or
                  // a point (B-rep tessellations have them at singular points).
                  const core::MeshCheck check = core::checkMesh(core::weldVertices(*p->mesh));
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

} // namespace

void registerExportCommands(CommandRegistry& registry)
{
    registerCheck(registry);
    registerExport(registry);
}

} // namespace aerocad::commands
