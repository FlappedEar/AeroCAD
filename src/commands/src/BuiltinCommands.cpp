// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"

#include "CommandHelpers.hpp"
#include "ExportCommands.hpp"

#include "aerocad/commands/ImportStage.hpp"
#include "aerocad/core/Path.hpp"
#include "aerocad/core/Units.hpp"
#include "aerocad/geometry/Transform.hpp"
#include "aerocad/io/Import.hpp"

#include <cctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
                                     {"modifiesDocument", info.modifiesDocument},
                                     {"effects",
                                      {{"readsFiles", info.effects.readsFiles},
                                       {"writesFiles", info.effects.writesFiles},
                                       {"deletesFiles", info.effects.deletesFiles},
                                       {"runsProcesses", info.effects.runsProcesses},
                                       {"network", info.effects.network},
                                       {"installsSoftware", info.effects.installsSoftware},
                                       {"undoable", info.effects.undoable},
                                       {"destructive", info.effects.destructive}}}});
                  }
              }
              return Json{{"commands", out}};
          });
}

void registerDocumentCommands(CommandRegistry& r)
{

    r.add({"document.import", "Import 3D file",
           "Imports a 3D file (STL, OBJ, PLY, 3MF, STEP, IGES, BREP) as a new part. A STEP file with several "
           "bodies gives one part per body, with the file's names and colours; the result is the first part "
           "and `parts` lists all of them. The document works in "
           "millimetres. STEP, IGES and 3MF use their file units automatically. For unitless STL, OBJ, PLY "
           "and BREP files, `units` selects the source unit to convert to millimetres.",
           objectSchema({{"path", {{"type", "string"}, {"minLength", 1}, {"description", "Absolute path of the file."}}},
                         {"name", {{"type", "string"}, {"description", "Optional part name; defaults to the file name."}}},
                         {"units", {{"type", "string"},
                                    {"enum", {"um", "mm", "cm", "m", "in", "ft"}},
                                    {"description", "Source unit for unitless files (default mm); ignored for STEP, "
                                                    "IGES and 3MF, whose file units are used automatically."}}}},
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
              // These readers already normalize both the mesh and exact shape to millimetres.
              // Use the parsed format, also for worker-thread results consumed from ImportStage.
              const bool fileUnits = imported.format == io::FileFormat::Step ||
                                     imported.format == io::FileFormat::Iges || imported.format == io::FileFormat::ThreeMf;
              const core::LengthUnit unit =
                  fileUnits ? core::LengthUnit::Millimetre
                            : core::parseLengthUnit(params.value("units", std::string("mm")))
                                  .value_or(core::LengthUnit::Millimetre);
              const double unitScale = core::toMillimetres(unit);
              if (unitScale != 1.0) {
                  imported.mesh.scale(unitScale, core::Vec3{});
                  if (imported.shape)
                      imported.shape = geometry::scaled(*imported.shape, unitScale, core::Vec3{});
              }

              const std::string baseName =
                  params.contains("name") ? params["name"].get<std::string>() : core::pathToUtf8(path.stem());
              std::vector<model::Part> newParts;
              if (imported.bodies.empty()) {
                  model::Part part;
                  part.name = baseName;
                  part.color = model::isPartColor(imported.color) ? imported.color : std::string{};
                  part.mesh = std::make_shared<const core::TriangleMesh>(std::move(imported.mesh));
                  part.shape = std::move(imported.shape);
                  newParts.push_back(std::move(part));
              } else {
                  // A file with several named bodies (STEP products) gives one part per body
                  // (CAD-227), named from the file or "<name> <n>".
                  std::size_t n = 0;
                  for (auto& body : imported.bodies) {
                      ++n;
                      model::Part part;
                      part.name = body.name.empty() ? baseName + " " + std::to_string(n) : body.name;
                      part.color = model::isPartColor(body.color) ? body.color : std::string{};
                      part.mesh = std::make_shared<const core::TriangleMesh>(std::move(body.mesh));
                      part.shape = std::move(body.shape);
                      newParts.push_back(std::move(part));
                  }
              }

              model::Document& doc = ctx.document;
              std::vector<model::PartId> ids;
              for (auto& part : newParts) {
                  part.source = core::pathToUtf8(path);
                  part.id = doc.addPart(part);
                  ids.push_back(part.id);
              }
              ctx.undoStack.push({"Import " + (newParts.size() == 1 ? newParts.front().name : baseName),
                                  [&doc, ids]() {
                                      for (const auto id : ids)
                                          doc.removePart(id);
                                  },
                                  [&doc, newParts]() {
                                      for (const auto& part : newParts)
                                          doc.addPart(part);
                                  }});

              const model::PartId id = ids.front();
              Json result = partToJson(*doc.part(id));
              if (ids.size() > 1) {
                  Json all = Json::array();
                  for (const auto partId : ids)
                      all.push_back(partToJson(*doc.part(partId)));
                  result["parts"] = all;
              }
              result["format"] = std::string(io::formatName(imported.format));
              result["units"] = std::string(core::unitSymbol(unit));
              result["unitSource"] = fileUnits ? "file" : "importSetting";
              result["outputUnits"] = "mm";
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
           "Copies a part (mesh and exact geometry) as a new part, e.g. to keep the original next to a modified "
           "copy (Copy / Paste). A body of the timeline is copied as a fixed solid outside the timeline: it keeps "
           "its shape but not its features. Sketches and planes cannot be copied.",
           objectSchema({{"id", idParam()}, {"name", {{"type", "string"}, {"minLength", 1}}}}, {"id"}), true},
          [](CommandContext& ctx, const Json& params) {
              model::Document& doc = ctx.document;
              const model::Part& source = requirePart(doc, params);
              if (source.kind == model::PartKind::Sketch || source.kind == model::PartKind::Plane)
                  throw CommandError("Cannot duplicate a sketch or plane: copy the body made from it");
              if (!source.mesh)
                  throw CommandError("Part has no geometry to copy");
              // `source` dangles once addPart() reallocates the part list.
              const std::string label = "Duplicate " + source.name;
              model::Part copy = source;
              copy.id = 0;
              copy.revision = 0;
              copy.selection = nullptr;
              if (copy.kind == model::PartKind::Body) {
                  // A fixed copy: the timeline does not rebuild or remove it.
                  copy.kind = model::PartKind::Imported;
                  copy.featureId = 0;
                  copy.source = "copy of " + source.name;
              }
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

    r.add({"part.set_color", "Appearance",
           "Sets the colour a part is shown in (Fusion's Appearance): \"#rrggbb\", or \"\" for the default "
           "colour of its kind. Saved with the project; timeline bodies keep it through edits.",
           objectSchema({{"id", idParam()},
                         {"color", {{"type", "string"}, {"description", "\"#rrggbb\" or \"\" (default)"}}}},
                        {"id", "color"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part before = requirePart(ctx.document, params);
              std::string color = params.at("color").get<std::string>();
              if (!color.empty() && !model::isPartColor(color))
                  throw CommandError("Colour must be \"#rrggbb\" or empty, got \"" + color + "\"");
              for (auto& ch : color)
                  ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
              ctx.document.modifyPart(before.id, [&](model::Part& p) { p.color = color; });
              pushPartStateUndo(ctx, "Appearance of " + before.name, before.id, before, *ctx.document.part(before.id));
              return partToJson(*ctx.document.part(before.id));
          });

    r.add({"part.isolate", "Isolate part",
           "Shows only this part and hides all the others (Fusion's Isolate), as one undo step. Returns the ids "
           "it hid in 'hidden': pass them to part.unisolate to bring back what was visible before.",
           objectSchema({{"id", idParam()}}, {"id"}), true},
          [](CommandContext& ctx, const Json& params) {
              const model::Part target = requirePart(ctx.document, params);
              std::vector<model::PartId> hidden;
              for (const auto& p : ctx.document.parts())
                  if (p.id != target.id && p.visible)
                      hidden.push_back(p.id);
              const bool shown = !target.visible;
              auto apply = [&doc = ctx.document, id = target.id, hidden, shown](bool isolate) {
                  if (shown)
                      doc.modifyPart(id, [&](model::Part& p) { p.visible = isolate; });
                  for (const auto h : hidden)
                      doc.modifyPart(h, [&](model::Part& p) { p.visible = !isolate; });
              };
              apply(true);
              ctx.undoStack.push({"Isolate " + target.name, [apply]() { apply(false); }, [apply]() { apply(true); }});
              return Json{{"id", target.id}, {"hidden", hidden}};
          });

    r.add({"part.unisolate", "Unisolate",
           "Shows the given parts again (the 'hidden' list of part.isolate), as one undo step. Ids of parts that "
           "no longer exist are skipped.",
           objectSchema({{"ids", {{"type", "array"}, {"items", idParam()}}}}, {"ids"}), true},
          [](CommandContext& ctx, const Json& params) {
              std::vector<model::PartId> shown;
              for (const auto& j : params.at("ids")) {
                  const auto id = j.get<model::PartId>();
                  const model::Part* p = ctx.document.part(id);
                  if (p != nullptr && !p->visible)
                      shown.push_back(id);
              }
              auto apply = [&doc = ctx.document, shown](bool visible) {
                  for (const auto id : shown)
                      doc.modifyPart(id, [&](model::Part& p) { p.visible = visible; });
              };
              apply(true);
              if (!shown.empty())
                  ctx.undoStack.push({"Unisolate", [apply]() { apply(false); }, [apply]() { apply(true); }});
              return Json{{"shown", shown}};
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
