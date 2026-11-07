// SPDX-License-Identifier: GPL-3.0-or-later
// Project commands: new / open / save of .aerocad files and design variants (A/B).
#include "aerocad/commands/Session.hpp"

#include "aerocad/core/Path.hpp"
#include "aerocad/project/ProjectFile.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace aerocad::commands {

namespace {

Json objectSchema(Json properties, Json required = Json::array())
{
    return Json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)},
                {"additionalProperties", false}};
}

ProjectState& projectState(CommandContext& ctx)
{
    if (ctx.project == nullptr || ctx.design == nullptr)
        throw CommandError("No project in this session");
    return *ctx.project;
}

std::string projectName(const ProjectState& p)
{
    if (p.path.empty())
        return "Untitled";
    return core::pathToUtf8(core::pathFromUtf8(p.path).stem());
}

Json variantSummary(const ProjectState& p)
{
    const cad::Variant* active = p.variants.find(p.variants.active());
    return Json{{"variants", p.variants.size()},
                {"activeVariant", {{"id", p.variants.active()}, {"name", active ? active->name : ""}}}};
}

Json projectInfo(CommandContext& ctx)
{
    const ProjectState& p = projectState(ctx);
    Json j{{"path", p.path},
           {"name", projectName(p)},
           {"dirty", p.dirty()},
           {"recovered", p.recovered},
           {"formatVersion", project::kFormatVersion},
           {"parts", ctx.document.partCount()},
           {"features", ctx.design->features().size()}};
    j.update(variantSummary(p));
    return j;
}

/// Kept dimensions go into the (otherwise UI-owned) view object of the file.
void storeDimensions(const CommandContext& ctx, Json& view)
{
    if (!view.is_object())
        view = Json::object();
    if (ctx.inspect != nullptr && !ctx.inspect->dimensions().empty())
        view["dimensions"] = dimensionsToJson(ctx.inspect->dimensions());
    else
        view.erase("dimensions");
}

/// Collects the current session as project data.
project::ProjectData snapshot(CommandContext& ctx)
{
    ProjectState& p = projectState(ctx);
    project::ProjectData data;
    // Retained bookkeeping first (parts of suppressed features or other variants), then
    // the live document parts, which are newer.
    std::map<cad::FeatureId, project::TimelinePart> timeline;
    for (const auto& info : ctx.design->partInfos())
        timeline[info.feature] = {info.feature, std::nullopt, info.part, info.name, info.visible, nullptr};
    for (const auto& part : ctx.document.parts()) {
        if (part.kind == model::PartKind::Imported)
            data.parts.push_back(part);
        else
            timeline[part.featureId] = {part.featureId, part.kind, part.id, part.name, part.visible, part.selection};
    }
    for (auto& [_, t] : timeline)
        data.timelineParts.push_back(std::move(t));
    data.design = ctx.design->state();
    data.variants = p.variants;
    data.view = p.view;
    storeDimensions(ctx, data.view);
    data.generator = p.generator;
    return data;
}

/// Replaces the session with `data`. The undo history is cleared.
void applyProject(CommandContext& ctx, project::ProjectData data)
{
    cad::Design& d = *ctx.design;
    ctx.undoStack.clear();
    d.clear();
    ctx.document.clear();
    for (auto& part : data.parts)
        ctx.document.addPart(std::move(part));
    std::vector<std::pair<model::PartId, std::shared_ptr<const core::TriangleSet>>> selections;
    for (auto& t : data.timelineParts) {
        ctx.document.reserveIds(t.id); // also ids of parts that are not regenerated now
        if (t.selection)
            selections.emplace_back(t.id, t.selection);
        d.setPartInfo(t.feature, t.id, std::move(t.name), t.visible);
    }
    d.restore(std::move(data.design));
    d.syncDocument(ctx.document);
    // Region selections on regenerated parts: indices are kept only while they fit the
    // regenerated mesh (same rule as Design::syncDocument when geometry changes).
    for (auto& [id, selection] : selections) {
        const model::Part* part = ctx.document.part(id);
        if (part == nullptr || !part->mesh)
            continue;
        const std::size_t n = part->mesh->triangleCount();
        if (std::all_of(selection->begin(), selection->end(), [n](std::uint32_t t) { return t < n; }))
            ctx.document.modifyPart(id, [&](model::Part& p) { p.selection = selection; });
    }
    ProjectState& p = projectState(ctx);
    p.variants = std::move(data.variants);
    p.view = std::move(data.view);
    if (ctx.inspect != nullptr) {
        // Analyses belong to the project shown: a new or opened one starts without any.
        ctx.inspect->clear();
        ctx.inspect->setDimensions(p.view.is_object() && p.view.contains("dimensions")
                                       ? dimensionsFromJson(p.view["dimensions"])
                                       : std::vector<Dimension>{});
    }
}

/// Marks the project as just opened / saved: nothing unsaved.
void markClean(ProjectState& p)
{
    ++p.changes; // tells Session::execute that the counters were handled
    p.savedChanges = p.changes;
    p.recovered = false;
}

std::filesystem::path withExtension(std::string utf8)
{
    std::filesystem::path path = core::pathFromUtf8(utf8);
    if (!path.has_extension())
        path += std::string(project::kFileExtension);
    return std::filesystem::absolute(path);
}

void registerFileCommands(CommandRegistry& r)
{
    r.add({"project.info", "Project info",
           "Current project: file path (empty when untitled), name, unsaved changes (dirty), part / feature "
           "counts and the active variant.",
           objectSchema(Json::object())},
          [](CommandContext& ctx, const Json&) { return projectInfo(ctx); });

    r.add({"project.new", "New Design",
           "Closes the current project without saving and starts an empty untitled one. Clears the undo "
           "history. Check project.info dirty first and save if needed.",
           objectSchema(Json::object()), true},
          [](CommandContext& ctx, const Json&) {
              ProjectState& p = projectState(ctx);
              applyProject(ctx, project::ProjectData{});
              p.path.clear();
              markClean(p);
              return projectInfo(ctx);
          });

    r.add({"project.save", "Save",
           "Saves the project to a .aerocad file, atomically (an interrupted save never damages the existing "
           "file). Stores imported meshes losslessly, their exact B-rep, the design timeline, user parameters, "
           "reference frames, all variants and the view. Without path, saves to the current file. "
           "copy: true writes a copy (e.g. autosave) and keeps the current path and unsaved state.",
           objectSchema(
               {{"path",
                 {{"type", "string"},
                  {"minLength", 1},
                  {"description", "Target file (absolute); .aerocad is appended when there is no extension."}}},
                {"copy", {{"type", "boolean"}, {"description", "Save a copy without changing the current path."}}},
                {"view", {{"type", "object"}, {"description", "View state (camera) to store; set by the UI."}}}}),
           false},
          [](CommandContext& ctx, const Json& params) {
              ProjectState& p = projectState(ctx);
              const bool copy = params.value("copy", false);
              const std::string target = params.value("path", copy ? std::string() : p.path);
              if (target.empty())
                  throw CommandError(copy ? "project.save copy needs a path"
                                          : "The project has no file yet: give a path (Save As)");
              const std::filesystem::path path = withExtension(target);
              project::ProjectData data = snapshot(ctx);
              if (params.contains("view")) {
                  data.view = params.at("view");
                  storeDimensions(ctx, data.view);
              }
              if (copy)
                  data.originalPath = p.path;
              std::string bytes;
              try {
                  bytes = project::encodeProject(data);
                  project::writeFileAtomically(path, bytes);
              } catch (const project::ProjectError& e) {
                  throw CommandError(e.what());
              }
              if (!copy) {
                  p.path = core::pathToUtf8(path);
                  p.view = data.view;
                  p.savedChanges = p.changes;
                  p.recovered = false;
              }
              Json out = projectInfo(ctx);
              out["savedTo"] = core::pathToUtf8(path);
              out["bytes"] = bytes.size();
              out["copy"] = copy;
              return out;
          });

    r.add(
        {"project.open", "Open",
         "Opens a .aerocad project, replacing the current one (unsaved changes are lost: check project.info "
         "dirty first). Bodies and sketches are regenerated from the timeline. recover: true opens an autosave "
         "copy: the project keeps the original file path and stays unsaved.",
         objectSchema({{"path", {{"type", "string"}, {"minLength", 1}, {"description", "Absolute path of the file."}}},
                       {"recover", {{"type", "boolean"}, {"description", "Open an autosave copy."}}}},
                      {"path"}),
         true},
        [](CommandContext& ctx, const Json& params) {
            const std::filesystem::path path = core::pathFromUtf8(params.at("path").get<std::string>());
            project::ProjectData data;
            try {
                data = project::loadProject(path); // fails before touching the session
            } catch (const project::ProjectError& e) {
                throw CommandError(e.what());
            }
            const int version = data.formatVersion;
            const std::string generator = data.generator;
            const std::string original = data.originalPath;
            ProjectState& p = projectState(ctx);
            applyProject(ctx, std::move(data));
            markClean(p);
            if (params.value("recover", false)) {
                p.path = original;
                p.recovered = true;
            } else {
                p.path = core::pathToUtf8(std::filesystem::absolute(path));
            }
            Json out = projectInfo(ctx);
            out["view"] = p.view;
            out["fileVersion"] = version;
            out["generator"] = generator;
            Json errors = Json::array();
            for (const auto& [id, st] : ctx.design->statuses()) {
                if (st.state == cad::FeatureStatus::State::Error)
                    errors.push_back({{"feature", id}, {"message", st.message}});
            }
            out["featureErrors"] = errors;
            return out;
        });
}

// --- Variants ------------------------------------------------------------------------------

Json variantRefSchema(const std::string& description)
{
    return Json{{"type", Json::array({"integer", "string"})},
                {"description", description + " Variant id or name (see variant.list)."}};
}

const cad::Variant& requireVariant(const ProjectState& p, const Json& ref)
{
    const cad::Variant* v =
        ref.is_string() ? p.variants.findByName(ref.get<std::string>()) : p.variants.find(ref.get<cad::VariantId>());
    if (v == nullptr)
        throw CommandError("No variant " + (ref.is_string() ? "'" + ref.get<std::string>() + "'" : ref.dump()));
    return *v;
}

Json variantsToJson(CommandContext& ctx)
{
    const ProjectState& p = projectState(ctx);
    Json items = Json::array();
    for (const auto& v : p.variants.variants()) {
        const cad::Design::State st = p.variants.stateOf(v.id, ctx.design->state());
        Json params = Json::array();
        for (const auto& par : st.parameters)
            params.push_back({{"name", par.name}, {"expression", par.expression}});
        items.push_back({{"id", v.id},
                         {"name", v.name},
                         {"active", v.id == p.variants.active()},
                         {"features", st.features.size()},
                         {"parameters", params}});
    }
    return Json{{"variants", items}, {"active", p.variants.active()}, {"previous", p.variants.previous()}};
}

/// Runs a variant operation as one undoable step: undo restores both the variant set and
/// the live design state, then mirrors the design into the document.
template <typename Fn> void variantChange(CommandContext& ctx, const std::string& label, Fn&& fn)
{
    ProjectState& p = projectState(ctx);
    cad::Design& d = *ctx.design;
    const cad::VariantSet beforeVariants = p.variants;
    const cad::Design::State beforeDesign = d.state();
    try {
        fn(p.variants, d);
    } catch (const cad::DesignError& e) {
        p.variants = beforeVariants;
        throw CommandError(e.what());
    }
    model::Document& doc = ctx.document;
    d.syncDocument(doc);
    const cad::VariantSet afterVariants = p.variants;
    const cad::Design::State afterDesign = d.state();
    ProjectState* pp = &p;
    cad::Design* dp = &d;
    ctx.undoStack.push({label,
                        [pp, dp, &doc, beforeVariants, beforeDesign]() {
                            pp->variants = beforeVariants;
                            dp->restore(beforeDesign);
                            dp->syncDocument(doc);
                        },
                        [pp, dp, &doc, afterVariants, afterDesign]() {
                            pp->variants = afterVariants;
                            dp->restore(afterDesign);
                            dp->syncDocument(doc);
                        }});
}

void activate(cad::VariantSet& vs, cad::Design& d, cad::VariantId id)
{
    if (id != vs.active())
        d.restore(vs.activate(id, d.state()));
}

/// Differences between two design states, for variant.compare.
Json compareStates(const cad::Design::State& a, const cad::Design::State& b)
{
    std::map<std::string, std::string> pa;
    std::map<std::string, std::string> pb;
    for (const auto& p : a.parameters)
        pa[p.name] = p.expression;
    for (const auto& p : b.parameters)
        pb[p.name] = p.expression;
    std::set<std::string> names;
    for (const auto& [n, _] : pa)
        names.insert(n);
    for (const auto& [n, _] : pb)
        names.insert(n);
    Json params = Json::array();
    for (const auto& n : names) {
        const auto ia = pa.find(n);
        const auto ib = pb.find(n);
        const Json ea = ia == pa.end() ? Json(nullptr) : Json(ia->second);
        const Json eb = ib == pb.end() ? Json(nullptr) : Json(ib->second);
        if (ea != eb)
            params.push_back({{"name", n}, {"a", ea}, {"b", eb}});
    }

    std::map<cad::FeatureId, const cad::Feature*> fa;
    std::map<cad::FeatureId, const cad::Feature*> fb;
    for (const auto& f : a.features)
        fa[f.id] = &f;
    for (const auto& f : b.features)
        fb[f.id] = &f;
    Json features = Json::array();
    for (const auto& [id, f] : fa) {
        auto it = fb.find(id);
        if (it == fb.end()) {
            features.push_back({{"id", id}, {"name", f->name}, {"change", "only_in_a"}});
        } else if (f->params != it->second->params || f->suppressed != it->second->suppressed ||
                   f->name != it->second->name) {
            features.push_back({{"id", id},
                                {"name", f->name},
                                {"change", "modified"},
                                {"patch", Json::diff(f->params, it->second->params)},
                                {"suppressed", {{"a", f->suppressed}, {"b", it->second->suppressed}}}});
        }
    }
    for (const auto& [id, f] : fb) {
        if (!fa.contains(id))
            features.push_back({{"id", id}, {"name", f->name}, {"change", "only_in_b"}});
    }
    return Json{{"parameters", params}, {"features", features}};
}

void registerVariantCommands(CommandRegistry& r)
{
    r.add({"variant.list", "Variants",
           "Lists the design variants (A/B comparison) with their user parameters. One variant is active; "
           "edits (feature.*, parameter.*, sketch.*) apply to it. Imported parts are shared by all variants.",
           objectSchema(Json::object())},
          [](CommandContext& ctx, const Json&) { return variantsToJson(ctx); });

    r.add({"variant.create", "Duplicate as Variant",
           "Duplicates the design (timeline, parameters, frames) of the active variant, or of `from`, as a new "
           "variant (default name: the next free letter, B, C, ...) and activates it unless activate is false. "
           "Then change parameters or features and switch with variant.activate / variant.toggle; the camera is "
           "not moved.",
           objectSchema({{"name", {{"type", "string"}, {"minLength", 1}}},
                         {"from", variantRefSchema("Variant to copy; default the active one.")},
                         {"activate", {{"type", "boolean"}, {"description", "Default true."}}}}),
           true},
          [](CommandContext& ctx, const Json& params) {
              ProjectState& p = projectState(ctx);
              const cad::VariantId from =
                  params.contains("from") ? requireVariant(p, params.at("from")).id : p.variants.active();
              cad::VariantId id = 0;
              variantChange(ctx, "Duplicate Variant", [&](cad::VariantSet& vs, cad::Design& d) {
                  id = vs.add(params.value("name", std::string()), vs.stateOf(from, d.state()));
                  if (params.value("activate", true))
                      activate(vs, d, id);
              });
              Json out = variantsToJson(ctx);
              out["created"] = id;
              return out;
          });

    r.add({"variant.activate", "Activate Variant",
           "Switches the design to another variant. Parts of features that exist in both keep their ids, so "
           "views update in place and the camera stays.",
           objectSchema({{"variant", variantRefSchema("Variant to activate.")}}, {"variant"}), true},
          [](CommandContext& ctx, const Json& params) {
              ProjectState& p = projectState(ctx);
              const cad::Variant& v = requireVariant(p, params.at("variant"));
              const cad::VariantId id = v.id;
              variantChange(ctx, "Activate Variant " + v.name,
                            [&](cad::VariantSet& vs, cad::Design& d) { activate(vs, d, id); });
              return variantsToJson(ctx);
          });

    r.add({"variant.toggle", "Toggle A/B",
           "Switches back to the previously active variant (A/B toggle). With two variants and no history, "
           "switches to the other one.",
           objectSchema(Json::object()), true},
          [](CommandContext& ctx, const Json&) {
              ProjectState& p = projectState(ctx);
              cad::VariantId target = p.variants.previous();
              if (target == 0 && p.variants.size() == 2) {
                  for (const auto& v : p.variants.variants())
                      if (v.id != p.variants.active())
                          target = v.id;
              }
              if (target == 0)
                  throw CommandError("No other variant to switch to; create one with variant.create");
              const std::string name = p.variants.find(target)->name;
              variantChange(ctx, "Activate Variant " + name,
                            [&](cad::VariantSet& vs, cad::Design& d) { activate(vs, d, target); });
              return variantsToJson(ctx);
          });

    r.add({"variant.rename", "Rename Variant", "Renames a variant.",
           objectSchema({{"variant", variantRefSchema("Variant.")}, {"name", {{"type", "string"}, {"minLength", 1}}}},
                        {"variant", "name"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const cad::VariantId id = requireVariant(projectState(ctx), params.at("variant")).id;
              variantChange(ctx, "Rename Variant", [&](cad::VariantSet& vs, cad::Design&) {
                  vs.rename(id, params.at("name").get<std::string>());
              });
              return variantsToJson(ctx);
          });

    r.add({"variant.delete", "Delete Variant", "Deletes an inactive variant.",
           objectSchema({{"variant", variantRefSchema("Variant.")}}, {"variant"}), true},
          [](CommandContext& ctx, const Json& params) {
              const cad::Variant& v = requireVariant(projectState(ctx), params.at("variant"));
              const cad::VariantId id = v.id;
              variantChange(ctx, "Delete Variant " + v.name, [&](cad::VariantSet& vs, cad::Design&) { vs.remove(id); });
              return variantsToJson(ctx);
          });

    r.add({"variant.compare", "Compare Variants",
           "Lists what differs between two variants: user parameters (expressions) and features (only in one, "
           "or modified with a JSON patch from a to b). Defaults: a = previous (or first other) variant, "
           "b = active.",
           objectSchema({{"a", variantRefSchema("First variant.")}, {"b", variantRefSchema("Second variant.")}})},
          [](CommandContext& ctx, const Json& params) {
              const ProjectState& p = projectState(ctx);
              const cad::VariantId active = p.variants.active();
              cad::VariantId a = p.variants.previous();
              if (a == 0) {
                  for (const auto& v : p.variants.variants())
                      if (v.id != active) {
                          a = v.id;
                          break;
                      }
              }
              if (params.contains("a"))
                  a = requireVariant(p, params.at("a")).id;
              const cad::VariantId b = params.contains("b") ? requireVariant(p, params.at("b")).id : active;
              if (a == 0)
                  throw CommandError("Only one variant; create one with variant.create");
              const cad::Design::State& live = ctx.design->state();
              Json out = compareStates(p.variants.stateOf(a, live), p.variants.stateOf(b, live));
              out["a"] = {{"id", a}, {"name", p.variants.find(a)->name}};
              out["b"] = {{"id", b}, {"name", p.variants.find(b)->name}};
              return out;
          });
}

} // namespace

void registerProjectCommands(CommandRegistry& registry)
{
    registerFileCommands(registry);
    registerVariantCommands(registry);
}

} // namespace aerocad::commands
