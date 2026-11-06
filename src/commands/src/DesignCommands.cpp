// SPDX-License-Identifier: GPL-3.0-or-later
// CAD commands: sketches, features and the parametric timeline. Each command edits the
// cad::Design, which regenerates and is mirrored into the document as parts. Undo
// restores the previous timeline state.
#include "aerocad/commands/Session.hpp"

#include "aerocad/geometry/Modeling.hpp"

#include <string>
#include <utility>

namespace aerocad::commands {

namespace {

// --- Schema helpers ---------------------------------------------------------------------

Json objectSchema(Json properties, Json required = Json::array())
{
    return Json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)},
                {"additionalProperties", false}};
}

Json numberOrExpr(const std::string& description)
{
    return Json{{"type", Json::array({"number", "string"})},
                {"description", description + " Number or expression with user parameters, e.g. \"width / 2\"."}};
}

Json point2(const std::string& description)
{
    return Json{{"type", "array"},
                {"items", {{"type", Json::array({"number", "string"})}}},
                {"minItems", 2},
                {"maxItems", 2},
                {"description", description + " [u, v] in sketch coordinates (mm)."}};
}

Json point3(const std::string& description)
{
    return Json{{"type", "array"},
                {"items", {{"type", Json::array({"number", "string"})}}},
                {"minItems", 3},
                {"maxItems", 3},
                {"description", description + " [x, y, z] in mm."}};
}

Json featureIdSchema(const std::string& description)
{
    return Json{{"type", "integer"}, {"minimum", 1}, {"description", description}};
}

Json bodyRefSchema(const std::string& description)
{
    return Json{{"type", Json::array({"integer", "string"})},
                {"description", description + " Body id (the id of the feature that created it, see "
                                              "timeline.list / document.parts featureId) or body name."}};
}

Json curveSchema()
{
    return Json{
        {"type", "object"},
        {"description",
         "A sketch curve. type selects the fields: line {start, end}; rectangle {corner, width, height} or "
         "{corner, opposite}; center_rectangle {center, width, height}; circle {center, radius | diameter}; "
         "arc {start, through, end} (three-point arc); center_arc {center, radius, startAngle, endAngle} "
         "(degrees, counter-clockwise); polygon {center, radius, sides, angle?}; slot {start, end, width} "
         "(centre-to-centre); spline {points, closed?}. construction: true excludes it from profiles."},
        {"properties",
         {{"type",
           {{"type", "string"},
            {"enum", {"line", "rectangle", "center_rectangle", "circle", "arc", "center_arc", "polygon", "slot",
                      "spline"}}}},
          {"start", point2("Start point.")},
          {"end", point2("End point.")},
          {"through", point2("Point on the arc.")},
          {"corner", point2("Rectangle corner.")},
          {"opposite", point2("Opposite rectangle corner.")},
          {"center", point2("Centre.")},
          {"width", numberOrExpr("Width (mm).")},
          {"height", numberOrExpr("Height (mm).")},
          {"radius", numberOrExpr("Radius (mm).")},
          {"diameter", numberOrExpr("Diameter (mm).")},
          {"startAngle", numberOrExpr("Start angle (degrees).")},
          {"endAngle", numberOrExpr("End angle (degrees).")},
          {"sides", numberOrExpr("Number of polygon sides.")},
          {"angle", numberOrExpr("Polygon rotation (degrees).")},
          {"points", {{"type", "array"}, {"items", point2("Fit point.")}, {"minItems", 2}}},
          {"closed", {{"type", "boolean"}}},
          {"construction", {{"type", "boolean"}}}}},
        {"required", {"type"}},
        {"additionalProperties", false}};
}

Json curvesSchema()
{
    return Json{{"type", "array"}, {"items", curveSchema()}, {"description", "Sketch curves."}};
}

Json planeSchema()
{
    return Json{{"type", Json::array({"string", "object"})},
                {"description",
                 "Sketch plane: \"XY\" (top, normal +Z), \"XZ\" (front, normal -Y), \"YZ\" (right, normal +X), "
                 "or {origin: [x,y,z], normal: [x,y,z], xDir: [x,y,z]}. Z is up."}};
}

Json faceRefSchema()
{
    return Json{{"type", "object"},
                {"description", "Planar face of a body to sketch on: {body, index} or {body, point: [x,y,z]} "
                                "(the face nearest to the point; robust when earlier features change)."},
                {"properties", {{"body", bodyRefSchema("Body.")}, {"index", {{"type", "integer"}, {"minimum", 0}}},
                                {"point", point3("Point on the face.")}}},
                {"required", {"body"}},
                {"additionalProperties", false}};
}

Json operationSchema(bool withNewBody = true)
{
    Json values = withNewBody ? Json::array({"new_body", "join", "cut", "intersect"})
                              : Json::array({"join", "cut", "intersect"});
    return Json{{"type", "string"},
                {"enum", values},
                {"description", "What to do with the result (Fusion's Operation): new_body (default), join, cut or "
                                "intersect. join/cut/intersect act on `bodies`, or on all bodies the result "
                                "touches."}};
}

Json bodiesSchema()
{
    return Json{{"type", "array"},
                {"items", bodyRefSchema("Body.")},
                {"description", "Target bodies for join/cut/intersect. Default: every body the result touches."}};
}

Json topologyListSchema(const std::string& what)
{
    return Json{{"type", "array"},
                {"items", {{"type", Json::array({"integer", "object"})}}},
                {"minItems", 1},
                {"description", what + " to use: indices from body." + what + "s, or {point: [x,y,z]} to take the " +
                                    what + " nearest to a point (stays valid when earlier features change)."}};
}

Json profileSelection()
{
    return Json{{"sketch", featureIdSchema("Sketch feature id.")},
                {"profiles", {{"type", "array"},
                              {"items", {{"type", "integer"}, {"minimum", 0}}},
                              {"description", "Profile indices (see sketch.profiles). Default: all profiles."}}},
                {"points", {{"type", "array"},
                            {"items", point2("Point inside the profile.")},
                            {"description", "Select profiles by a point inside them (alternative to profiles)."}}}};
}

Json merged(Json a, const Json& b)
{
    for (const auto& [k, v] : b.items())
        a[k] = v;
    return a;
}

// --- Result helpers ---------------------------------------------------------------------

cad::Design& design(CommandContext& ctx)
{
    if (ctx.design == nullptr)
        throw CommandError("No design in this session");
    return *ctx.design;
}

model::PartId partOf(const model::Document& doc, cad::FeatureId id, model::PartKind kind)
{
    for (const auto& p : doc.parts()) {
        if (p.kind == kind && p.featureId == id)
            return p.id;
    }
    return 0;
}

Json vec(const core::Vec3& v)
{
    return Json::array({v.x, v.y, v.z});
}

Json featureToJson(const cad::Design& d, const cad::Feature& f, std::size_t index)
{
    const auto st = d.status(f.id);
    Json j{{"id", f.id},
           {"index", index},
           {"type", f.type},
           {"name", f.name},
           {"params", f.params},
           {"suppressed", f.suppressed},
           {"status", std::string(cad::toString(st.state))}};
    if (!st.message.empty())
        j["error"] = st.message;
    return j;
}

Json bodiesToJson(const cad::Design& d, const model::Document& doc)
{
    Json out = Json::array();
    for (const auto& [id, b] : d.bodies()) {
        const auto bb = b.shape.bounds();
        out.push_back({{"id", id},
                       {"name", b.name},
                       {"partId", partOf(doc, id, model::PartKind::Body)},
                       {"volume", b.shape.volume()},
                       {"faces", b.shape.faceCount()},
                       {"bounds", {{"min", vec(bb.min)}, {"max", vec(bb.max)}}}});
    }
    return out;
}

Json timelineToJson(const cad::Design& d, const model::Document& doc)
{
    Json features = Json::array();
    for (std::size_t i = 0; i < d.features().size(); ++i)
        features.push_back(featureToJson(d, d.features()[i], i));
    Json params = Json::array();
    for (const auto& p : d.parameters()) {
        Json pj{{"name", p.name}, {"expression", p.expression}, {"comment", p.comment}};
        try {
            pj["value"] = *d.parameterValue(p.name);
        } catch (const std::exception& e) {
            pj["error"] = e.what();
        }
        params.push_back(pj);
    }
    return Json{{"features", features},
                {"marker", d.marker()},
                {"parameters", params},
                {"bodies", bodiesToJson(d, doc)}};
}

/// Runs a design change as one undoable step and mirrors the result into the document.
template <typename Fn>
Json designChange(CommandContext& ctx, const std::string& label, Fn&& fn)
{
    cad::Design& d = design(ctx);
    const cad::Design::State before = d.state();
    Json result;
    try {
        result = fn(d);
    } catch (const cad::DesignError& e) {
        throw CommandError(e.what());
    }
    const cad::Design::State after = d.state();
    model::Document& doc = ctx.document;
    d.syncDocument(doc);
    cad::Design* dp = &d;
    ctx.undoStack.push({label,
                        [dp, &doc, before]() {
                            dp->restore(before);
                            dp->syncDocument(doc);
                        },
                        [dp, &doc, after]() {
                            dp->restore(after);
                            dp->syncDocument(doc);
                        }});
    return result;
}

/// Result of commands that add a feature.
Json addedFeature(cad::Design& d, const model::Document& doc, cad::FeatureId id)
{
    const cad::Feature* f = d.feature(id);
    std::size_t index = 0;
    for (; index < d.features().size(); ++index)
        if (d.features()[index].id == id)
            break;
    Json j = featureToJson(d, *f, index);
    j["bodies"] = bodiesToJson(d, doc);
    if (auto it = d.sketches().find(id); it != d.sketches().end())
        j["profiles"] = it->second.geometry.profiles.size();
    return j;
}

/// Copies the command params into feature params, without the command-only keys.
Json featureParams(const Json& params)
{
    Json p = params;
    p.erase("name");
    return p;
}

void addFeatureCommand(CommandRegistry& r, const std::string& type, const std::string& title,
                       const std::string& description, Json properties, Json required)
{
    properties["name"] = {{"type", "string"}, {"description", "Optional feature name for the timeline."}};
    r.add({"feature." + type, title, description, objectSchema(std::move(properties), std::move(required)), true},
          [type, title](CommandContext& ctx, const Json& params) {
              cad::FeatureId id = 0;
              designChange(ctx, title, [&](cad::Design& d) {
                  id = d.addFeature(type, featureParams(params), params.value("name", std::string()));
                  return Json{};
              });
              // The document is synced only after designChange; build the result now.
              return addedFeature(design(ctx), ctx.document, id);
          });
}

const cad::Body& requireBody(const cad::Design& d, const Json& ref)
{
    if (ref.is_number_integer()) {
        if (auto it = d.bodies().find(ref.get<cad::FeatureId>()); it != d.bodies().end())
            return it->second;
    } else if (ref.is_string()) {
        for (const auto& [_, b] : d.bodies())
            if (b.name == ref.get<std::string>())
                return b;
    }
    throw CommandError("No body " + ref.dump());
}

// --- Registration -----------------------------------------------------------------------

void registerSketchCommands(CommandRegistry& r)
{
    r.add({"sketch.create", "Create Sketch",
           "Creates a 2D sketch on a standard plane, an arbitrary plane or a planar face of a body, with curves "
           "(lines, rectangles, circles, arcs, polygons, slots, splines). Closed regions become profiles for "
           "extrude/revolve. Dimensions can be expressions using user parameters (parameter.set), so editing a "
           "parameter updates the model.",
           objectSchema({{"plane", planeSchema()},
                         {"face", faceRefSchema()},
                         {"offset", numberOrExpr("Offset of the plane along its normal (mm).")},
                         {"curves", curvesSchema()},
                         {"name", {{"type", "string"}, {"description", "Optional name, default SketchN."}}}}),
           true},
          [](CommandContext& ctx, const Json& params) {
              if (params.contains("plane") && params.contains("face"))
                  throw CommandError("Give either plane or face, not both");
              cad::FeatureId id = 0;
              designChange(ctx, "Create Sketch", [&](cad::Design& d) {
                  id = d.addFeature("sketch", featureParams(params), params.value("name", std::string()));
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.add_curves", "Add Sketch Curves", "Appends curves to an existing sketch.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")}, {"curves", curvesSchema()}},
                        {"sketch", "curves"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              designChange(ctx, "Edit Sketch", [&](cad::Design& d) {
                  const cad::Feature* f = d.feature(id);
                  if (f == nullptr || f->type != "sketch")
                      throw cad::DesignError("No sketch with id " + std::to_string(id));
                  Json curves = f->params.value("curves", Json::array());
                  for (const auto& c : params.at("curves"))
                      curves.push_back(c);
                  d.editFeature(id, {{"curves", curves}});
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"sketch.profiles", "Sketch profiles",
           "Lists the closed profiles of a sketch with their area and centroid (sketch and world coordinates), "
           "to pick profiles for extrude/revolve.",
           objectSchema({{"sketch", featureIdSchema("Sketch feature id.")}}, {"sketch"})},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("sketch").get<cad::FeatureId>();
              const auto& sketches = design(ctx).sketches();
              auto it = sketches.find(id);
              if (it == sketches.end())
                  throw CommandError("No evaluated sketch with id " + std::to_string(id));
              const auto& sk = it->second;
              Json out = Json::array();
              for (std::size_t i = 0; i < sk.geometry.profiles.size(); ++i) {
                  const auto c = geometry::centroid(sk.geometry.profiles[i]);
                  const auto uv = sk.plane.toLocal(c);
                  out.push_back({{"index", i},
                                 {"area", geometry::area(sk.geometry.profiles[i])},
                                 {"centroid", Json::array({uv.x, uv.y})},
                                 {"centroidWorld", vec(c)}});
              }
              return Json{{"sketch", id},
                          {"plane", {{"origin", vec(sk.plane.origin)},
                                     {"normal", vec(sk.plane.normal)},
                                     {"xDir", vec(sk.plane.xDir)},
                                     {"yDir", vec(sk.plane.yDir())}}},
                          {"curves", sk.curves.size()},
                          {"profiles", out}};
          });
}

void registerFeatureCommands(CommandRegistry& r)
{
    addFeatureCommand(
        r, "extrude", "Extrude",
        "Extrudes sketch profiles along the sketch normal (Fusion: Extrude, shortcut E). Creates a new body or "
        "joins / cuts / intersects existing bodies.",
        merged(profileSelection(),
               {{"distance", numberOrExpr("Distance along the sketch normal (mm); negative goes the other way.")},
                {"extent", {{"type", "string"},
                            {"enum", {"one_side", "symmetric", "two_sides"}},
                            {"description", "one_side (default); symmetric: `distance` on both sides; two_sides: "
                                            "`distance` forwards and `distance2` backwards."}}},
                {"distance2", numberOrExpr("Backward distance for two_sides (mm).")},
                {"flip", {{"type", "boolean"}, {"description", "Extrude against the sketch normal."}}},
                {"operation", operationSchema()},
                {"bodies", bodiesSchema()}}),
        {"sketch", "distance"});

    addFeatureCommand(
        r, "revolve", "Revolve",
        "Revolves sketch profiles around an axis (Fusion: Revolve).",
        merged(profileSelection(),
               {{"axis", {{"type", Json::array({"string", "object"})},
                          {"description", "\"X\", \"Y\", \"Z\" (world axes through the origin, default Z), "
                                          "{line: curve index of a sketch line} or {origin: [x,y,z], direction: "
                                          "[x,y,z]}."}}},
                {"angle", numberOrExpr("Angle in degrees, default 360.")},
                {"operation", operationSchema()},
                {"bodies", bodiesSchema()}}),
        {"sketch"});

    addFeatureCommand(r, "fillet", "Fillet", "Rounds edges of a body with a constant radius (Fusion: Fillet, shortcut F).",
                      {{"body", bodyRefSchema("Body.")},
                       {"edges", topologyListSchema("edge")},
                       {"radius", numberOrExpr("Radius (mm).")}},
                      {"body", "edges", "radius"});

    addFeatureCommand(r, "chamfer", "Chamfer", "Bevels edges of a body with an equal distance (Fusion: Chamfer).",
                      {{"body", bodyRefSchema("Body.")},
                       {"edges", topologyListSchema("edge")},
                       {"distance", numberOrExpr("Distance (mm).")}},
                      {"body", "edges", "distance"});

    addFeatureCommand(r, "shell", "Shell",
                      "Hollows a body, removing the selected faces and keeping inside walls of the given thickness.",
                      {{"body", bodyRefSchema("Body.")},
                       {"faces", topologyListSchema("face")},
                       {"thickness", numberOrExpr("Wall thickness (mm).")}},
                      {"body", "thickness"});

    addFeatureCommand(r, "combine", "Combine",
                      "Boolean between bodies (Fusion: Combine): joins, cuts or intersects the target with the tool "
                      "bodies.",
                      {{"target", bodyRefSchema("Target body.")},
                       {"tools", {{"type", "array"}, {"items", bodyRefSchema("Tool body.")}, {"minItems", 1}}},
                       {"operation", operationSchema(false)},
                       {"keepTools", {{"type", "boolean"}, {"description", "Keep the tool bodies (default false)."}}}},
                      {"target", "tools"});

    addFeatureCommand(r, "box", "Box", "Creates an axis-aligned box (Fusion: Box).",
                      {{"corner", point3("Minimum corner, default origin.")},
                       {"size", point3("Size along X, Y, Z.")},
                       {"operation", operationSchema()},
                       {"bodies", bodiesSchema()}},
                      {"size"});

    addFeatureCommand(r, "cylinder", "Cylinder", "Creates a cylinder along +Z (Fusion: Cylinder).",
                      {{"center", point3("Centre of the base, default origin.")},
                       {"radius", numberOrExpr("Radius (mm).")},
                       {"diameter", numberOrExpr("Diameter (mm), alternative to radius.")},
                       {"height", numberOrExpr("Height (mm).")},
                       {"operation", operationSchema()},
                       {"bodies", bodiesSchema()}},
                      {"height"});

    addFeatureCommand(r, "hole", "Hole",
                      "Drills a round hole into a body, normal to the planar face at `point` (Fusion: Hole, "
                      "shortcut H). Through all unless `depth` is given.",
                      {{"body", bodyRefSchema("Body.")},
                       {"point", point3("Hole centre on a planar face.")},
                       {"diameter", numberOrExpr("Diameter (mm).")},
                       {"depth", numberOrExpr("Depth (mm); omit for through all.")},
                       {"through", {{"type", "boolean"}, {"description", "Through all (default when no depth)."}}}},
                      {"body", "point", "diameter"});

    r.add({"feature.base_body", "Convert to Body",
           "Turns an imported part into a timeline body so it can be sketched on, cut, joined, filleted "
           "(Fusion: base feature / Mesh to BRep). B-rep imports (STEP, IGES, BREP) are used as they are; "
           "meshes (STL, OBJ, 3MF) become faceted bodies with coplanar triangles merged into planar faces "
           "(up to 50 000 triangles). The imported part is hidden.",
           objectSchema({{"part", {{"type", "integer"}, {"minimum", 1}, {"description", "Imported part id (document.parts)."}}},
                         {"name", {{"type", "string"}, {"description", "Optional feature name."}}}},
                        {"part"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto partId = params.at("part").get<model::PartId>();
              model::Document& doc = ctx.document;
              const model::Part* source = doc.part(partId);
              if (source == nullptr)
                  throw CommandError("No part with id " + std::to_string(partId));
              cad::Design& d = design(ctx);
              const cad::Design::State before = d.state();
              cad::FeatureId id = 0;
              try {
                  id = d.addFeature("base_body", featureParams(params), params.value("name", std::string()));
              } catch (const cad::DesignError& e) {
                  throw CommandError(e.what());
              }
              const cad::Design::State after = d.state();
              const bool wasVisible = source->visible;
              doc.modifyPart(partId, [](model::Part& p) { p.visible = false; });
              d.syncDocument(doc);
              cad::Design* dp = &d;
              ctx.undoStack.push({"Convert to Body",
                                  [dp, &doc, before, partId, wasVisible]() {
                                      dp->restore(before);
                                      dp->syncDocument(doc);
                                      doc.modifyPart(partId, [&](model::Part& p) { p.visible = wasVisible; });
                                  },
                                  [dp, &doc, after, partId]() {
                                      doc.modifyPart(partId, [](model::Part& p) { p.visible = false; });
                                      dp->restore(after);
                                      dp->syncDocument(doc);
                                  }});
              return addedFeature(d, doc, id);
          });

    r.add({"feature.edit", "Edit Feature",
           "Changes parameters of a feature in the timeline (JSON merge patch: given keys replace, null removes) "
           "and regenerates the model. Fails without changes if the feature would fail.",
           objectSchema({{"id", featureIdSchema("Feature id.")},
                         {"params", {{"type", "object"}, {"description", "Parameters to change."}}}},
                        {"id", "params"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("id").get<cad::FeatureId>();
              designChange(ctx, "Edit Feature", [&](cad::Design& d) {
                  d.editFeature(id, params.at("params"));
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"feature.rename", "Rename Feature", "Renames a timeline feature.",
           objectSchema({{"id", featureIdSchema("Feature id.")}, {"name", {{"type", "string"}, {"minLength", 1}}}},
                        {"id", "name"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("id").get<cad::FeatureId>();
              designChange(ctx, "Rename Feature", [&](cad::Design& d) {
                  d.renameFeature(id, params.at("name").get<std::string>());
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"feature.suppress", "Suppress Feature",
           "Suppresses (skips) or unsuppresses a feature; the model regenerates without it.",
           objectSchema({{"id", featureIdSchema("Feature id.")}, {"suppressed", {{"type", "boolean"}}}},
                        {"id", "suppressed"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("id").get<cad::FeatureId>();
              const bool s = params.at("suppressed").get<bool>();
              designChange(ctx, s ? "Suppress Feature" : "Unsuppress Feature", [&](cad::Design& d) {
                  d.setSuppressed(id, s);
                  return Json{};
              });
              return addedFeature(design(ctx), ctx.document, id);
          });

    r.add({"feature.delete", "Delete Feature",
           "Deletes a feature from the timeline. Features that depended on it report errors (see timeline.list).",
           objectSchema({{"id", featureIdSchema("Feature id.")}}, {"id"}), true},
          [](CommandContext& ctx, const Json& params) {
              const auto id = params.at("id").get<cad::FeatureId>();
              designChange(ctx, "Delete Feature", [&](cad::Design& d) {
                  d.removeFeature(id);
                  return Json{};
              });
              return timelineToJson(design(ctx), ctx.document);
          });
}

void registerTimelineCommands(CommandRegistry& r)
{
    r.add({"timeline.list", "Timeline",
           "Lists the design timeline: features in order with parameters and status (ok, error, suppressed, "
           "rolled_back), the marker position, user parameters and the resulting bodies.",
           objectSchema(Json::object())},
          [](CommandContext& ctx, const Json&) { return timelineToJson(design(ctx), ctx.document); });

    r.add({"timeline.set_marker", "Move Timeline Marker",
           "Moves the timeline marker (Fusion: roll back). Features after it are not evaluated and new features "
           "are inserted at it. 0 = before the first feature; omit position to move to the end.",
           objectSchema({{"position", {{"type", "integer"}, {"minimum", 0}}}}), true},
          [](CommandContext& ctx, const Json& params) {
              designChange(ctx, "Move Timeline Marker", [&](cad::Design& d) {
                  d.setMarker(params.contains("position") ? params.at("position").get<std::size_t>()
                                                          : d.features().size());
                  return Json{};
              });
              return timelineToJson(design(ctx), ctx.document);
          });

    r.add({"parameter.set", "Set Parameter",
           "Creates or changes a user parameter (Fusion: Change Parameters). The expression may use other "
           "parameters and units (mm, cm, m, in, deg). Features using it regenerate.",
           objectSchema({{"name", {{"type", "string"}, {"minLength", 1}}},
                         {"expression", {{"type", Json::array({"string", "number"})}}},
                         {"comment", {{"type", "string"}}}},
                        {"name", "expression"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              const auto name = params.at("name").get<std::string>();
              const Json& e = params.at("expression");
              const std::string expr = e.is_string() ? e.get<std::string>() : e.dump();
              designChange(ctx, "Set Parameter " + name, [&](cad::Design& d) {
                  d.setParameter(name, expr, params.value("comment", std::string()));
                  return Json{};
              });
              return timelineToJson(design(ctx), ctx.document);
          });

    r.add({"parameter.remove", "Remove Parameter", "Removes a user parameter.",
           objectSchema({{"name", {{"type", "string"}, {"minLength", 1}}}}, {"name"}), true},
          [](CommandContext& ctx, const Json& params) {
              const auto name = params.at("name").get<std::string>();
              designChange(ctx, "Remove Parameter " + name, [&](cad::Design& d) {
                  d.removeParameter(name);
                  return Json{};
              });
              return timelineToJson(design(ctx), ctx.document);
          });
}

void registerBodyCommands(CommandRegistry& r)
{
    r.add({"body.faces", "Body faces",
           "Lists the faces of a body: index, surface type, area, centre and outward normal. Use the index or "
           "the centre as {point} in sketch.create face, feature.shell faces.",
           objectSchema({{"body", bodyRefSchema("Body.")}}, {"body"})},
          [](CommandContext& ctx, const Json& params) {
              const cad::Body& b = requireBody(design(ctx), params.at("body"));
              Json out = Json::array();
              for (const auto& f : geometry::faces(b.shape)) {
                  out.push_back({{"index", f.index},
                                 {"type", f.type},
                                 {"area", f.area},
                                 {"center", vec(f.center)},
                                 {"normal", vec(f.normal)}});
              }
              return Json{{"body", b.id}, {"name", b.name}, {"faces", out}};
          });

    r.add({"body.edges", "Body edges",
           "Lists the edges of a body: index, curve type, length, start, middle and end points. Use the index "
           "or the middle as {point} in feature.fillet / feature.chamfer edges.",
           objectSchema({{"body", bodyRefSchema("Body.")}}, {"body"})},
          [](CommandContext& ctx, const Json& params) {
              const cad::Body& b = requireBody(design(ctx), params.at("body"));
              Json out = Json::array();
              for (const auto& e : geometry::edges(b.shape)) {
                  out.push_back({{"index", e.index},
                                 {"type", e.type},
                                 {"length", e.length},
                                 {"start", vec(e.start)},
                                 {"middle", vec(e.middle)},
                                 {"end", vec(e.end)}});
              }
              return Json{{"body", b.id}, {"name", b.name}, {"edges", out}};
          });
}

} // namespace

void registerDesignCommands(CommandRegistry& registry)
{
    registerSketchCommands(registry);
    registerFeatureCommands(registry);
    registerTimelineCommands(registry);
    registerBodyCommands(registry);
}

} // namespace aerocad::commands
