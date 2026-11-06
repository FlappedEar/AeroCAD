// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"

#include "ExportCommands.hpp"

#include "aerocad/core/Path.hpp"
#include "aerocad/geometry/Transform.hpp"
#include "aerocad/io/Import.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace aerocad::commands {

namespace {

Json vecToJson(const core::Vec3& v)
{
    return Json::array({v.x, v.y, v.z});
}

Json boundsToJson(const core::BoundingBox& box)
{
    if (box.isEmpty())
        return nullptr;
    return Json{{"min", vecToJson(box.min)}, {"max", vecToJson(box.max)}, {"size", vecToJson(box.size())}};
}

Json partToJson(const model::Part& p)
{
    return Json{
        {"id", p.id},
        {"name", p.name},
        {"source", p.source},
        {"visible", p.visible},
        {"vertices", p.mesh ? p.mesh->vertexCount() : 0},
        {"triangles", p.mesh ? p.mesh->triangleCount() : 0},
        {"hasBrep", p.shape.has_value()},
        {"bounds", boundsToJson(p.mesh ? p.mesh->bounds() : core::BoundingBox{})},
    };
}

const model::Part& requirePart(const model::Document& doc, const Json& params)
{
    const auto id = params.at("id").get<model::PartId>();
    const model::Part* p = doc.part(id);
    if (p == nullptr)
        throw CommandError("No part with id " + std::to_string(id));
    return *p;
}

Json idParam()
{
    return Json{{"type", "integer"}, {"minimum", 1}, {"description", "Part id (see document.parts)."}};
}

Json objectSchema(Json properties, Json required = Json::array())
{
    return Json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)},
                {"additionalProperties", false}};
}

/// Records a part-level change in the undo stack: swaps the part state.
void pushPartStateUndo(CommandContext& ctx, std::string label, model::PartId id, model::Part before, model::Part after)
{
    model::Document& doc = ctx.document;
    ctx.undoStack.push({std::move(label),
                        [&doc, id, before]() { doc.modifyPart(id, [&](model::Part& p) { p = before; }); },
                        [&doc, id, after]() { doc.modifyPart(id, [&](model::Part& p) { p = after; }); }});
}

void registerIntrospection(CommandRegistry& r)
{
    r.add({"commands.list", "List commands",
           "Lists all commands with their parameter schemas. Used by the MCP server to build its tool list.",
           objectSchema(Json::object())},
          [](CommandContext& ctx, const Json&) {
              Json out = Json::array();
              if (ctx.registry != nullptr) {
                  for (const auto& info : ctx.registry->list()) {
                      out.push_back({{"name", info.name},
                                     {"title", info.title},
                                     {"description", info.description},
                                     {"params", info.params},
                                     {"modifiesDocument", info.modifiesDocument}});
                  }
              }
              return Json{{"commands", out}};
          });
}

void registerDocumentCommands(CommandRegistry& r)
{

    r.add({"document.import", "Import 3D file",
           "Imports a 3D file (STL, OBJ, 3MF, STEP, IGES, BREP) as a new part. Coordinates are in millimetres.",
           objectSchema({{"path", {{"type", "string"}, {"minLength", 1}, {"description", "Absolute path of the file."}}},
                         {"name", {{"type", "string"}, {"description", "Optional part name; defaults to the file name."}}}},
                        {"path"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const std::filesystem::path path = core::pathFromUtf8(params.at("path").get<std::string>());
              io::ImportResult imported;
              try {
                  imported = io::importFile(path);
              } catch (const io::ImportError& e) {
                  throw CommandError(e.what());
              }

              model::Part part;
              part.name = params.contains("name") ? params["name"].get<std::string>() : core::pathToUtf8(path.stem());
              part.source = core::pathToUtf8(path);
              part.mesh = std::make_shared<const core::TriangleMesh>(std::move(imported.mesh));
              part.shape = std::move(imported.shape);

              model::Document& doc = ctx.document;
              const model::PartId id = doc.addPart(part);
              part.id = id;
              ctx.undoStack.push({"Import " + part.name,
                                  [&doc, id]() { doc.removePart(id); },
                                  [&doc, part]() { doc.addPart(part); }});

              Json result = partToJson(*doc.part(id));
              result["format"] = std::string(io::formatName(imported.format));
              result["warnings"] = imported.warnings;
              return result;
          });

    r.add({"document.parts", "List parts", "Lists all parts in the document with their size and bounds.",
           objectSchema(Json::object())},
          [](CommandContext& ctx, const Json&) {
              Json parts = Json::array();
              for (const auto& p : ctx.document.parts())
                  parts.push_back(partToJson(p));
              return Json{{"parts", parts}};
          });

    r.add({"document.bounds", "Document bounds", "Bounding box of all visible parts (mm).",
           objectSchema(Json::object())},
          [](CommandContext& ctx, const Json&) { return Json{{"bounds", boundsToJson(ctx.document.bounds())}}; });
}

void registerPartCommands(CommandRegistry& r)
{
    r.add({"part.remove", "Remove part", "Removes a part from the document.", objectSchema({{"id", idParam()}}, {"id"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              model::Document& doc = ctx.document;
              model::Part removed = *doc.removePart(requirePart(doc, params).id);
              ctx.undoStack.push({"Remove " + removed.name,
                                  [&doc, removed]() { doc.addPart(removed); },
                                  [&doc, id = removed.id]() { doc.removePart(id); }});
              return Json{{"removed", removed.id}};
          });

    r.add({"part.set_visible", "Show/hide part", "Shows or hides a part in the viewport.",
           objectSchema({{"id", idParam()}, {"visible", {{"type", "boolean"}}}}, {"id", "visible"}), true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part before = requirePart(ctx.document, params);
              model::Part after = before;
              after.visible = params.at("visible").get<bool>();
              ctx.document.modifyPart(before.id, [&](model::Part& p) { p.visible = after.visible; });
              pushPartStateUndo(ctx, after.visible ? "Show " + before.name : "Hide " + before.name, before.id, before,
                                *ctx.document.part(before.id));
              return partToJson(*ctx.document.part(before.id));
          });

    r.add({"part.rename", "Rename part", "Renames a part.",
           objectSchema({{"id", idParam()}, {"name", {{"type", "string"}, {"minLength", 1}}}}, {"id", "name"}), true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part before = requirePart(ctx.document, params);
              ctx.document.modifyPart(before.id, [&](model::Part& p) { p.name = params.at("name").get<std::string>(); });
              pushPartStateUndo(ctx, "Rename " + before.name, before.id, before, *ctx.document.part(before.id));
              return partToJson(*ctx.document.part(before.id));
          });

    r.add({"part.scale", "Scale part",
           "Uniformly scales a part (mesh and exact geometry) about a point, by default the centre of its bounding box.",
           objectSchema({{"id", idParam()},
                         {"factor", {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Scale factor, e.g. 2 doubles the size."}}},
                         {"origin", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3},
                                     {"description", "Optional [x, y, z] in mm."}}}},
                        {"id", "factor"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part before = requirePart(ctx.document, params);
              const double factor = params.at("factor").get<double>();
              core::Vec3 origin = before.mesh ? before.mesh->bounds().center() : core::Vec3{};
              if (params.contains("origin")) {
                  const auto& o = params["origin"];
                  origin = {o[0].get<double>(), o[1].get<double>(), o[2].get<double>()};
              }
              ctx.document.modifyPart(before.id, [&](model::Part& p) {
                  if (p.mesh) {
                      auto mesh = std::make_shared<core::TriangleMesh>(*p.mesh);
                      mesh->scale(factor, origin);
                      p.mesh = std::move(mesh);
                  }
                  if (p.shape)
                      p.shape = geometry::scaled(*p.shape, factor, origin);
              });
              pushPartStateUndo(ctx, "Scale " + before.name, before.id, before, *ctx.document.part(before.id));
              return partToJson(*ctx.document.part(before.id));
          });
}

void registerEditCommands(CommandRegistry& r)
{
    r.add({"edit.undo", "Undo", "Undoes the last document change.", objectSchema(Json::object()), true},
          [](CommandContext& ctx, const Json&) {
              const std::string label = ctx.undoStack.undoLabel();
              if (!ctx.undoStack.undo())
                  throw CommandError("Nothing to undo");
              return Json{{"undone", label}};
          });
    r.add({"edit.redo", "Redo", "Redoes the last undone change.", objectSchema(Json::object()), true},
          [](CommandContext& ctx, const Json&) {
              const std::string label = ctx.undoStack.redoLabel();
              if (!ctx.undoStack.redo())
                  throw CommandError("Nothing to redo");
              return Json{{"redone", label}};
          });
    r.add({"edit.history", "History", "Undo/redo state.", objectSchema(Json::object())},
          [](CommandContext& ctx, const Json&) {
              return Json{{"canUndo", ctx.undoStack.canUndo()},
                          {"canRedo", ctx.undoStack.canRedo()},
                          {"undoLabel", ctx.undoStack.undoLabel()},
                          {"redoLabel", ctx.undoStack.redoLabel()}};
          });
}

} // namespace

void registerBuiltinCommands(CommandRegistry& registry)
{
    registerIntrospection(registry);
    registerDocumentCommands(registry);
    registerPartCommands(registry);
    registerEditCommands(registry);
    registerExportCommands(registry);
}

} // namespace aerocad::commands
