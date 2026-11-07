// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"

#include "CommandHelpers.hpp"
#include "ExportCommands.hpp"

#include "aerocad/commands/ImportStage.hpp"
#include "aerocad/core/Path.hpp"
#include "aerocad/core/Units.hpp"
#include "aerocad/geometry/Transform.hpp"
#include "aerocad/io/Import.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace aerocad::commands {

namespace {

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
           "Imports a 3D file (STL, OBJ, PLY, 3MF, STEP, IGES, BREP) as a new part. The document works in "
           "millimetres; pass `units` when the file was written in another unit (mesh formats such as STL and "
           "PLY carry none) and the geometry is scaled to millimetres.",
           objectSchema({{"path", {{"type", "string"}, {"minLength", 1}, {"description", "Absolute path of the file."}}},
                         {"name", {{"type", "string"}, {"description", "Optional part name; defaults to the file name."}}},
                         {"units", {{"type", "string"},
                                    {"enum", {"um", "mm", "cm", "m", "in", "ft"}},
                                    {"description", "Unit the file coordinates are in (default mm)."}}}},
                        {"path"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const std::string pathText = params.at("path").get<std::string>();
              const std::filesystem::path path = core::pathFromUtf8(pathText);
              io::ImportResult imported;
              std::optional<io::ImportResult> staged;
              if (ctx.importStage != nullptr)
                  staged = ctx.importStage->take(pathText);
              if (staged) {
                  imported = std::move(*staged);
              } else {
                  try {
                      imported = io::importFile(path);
                  } catch (const io::ImportError& e) {
                      throw CommandError(e.what());
                  }
              }
              const core::LengthUnit unit =
                  core::parseLengthUnit(params.value("units", std::string("mm"))).value_or(core::LengthUnit::Millimetre);
              const double unitScale = core::toMillimetres(unit);
              if (unitScale != 1.0) {
                  imported.mesh.scale(unitScale, core::Vec3{});
                  if (imported.shape)
                      imported.shape = geometry::scaled(*imported.shape, unitScale, core::Vec3{});
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
              result["units"] = std::string(core::unitSymbol(unit));
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
              const model::Part& target = requirePart(doc, params);
              requireImported(target, "remove");
              model::Part removed = *doc.removePart(target.id);
              ctx.undoStack.push({"Remove " + removed.name,
                                  [&doc, removed]() { doc.addPart(removed); },
                                  [&doc, id = removed.id]() { doc.removePart(id); }});
              return Json{{"removed", removed.id}};
          });

    r.add({"part.duplicate", "Duplicate part",
           "Copies an imported part (mesh and exact geometry) as a new part, e.g. to keep the original next to "
           "a modified copy. Bodies and sketches are duplicated with variant.create instead.",
           objectSchema({{"id", idParam()}, {"name", {{"type", "string"}, {"minLength", 1}}}}, {"id"}), true},
          [](CommandContext& ctx, const Json& params) {
              model::Document& doc = ctx.document;
              const model::Part& source = requirePart(doc, params);
              requireImported(source, "duplicate");
              // `source` dangles once addPart() reallocates the part list.
              const std::string label = "Duplicate " + source.name;
              model::Part copy = source;
              copy.id = 0;
              copy.revision = 0;
              copy.name = params.contains("name") ? params["name"].get<std::string>() : source.name + " (copy)";
              const model::PartId id = doc.addPart(copy);
              copy.id = id;
              ctx.undoStack.push({label,
                                  [&doc, id]() { doc.removePart(id); },
                                  [&doc, copy]() { doc.addPart(copy); }});
              return partToJson(*doc.part(id));
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
                         {"factor", {{"type", "number"},
                                     {"minimum", geometry::kMinScaleFactor},
                                     {"maximum", geometry::kMaxScaleFactor},
                                     {"description", "Scale factor, e.g. 2 doubles the size."}}},
                         {"origin", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3},
                                     {"description", "Optional [x, y, z] in mm."}}}},
                        {"id", "factor"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part before = requirePart(ctx.document, params);
              requireImported(before, "scale");
              const double factor = params.at("factor").get<double>();
              core::Vec3 origin = before.mesh ? before.mesh->bounds().center() : core::Vec3{};
              if (params.contains("origin")) {
                  const auto& o = params["origin"];
                  origin = {o[0].get<double>(), o[1].get<double>(), o[2].get<double>()};
              }
              // Build the new geometry first: if the kernel rejects it, nothing has changed.
              std::shared_ptr<const core::TriangleMesh> mesh = before.mesh;
              if (before.mesh) {
                  auto scaledMesh = std::make_shared<core::TriangleMesh>(*before.mesh);
                  scaledMesh->scale(factor, origin);
                  mesh = std::move(scaledMesh);
              }
              std::optional<geometry::Shape> shape = before.shape;
              if (before.shape)
                  shape = geometry::scaled(*before.shape, factor, origin);
              ctx.document.modifyPart(before.id, [&](model::Part& p) {
                  p.mesh = mesh;
                  p.shape = shape;
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
    registerSelectionCommands(registry);
    registerExportCommands(registry);
    registerSurfaceCommands(registry);
}

} // namespace aerocad::commands
