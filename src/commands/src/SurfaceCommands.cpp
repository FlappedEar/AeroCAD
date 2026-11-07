// SPDX-License-Identifier: GPL-3.0-or-later
// surface.*: base surfaces built from a region selection.
#include "CommandHelpers.hpp"

#include "aerocad/cad/Design.hpp"
#include "aerocad/core/MeshCheck.hpp"
#include "aerocad/core/MeshSelection.hpp"
#include "aerocad/core/Symmetry.hpp"
#include "aerocad/geometry/DrapeFit.hpp"
#include "aerocad/geometry/Modeling.hpp"
#include "aerocad/geometry/Offset.hpp"
#include "aerocad/geometry/RegionFit.hpp"
#include "aerocad/geometry/Shape.hpp"
#include "aerocad/geometry/SurfaceFit.hpp"
#include "aerocad/geometry/Tessellation.hpp"
#include "aerocad/geometry/Transform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aerocad::commands {

namespace {

std::string formatMm(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.2g", value);
    return buffer;
}

/// The warning for a fit that misses the requested tolerance: the result is kept as an
/// approximation, and says so in the result, the timeline and the saved feature report.
std::string toleranceWarning(double p99, double max, double tolerance)
{
    char buffer[256];
    std::snprintf(buffer, sizeof buffer,
                  "Approximation: the surface is up to %.3g mm from the scan (99%% within %.3g mm), more than "
                  "the requested tolerance of %.3g mm",
                  max, p99, tolerance);
    return buffer;
}

const model::Part& requireSurfacePart(model::Document& doc, const Json& params)
{
    const model::Part& source = requirePart(doc, params);
    if (source.kind == model::PartKind::Sketch)
        throw CommandError("Part " + std::to_string(source.id) + " is a sketch; offset and thicken work on surfaces");
    if (!source.shape || source.shape->isNull())
        throw CommandError("Part " + std::to_string(source.id) +
                           " has no exact surface; fit one with surface.fit_selection first");
    return source;
}

/// A part to show or hide along with adding a new one.
struct VisibilityChange {
    model::PartId id;
    bool visible;
};

/// What a part made from a sketch replaces on screen: the sketch (Fusion hides used sketches)
/// and a surface fitted just to draw on, whose scan comes back so the part is seen on it. All
/// stay in the browser.
std::vector<VisibilityChange> replacedByPart(const model::Document& doc, cad::FeatureId sketch, const model::Part& source)
{
    std::vector<VisibilityChange> changes;
    for (const auto& p : doc.parts())
        if (p.kind == model::PartKind::Sketch && p.featureId == sketch)
            changes.push_back({p.id, false});
    if (const model::PartId scan = model::fittedScanId(source); scan != 0) {
        changes.push_back({source.id, false});
        changes.push_back({scan, true});
    }
    return changes;
}

const cad::SketchResult& requireSketch(CommandContext& ctx, const Json& id)
{
    if (ctx.design == nullptr)
        throw CommandError("No design in this session");
    const auto sketchId = id.get<cad::FeatureId>();
    const auto& sketches = ctx.design->sketches();
    const auto it = sketches.find(sketchId);
    if (it == sketches.end())
        throw CommandError("No evaluated sketch with id " + std::to_string(sketchId));
    return it->second;
}

/// The sketch's closed profiles (all, or the given indices).
std::vector<geometry::Shape> sketchOutlines(const cad::SketchResult& sk, const Json& params)
{
    std::vector<geometry::Shape> outlines;
    if (params.contains("profiles")) {
        for (const auto& i : params.at("profiles")) {
            const auto index = i.get<std::size_t>();
            if (index >= sk.geometry.profiles.size())
                throw CommandError("Sketch " + sk.name + " has no profile " + std::to_string(index));
            outlines.push_back(sk.geometry.profiles[index]);
        }
    } else {
        outlines = sk.geometry.profiles;
    }
    if (outlines.empty())
        throw CommandError("Sketch " + sk.name + " has no closed profile; close the outline first");
    return outlines;
}

/// Bounds of the evaluated outlines in sketch coordinates (arcs bulge past their
/// defining points): {u0, v0, u1, v1}.
std::array<double, 4> outlineBounds(const geometry::Plane& plane, const std::vector<geometry::Shape>& outlines)
{
    double u0 = std::numeric_limits<double>::max(), v0 = u0, u1 = -u0, v1 = -u0;
    for (const auto& o : outlines) {
        const auto t = geometry::tessellate(o);
        for (const auto& p : t.vertices()) {
            const auto uv = plane.toLocal(p);
            u0 = std::min(u0, uv.x);
            u1 = std::max(u1, uv.x);
            v0 = std::min(v0, uv.y);
            v1 = std::max(v1, uv.y);
        }
    }
    if (u0 > u1)
        throw CommandError("The sketch outline is empty");
    return {u0, v0, u1, v1};
}

/// The evaluated sketch a feature is built from (params "sketch").
const cad::SketchResult& sketchOf(const cad::Design& d, const Json& params)
{
    const auto sketchId = params.at("sketch").get<cad::FeatureId>();
    const auto it = d.sketches().find(sketchId);
    if (it == d.sketches().end())
        throw CommandError("No sketch " + std::to_string(sketchId) + " before this step of the timeline");
    return it->second;
}

/// A positive length from the params: a number or an expression with user parameters.
double positive(const cad::Design& d, const Json& params, const char* key)
{
    const double v = d.evaluate(params.at(key));
    if (!(v > 0.0))
        throw CommandError(std::string(key) + " must be positive");
    return v;
}

/// The surface part a sketch-made part stands on: `id`, else the part the sketch was made on.
model::PartId surfacePartOf(const cad::Design& d, const Json& params)
{
    if (params.contains("id"))
        return params.at("id").get<model::PartId>();
    const cad::Feature* sketch = d.feature(params.at("sketch").get<cad::FeatureId>());
    if (sketch != nullptr)
        if (const Json on = sketch->params.value("onSurface", Json::object()); on.contains("part"))
            return on.at("part").get<model::PartId>();
    return 0;
}

/// Content fingerprint of a timeline body's exact geometry, so a cached result is reused
/// while the body under it is unchanged. Digest of its B-rep text without triangles and
/// without the per-shape flag lines (seven 0/1 digits: modified, checked, ...), which change
/// when the shape is merely tessellated or checked.
std::string shapeFingerprint(const geometry::Shape& shape)
{
    const std::string text = geometry::toBrep(shape, false);
    std::uint64_t h = 1469598103934665603ull;
    std::size_t used = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        const std::string_view line(text.data() + at, end - at);
        const bool flags = line.size() == 7 && line.find_first_not_of("01") == std::string_view::npos;
        if (!flags) {
            for (const unsigned char c : line) {
                h ^= c;
                h *= 1099511628211ull;
            }
            h ^= '\n';
            h *= 1099511628211ull;
            used += line.size() + 1;
        }
        at = end + 1;
    }
    return std::to_string(used) + "/" + std::to_string(h);
}

/// Content fingerprint of a part's geometry (its mesh, which follows its exact shape), so a
/// cached timeline result is reused while the scan or surface under it is unchanged.
std::string meshFingerprint(const core::TriangleMesh& mesh)
{
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
    };
    for (const auto& v : mesh.vertices()) {
        const double xyz[3] = {v.x, v.y, v.z};
        mix(xyz, sizeof xyz);
    }
    for (const auto& t : mesh.triangles())
        mix(t.data(), sizeof(std::uint32_t) * 3);
    return std::to_string(mesh.vertexCount()) + "/" + std::to_string(mesh.triangleCount()) + "/" + std::to_string(h);
}
/// The timeline feature (body id) whose document part is `id`, or 0 for an imported part.
cad::FeatureId timelineBodyOf(const cad::Design& d, model::PartId id)
{
    for (const auto& info : d.partInfos())
        if (info.part == id)
            return info.feature;
    return 0;
}

/// The part a step stands on. A timeline body is taken from the design as evaluated up to
/// this step, not from the document: while a project opens the document does not hold it
/// yet, and while an earlier step is edited it still holds the previous geometry.
const model::Part* standingPart(const model::Document& doc, const cad::Design& d, model::PartId id,
                                model::Part& scratch)
{
    const cad::FeatureId body = timelineBodyOf(d, id);
    if (body == 0)
        return doc.part(id);
    const auto it = d.bodies().find(body);
    if (it == d.bodies().end())
        throw CommandError("Part " + std::to_string(id) + " is not built before this step of the timeline");
    scratch = model::Part{};
    scratch.id = id;
    scratch.name = it->second.name;
    scratch.kind = model::PartKind::Body;
    scratch.featureId = body;
    scratch.shape = it->second.shape;
    scratch.mesh = std::make_shared<const core::TriangleMesh>(geometry::tessellate(it->second.shape));
    // On open there is no document copy yet. Preserve the fitted-scan fallback for
    // part_on_surface / fin_on_surface from the persistent upstream definition.
    if (const auto* f = d.feature(cad::featureOfBody(body)); f != nullptr && f->type == "fit_selection")
        scratch.source = "surface.fit_selection of part " + std::to_string(f->params.at("id").get<model::PartId>());
    return &scratch;
}

/// First body slot of mirrored fins (see FeatureEvaluator::Output::bodySlots).
constexpr std::size_t kMirrorSlot = 1000;

/// part_on_surface: the solid of surface.part_from_sketch (see its description).
cad::FeatureEvaluator::Output partOnSurface(const model::Document& doc, const cad::Design& d, const Json& params)
{
    const auto& sk = sketchOf(d, params);
    const auto outlines = sketchOutlines(sk, params);
    Json on;
    if (const cad::Feature* f = d.feature(sk.id); f != nullptr)
        on = f->params.value("onSurface", Json::object());
    model::PartId partId = 0;
    if (params.contains("id"))
        partId = params.at("id").get<model::PartId>();
    else if (on.contains("part"))
        partId = on.at("part").get<model::PartId>();
    else
        throw CommandError("Sketch " + sk.name + " was not made over a surface; give the surface part `id`");
    model::Part bodyPart;
    const model::Part* source = standingPart(doc, d, partId, bodyPart);
    if (source == nullptr)
        throw CommandError("No part with id " + std::to_string(partId));
    if (source->kind == model::PartKind::Sketch)
        throw CommandError("Part " + std::to_string(partId) + " is a sketch");

    const double thickness = positive(d, params, "thickness");
    const std::string sideName = params.value("side", std::string("inside"));
    const geometry::ThickenSide side = sideName == "outside" ? geometry::ThickenSide::Outside
                                       : sideName == "both"  ? geometry::ThickenSide::Both
                                                             : geometry::ThickenSide::Inside;
    // Planar area of the outline: the cut-out skin covers at least that much
    // (a curved surface over it is larger); less means it ran off the surface.
    double outlineArea = 0.0;
    for (const auto& o : outlines)
        outlineArea += geometry::area(o);
    const double tolerance = params.value("tolerance", geometry::SurfaceFitOptions{}.tolerance);
    Json fitReport;
    Json warnings = Json::array();
    // A fitted surface remembers the scan it came from: build the part on the scan
    // itself, whatever area the earlier fit covered.
    const model::Part* scan = source;
    if (source->shape && !source->shape->isNull()) {
        scan = doc.part(model::fittedScanId(*source));
        if (scan != nullptr && (!scan->mesh || scan->shape))
            scan = nullptr;
    } else if (!source->mesh || source->mesh->triangleCount() == 0) {
        throw CommandError("Part " + std::to_string(partId) + " has no surface");
    }

    geometry::Shape solid;
    double area = 0.0;
    if (scan != nullptr) {
        // The part follows the skin seen along the sketch normal.
        const auto [u0, v0, u1, v1] = outlineBounds(sk.plane, outlines);
        const double margin = std::max(10.0, 0.05 * std::max(u1 - u0, v1 - v0));
        geometry::DrapeArea drape;
        drape.origin = sk.plane.origin;
        drape.x = sk.plane.xDir;
        drape.y = sk.plane.yDir();
        drape.normal = sk.plane.normal;
        drape.u0 = u0 - margin;
        drape.v0 = v0 - margin;
        drape.u1 = u1 + margin;
        drape.v1 = v1 + margin;
        drape.insideU0 = u0;
        drape.insideV0 = v0;
        drape.insideU1 = u1;
        drape.insideV1 = v1;
        geometry::DrapeFitOptions options;
        options.tolerance = tolerance;
        try {
            const core::MeshProjector projector(*scan->mesh, sk.plane.normal);
            auto part = geometry::partOnScan(projector, drape, outlines, thickness, side, options);
            solid = part.solid;
            area = part.skinArea;
            const auto& rep = part.report;
            fitReport = {{"part", scan->id},
                         {"method", "draped"},
                         {"maxDeviation", rep.maxDeviation},
                         {"p99Deviation", rep.p99Deviation},
                         {"rmsDeviation", rep.rmsDeviation},
                         {"bridged", rep.bridgedFraction},
                         {"poles", {rep.uPoles, rep.vPoles}},
                         {"tolerance", tolerance},
                         {"withinTolerance", rep.p99Deviation <= tolerance},
                         {"milliseconds", rep.milliseconds}};
            if (rep.p99Deviation > tolerance)
                warnings.push_back(toleranceWarning(rep.p99Deviation, rep.maxDeviation, tolerance));
            if (rep.bridgedFraction > 0.05)
                warnings.push_back("About " + std::to_string(static_cast<int>(rep.bridgedFraction * 100.0)) +
                                   "% of the outline's area has no skin under it (holes, past the scan's "
                                   "edge); the part bridges it smoothly");
        } catch (const geometry::SurfaceFitError& e) {
            throw CommandError(std::string("Could not make the part on the scan: ") + e.what());
        }
    } else {
        geometry::Shape skin;
        try {
            skin = geometry::trimSurface(*source->shape, outlines, sk.plane.normal, true, true);
            area = geometry::surfaceArea(skin);
            if (area < 0.995 * outlineArea)
                warnings.push_back("The outline reaches past the surface; the part was cut off where the "
                                   "surface ends");
            solid = geometry::thickenSurface(skin, thickness, side);
        } catch (const geometry::OffsetError& e) {
            throw CommandError(e.what());
        }
    }
    cad::FeatureEvaluator::Output out;
    out.bodies.emplace_back(solid, params.value("name", sk.name + " part"));
    out.report = {{"thickness", thickness}, {"side", sideName}, {"area", area}, {"volume", solid.volume()},
                  {"warnings", warnings}};
    if (!fitReport.is_null())
        out.report["fit"] = fitReport;
    return out;
}

/// fin_on_surface: the fins of surface.fin_from_sketch (see its description).
cad::FeatureEvaluator::Output finOnSurface(const model::Document& doc, const cad::Design& d, const Json& params)
{
    const auto& sk = sketchOf(d, params);
    const auto outlines = sketchOutlines(sk, params);
    Json on;
    if (const cad::Feature* f = d.feature(sk.id); f != nullptr)
        on = f->params.value("onSurface", Json::object());
    model::PartId partId = 0;
    if (params.contains("id"))
        partId = params.at("id").get<model::PartId>();
    else if (on.contains("part"))
        partId = on.at("part").get<model::PartId>();
    else
        throw CommandError("Sketch " + sk.name + " was not made on a surface; give the surface part `id`");
    model::Part bodyPart;
    const model::Part* source = standingPart(doc, d, partId, bodyPart);
    if (source == nullptr)
        throw CommandError("No part with id " + std::to_string(partId));
    if (source->kind == model::PartKind::Sketch)
        throw CommandError("Part " + std::to_string(partId) + " is a sketch");
    const double thickness = positive(d, params, "thickness");
    const double rootFillet = params.contains("rootFillet") ? d.evaluate(params.at("rootFillet")) : 0.0;
    if (rootFillet < 0.0)
        throw CommandError("rootFillet must not be negative");
    const double tolerance = params.value("tolerance", geometry::SurfaceFitOptions{}.tolerance);
    const int count = params.contains("count") ? static_cast<int>(std::lround(d.evaluate(params.at("count")))) : 1;
    const double spacing = params.contains("spacing") ? d.evaluate(params.at("spacing")) : 40.0;
    if (count < 1 || count > 20)
        throw CommandError("count must be 1 to 20");

    // The scan the fins stand on (a fitted surface remembers its scan), to set each
    // fin's root on the skin.
    const model::Part* scanPart = source;
    if (source->shape && !source->shape->isNull()) {
        const model::Part* scan = doc.part(model::fittedScanId(*source));
        scanPart = scan != nullptr && scan->mesh && !scan->shape ? scan : source;
    }

    // Where each fin goes: the sketch plane and its outlines, moved together.
    struct Placement {
        geometry::Plane plane;
        std::vector<geometry::Shape> outlines;
    };
    Json warnings = Json::array();
    auto moved = [&](const Placement& p, const core::Vec3& offset) {
        Placement q{p.plane, {}};
        q.plane.origin = p.plane.origin + offset;
        for (const auto& o : p.outlines)
            q.outlines.push_back(geometry::translated(o, offset));
        return q;
    };
    // The plane's origin moved along its X axis (outward) onto the skin under it,
    // so the root reaches into the surface as drawn.
    auto onSkin = [&](const Placement& p, const std::string& what) {
        if (!scanPart->mesh || scanPart->mesh->triangleCount() == 0)
            return p;
        const core::MeshProjector projector(*scanPart->mesh, p.plane.xDir);
        const auto hit = projector.drop(p.plane.origin);
        if (!hit) {
            warnings.push_back(what + ": no surface under its root (a hole, or past the scan's edge); "
                               "it was left where it was placed");
            return p;
        }
        return moved(p, p.plane.xDir * (*hit - p.plane.origin).dot(p.plane.xDir));
    };
    std::vector<Placement> placements{{sk.plane, outlines}};
    for (int k = 1; k < count; ++k)
        placements.push_back(onSkin(moved(placements.front(), sk.plane.normal * (spacing * k)),
                                    "Fin " + std::to_string(k + 1)));

    // The other side of the car: every fin mirrored in the symmetry plane, then set
    // on the skin there (a scan is never exactly symmetric).
    Json mirrorReport;
    if (params.value("mirror", false)) {
        core::Vec3 point, normal;
        const Json plane = params.value("mirrorPlane", Json("auto"));
        if (plane.is_object()) {
            const auto& pt = plane.at("point");
            const auto& nm = plane.at("normal");
            point = {pt.at(0).get<double>(), pt.at(1).get<double>(), pt.at(2).get<double>()};
            normal = {nm.at(0).get<double>(), nm.at(1).get<double>(), nm.at(2).get<double>()};
            if (!(normal.length() > 0.0))
                throw CommandError("mirrorPlane normal is zero");
            normal = normal.normalized();
            mirrorReport = {{"source", "given"}};
        } else if (plane == "yz" || plane == "xz" || plane == "xy") {
            normal = plane == "yz" ? core::Vec3{1, 0, 0} : plane == "xz" ? core::Vec3{0, 1, 0} : core::Vec3{0, 0, 1};
            mirrorReport = {{"source", plane}};
        } else if (plane == "auto") {
            if (!scanPart->mesh)
                throw CommandError("No mesh to find the symmetry plane on; give mirrorPlane");
            const auto found = core::findSymmetryPlane(*scanPart->mesh);
            if (!found)
                throw CommandError("The scan is not symmetric enough to find its centre plane (a scan of one "
                                   "side only?); pick the mirror plane (yz, xz, xy) instead");
            point = found->point;
            normal = found->normal;
            mirrorReport = {{"source", "scan"}, {"match", found->match}, {"distance", found->distance}};
        } else {
            throw CommandError("mirrorPlane must be auto, yz, xz, xy or {point, normal}");
        }
        mirrorReport["point"] = {point.x, point.y, point.z};
        mirrorReport["normal"] = {normal.x, normal.y, normal.z};
        const std::size_t one = placements.size();
        for (std::size_t k = 0; k < one; ++k) {
            const Placement& p = placements[k];
            if (std::abs((p.plane.origin - point).dot(normal)) < 1.0)
                throw CommandError("The fin stands on the mirror plane; a mirrored copy would fall on it");
            Placement q;
            q.plane.origin = core::mirrorPoint(p.plane.origin, point, normal);
            q.plane.xDir = core::mirrorDirection(p.plane.xDir, normal);
            q.plane.normal = core::mirrorDirection(p.plane.normal, normal);
            for (const auto& o : p.outlines)
                q.outlines.push_back(geometry::mirrored(o, point, normal));
            placements.push_back(onSkin(q, "Mirrored fin " + std::to_string(k + 1)));
        }
    }

    // The worst fit of all fins made: one fin out of tolerance marks the feature.
    Json fitReport;
    auto addFit = [&](const Json& fit) {
        if (fit.is_null())
            return;
        if (fitReport.is_null()) {
            fitReport = fit;
            return;
        }
        const bool within = fitReport.value("withinTolerance", true) && fit.value("withinTolerance", true);
        if (fit.value("p99Deviation", 0.0) > fitReport.value("p99Deviation", 0.0)) {
            const double maxDeviation = std::max(fit.value("maxDeviation", 0.0), fitReport.value("maxDeviation", 0.0));
            fitReport = fit;
            fitReport["maxDeviation"] = maxDeviation;
        } else {
            fitReport["maxDeviation"] = std::max(fit.value("maxDeviation", 0.0), fitReport.value("maxDeviation", 0.0));
        }
        fitReport["withinTolerance"] = within;
    };
    // One fin at a placement, its root fillet as big as fits (up to rootFillet).
    auto makeFin = [&](const Placement& at, double& appliedFillet) {
        Json lastFit; // fit under the most recent build attempt of this fin
        const geometry::Plane& plane = at.plane;
        // Outward: the sketch X axis for a sketch made across the surface.
        const core::Vec3 outward = plane.xDir;
        // On a scan: the skin under the root, seen from outside (the first surface
        // met, never a layer behind it; holes bridged), as a height field over the
        // fin's footprint: along its chord (sketch Y) and across its thickness.
        auto fitUnder = [&](const model::Part& scan, double fillet) {
            const auto [u0, v0, u1, v1] = outlineBounds(plane, at.outlines);
            (void)u0;
            (void)u1;
            // Room around the root for its fillet to land on the surface.
            const double flare = fillet > 0.0 ? 2.0 * fillet + 2.0 : 0.0;
            const double margin = std::max(5.0, 0.1 * (v1 - v0)) + flare;
            const double half = 0.5 * thickness;
            const double across = std::max(5.0, 2.0 * thickness) + flare;
            geometry::DrapeArea area;
            area.origin = plane.origin;
            area.x = plane.yDir();
            area.y = plane.normal;
            area.normal = plane.xDir;
            area.u0 = v0 - margin;
            area.u1 = v1 + margin;
            area.v0 = -half - across;
            area.v1 = half + across;
            area.insideU0 = v0;
            area.insideU1 = v1;
            area.insideV0 = -half;
            area.insideV1 = half;
            geometry::DrapeFitOptions options;
            options.tolerance = tolerance;
            try {
                const core::MeshProjector projector(*scan.mesh, plane.xDir);
                geometry::DrapeFitReport rep;
                auto face = geometry::skinOnScan(projector, area, &rep, options);
                lastFit = {{"part", scan.id},
                           {"method", "draped"},
                           {"maxDeviation", rep.maxDeviation},
                           {"p99Deviation", rep.p99Deviation},
                           {"rmsDeviation", rep.rmsDeviation},
                           {"bridged", rep.bridgedFraction},
                           {"tolerance", tolerance},
                           {"withinTolerance", rep.p99Deviation <= tolerance},
                           {"milliseconds", rep.milliseconds}};
                return face;
            } catch (const geometry::SurfaceFitError& e) {
                throw CommandError(std::string("Could not find the scan under the fin root: ") + e.what());
            }
        };
        auto build = [&](double fillet) {
            lastFit = nullptr;
            auto finOn = [&](const geometry::Shape& surface) {
                return geometry::finOnSurface(surface, at.outlines, plane.normal, thickness, outward, fillet);
            };
            geometry::Shape solid;
            try {
                if (source->shape && !source->shape->isNull()) {
                    try {
                        solid = finOn(*source->shape);
                    } catch (const geometry::OffsetError&) {
                        // The fitted surface may not reach under the whole root: fit
                        // again on the scan it came from.
                        if (scanPart == source)
                            throw;
                        solid = finOn(fitUnder(*scanPart, fillet));
                    }
                } else {
                    if (!source->mesh || source->mesh->triangleCount() == 0)
                        throw CommandError("Part " + std::to_string(partId) + " has no surface");
                    solid = finOn(fitUnder(*source, fillet));
                }
            } catch (const geometry::OffsetError& e) {
                throw CommandError(e.what());
            }
            return solid;
        };
        // A fillet that does not fit must not cost the fin: smaller radii are tried
        // (a rolling ball wider than the fin's ends or the surface's bumps folds over
        // itself, which shows as a mesh that is not watertight), then none.
        appliedFillet = 0.0;
        geometry::Shape solid;
        std::string filletProblem;
        // Wider than twice the fin's thickness, a fillet is past what a canard needs
        // and what the fillet algorithm handles in good time.
        const double largest = std::min(rootFillet, 2.0 * thickness);
        const double smallest = std::max(0.5, 0.2 * largest);
        for (double f = largest; f > 0.0 && solid.isNull(); f = f * 0.6 >= smallest ? f * 0.6 : 0.0) {
            try {
                geometry::Shape candidate = build(f);
                if (!core::checkMesh(core::weldVertices(geometry::tessellate(candidate))).watertight()) {
                    filletProblem = "A root fillet of " + formatMm(f) + " mm folds over itself here";
                    continue;
                }
                solid = candidate;
                appliedFillet = f;
                addFit(lastFit);
            } catch (const CommandError& e) {
                if (filletProblem.empty())
                    filletProblem = e.what();
            }
        }
        if (solid.isNull()) {
            solid = build(0.0);
            addFit(lastFit);
        }
        if (rootFillet > 0.0 && appliedFillet == 0.0)
            warnings.push_back(filletProblem + "; the fin was made without a root fillet");
        else if (appliedFillet < rootFillet)
            warnings.push_back("A root fillet of " + formatMm(rootFillet) + " mm does not fit this fin; it got " +
                               formatMm(appliedFillet) + " mm");
        return solid;
    };

    const std::string baseName = params.value("name", sk.name + " fin");
    std::vector<std::pair<geometry::Shape, std::string>> fins;
    std::vector<unsigned> bodySlots;
    std::vector<double> fillets;
    for (std::size_t k = 0; k < placements.size(); ++k) {
        double applied = 0.0;
        try {
            const bool mirroredFin = !mirrorReport.is_null() && k >= placements.size() / 2;
            const std::size_t n = mirroredFin ? k - placements.size() / 2 : k;
            std::string name = count == 1 ? baseName : baseName + " " + std::to_string(n + 1);
            if (!mirrorReport.is_null())
                name += mirroredFin ? " (mirror)" : "";
            fins.emplace_back(makeFin(placements[k], applied), name);
            // Each fin keeps its id when the count changes or a fin before it fails:
            // its place in the stack, mirrored fins from slot 1000.
            bodySlots.push_back(static_cast<unsigned>(mirroredFin ? kMirrorSlot + n : n));
        } catch (const CommandError& e) {
            if (k == 0)
                throw;
            warnings.push_back("Fin " + std::to_string(k + 1) + " was not made: " + e.what());
            continue;
        }
        fillets.push_back(applied);
    }
    if (!fitReport.is_null() && !fitReport.value("withinTolerance", true))
        warnings.push_back(toleranceWarning(fitReport.at("p99Deviation").get<double>(),
                                            fitReport.at("maxDeviation").get<double>(),
                                            fitReport.at("tolerance").get<double>()));
    cad::FeatureEvaluator::Output out;
    out.bodies = std::move(fins);
    out.bodySlots = std::move(bodySlots);
    Json perFin = Json::array();
    for (std::size_t k = 0; k < out.bodies.size(); ++k)
        perFin.push_back({{"rootFillet", fillets[k]}, {"volume", out.bodies[k].first.volume()}});
    out.report = {{"thickness", thickness}, {"rootFillet", fillets.front()}, {"volume", out.bodies.front().first.volume()},
                  {"fins", perFin}, {"warnings", warnings}};
    if (!mirrorReport.is_null())
        out.report["mirror"] = mirrorReport;
    if (!fitReport.is_null())
        out.report["fit"] = fitReport;
    return out;
}

/// Connectivity fingerprint for a saved triangle selection. Coordinates may change (move,
/// scale, alignment), but silently reinterpreting indices after remeshing is unsafe.
std::string topologyFingerprint(const core::TriangleMesh& mesh)
{
    std::uint64_t h = 1469598103934665603ull;
    for (const auto& triangle : mesh.triangles())
        for (const auto index : triangle) {
            h ^= index;
            h *= 1099511628211ull;
        }
    return std::to_string(mesh.vertexCount()) + ":" + std::to_string(mesh.triangleCount()) + ":" + std::to_string(h);
}

int integerOption(const cad::Design& d, const Json& params, const char* key, int fallback, int min, int max)
{
    const double value = d.evaluate(params.value(key, Json(fallback)));
    if (!std::isfinite(value) || value != std::trunc(value) || value < min || value > max)
        throw CommandError(std::string(key) + " must be an integer between " + std::to_string(min) + " and " + std::to_string(max));
    return static_cast<int>(value);
}

cad::FeatureEvaluator::Output surfaceStep(const model::Document& doc, const cad::Design& d, const cad::Feature& f)
{
    const Json& params = f.params;
    model::Part scratch;
    const auto partId = params.at("id").get<model::PartId>();
    const model::Part* found = standingPart(doc, d, partId, scratch);
    if (found == nullptr)
        throw CommandError("No part with id " + std::to_string(partId));
    model::Part source = *found;
    const bool fitting = f.type == "fit_selection" || f.type == "fit_regions";
    if (fitting) {
        if (!source.mesh || source.mesh->triangleCount() == 0)
            throw CommandError("Part " + std::to_string(partId) + " has no mesh");
        const bool useSelection = f.type == "fit_selection" || params.value("useSelection", false);
        if (useSelection) {
            const auto triangles = params.value("triangles", core::TriangleSet{});
            if (triangles.empty())
                throw CommandError("The fit has no saved region; select a region and create a replacement fit");
            if (params.value("topology", std::string()) != topologyFingerprint(*source.mesh))
                throw CommandError("The source mesh topology changed; select the intended region and create a replacement fit");
            if (std::any_of(triangles.begin(), triangles.end(), [&](auto i) { return i >= source.mesh->triangleCount(); }))
                throw CommandError("The saved selection contains an invalid triangle");
            source.selection = std::make_shared<const core::TriangleSet>(triangles);
        }
        if (f.type == "fit_selection") {
            // One smooth surface over a large part of a scan takes minutes and cannot follow
            // it anyway. A part drawn on the scan needs no selection or fit.
            constexpr std::size_t maxTriangles = 200000;
            if (source.selection->size() > maxTriangles)
                throw CommandError("The selected area is too large for one smooth surface (" +
                                   std::to_string(source.selection->size()) + " triangles, at most " +
                                   std::to_string(maxTriangles) +
                                   "). Select a smaller area, or use Sketch and click the scan: a part drawn there "
                                   "follows the scan without a fitted surface");

            geometry::SurfaceFitOptions options;
            options.tolerance = d.evaluate(params.value("tolerance", Json(options.tolerance)));
            options.gridSize = params.contains("gridSize") ? integerOption(d, params, "gridSize", 0, 4, 200) : 0;
            const std::string method = params.value("method", std::string("auto"));
            if (method != "auto" && method != "plane" && method != "unfold")
                throw CommandError("Unknown surface fit method");
            if (!(options.tolerance > 0) || (options.gridSize != 0 && (options.gridSize < 4 || options.gridSize > 200)))
                throw CommandError("Invalid fit tolerance or grid size");
            options.method = method == "plane"    ? geometry::SurfaceFitMethod::Plane
                             : method == "unfold" ? geometry::SurfaceFitMethod::Unfold
                                                  : geometry::SurfaceFitMethod::Auto;
            geometry::SurfaceFitResult fit;
            try {
                fit = geometry::fitSurface(*source.mesh, *source.selection, options);
            } catch (const geometry::SurfaceFitError& e) {
                throw CommandError(e.what());
            }

            const auto& rep = fit.report;
            Json result;
            result["fit"] = {{"meshToSurfaceMax", rep.meshToSurfaceMax},
                             {"maxDeviation", std::max(rep.meshToSurfaceMax, rep.surfaceToMeshMax)},
                             {"meshToSurfaceRms", rep.meshToSurfaceRms},
                             {"surfaceToMeshMax", rep.surfaceToMeshMax},
                             {"outsideTrimMax", rep.outsideTrimMax},
                             {"tolerance", options.tolerance},
                             {"withinTolerance", rep.withinTolerance},
                             {"poles", {rep.uPoles, rep.vPoles}},
                             {"trimmed", rep.trimmed},
                             {"holes", rep.holes},
                             {"method", rep.method},
                             {"milliseconds", rep.milliseconds}};
            result["warnings"] = rep.warnings;
            return {{{fit.face, params.value("name", source.name + " surface")}}, {}, result};
        }
        geometry::RegionFitOptions options;
        options.creaseAngleDeg = d.evaluate(params.value("creaseAngle", Json(options.creaseAngleDeg)));
        options.fit.tolerance = d.evaluate(params.value("tolerance", Json(options.fit.tolerance)));
        options.blendRings = integerOption(d, params, "blendRings", options.blendRings, 0, 50);
        if (!(options.fit.tolerance > 0) || !(options.creaseAngleDeg > 0 && options.creaseAngleDeg <= 180) ||
            options.blendRings < 0 || options.blendRings > 50)
            throw CommandError("Invalid region fit tolerance, crease angle or blend rings");
        geometry::RegionFitResult fit;
        try {
            fit = geometry::fitRegions(*source.mesh, useSelection ? *source.selection : core::TriangleSet{},
                                       options);
        } catch (const geometry::SurfaceFitError& e) {
            throw CommandError(e.what());
        }
        Json result;
        Json patches = Json::array();
        double worst = 0.0;
        double maxDeviation = 0.0;
        bool allWithin = true;
        for (const auto& p : fit.patches) {
            Json j{{"triangles", p.triangles.size()}};
            if (p.error.empty()) {
                j["meshToSurfaceMax"] = p.report.meshToSurfaceMax;
                j["surfaceToMeshMax"] = p.report.surfaceToMeshMax;
                j["withinTolerance"] = p.report.withinTolerance;
                j["method"] = p.report.method;
                worst = std::max(worst, p.report.meshToSurfaceMax);
                maxDeviation = std::max({maxDeviation, p.report.meshToSurfaceMax, p.report.surfaceToMeshMax});
                allWithin = allWithin && p.report.withinTolerance;
            } else {
                j["error"] = p.error;
                allWithin = false;
            }
            patches.push_back(std::move(j));
        }
        result["fit"] = {{"patches", patches},
                         {"faces", fit.faces},
                         {"freeEdges", fit.freeEdges},
                         {"solid", fit.solid},
                         {"volume", fit.volume},
                         {"meshToSurfaceMax", worst},
                         {"maxDeviation", maxDeviation},
                         {"withinTolerance", allWithin},
                         {"tolerance", options.fit.tolerance},
                         {"sewTolerance", fit.sewTolerance},
                         {"smoothSeams", fit.smoothSeams},
                         {"seamAngleMaxDeg", fit.seamAngleMaxDeg},
                         {"milliseconds", fit.milliseconds}};
        Json warnings = fit.warnings;
        if (!allWithin)
            warnings.push_back("Approximation: some patches are more than the requested tolerance of " +
                               formatMm(options.fit.tolerance) + " mm from the mesh (worst " +
                               formatMm(worst) + " mm); see fit.patches");
        result["warnings"] = warnings;
        return {{{fit.shape, params.value("name", source.name + (fit.solid ? " solid" : " surfaces"))}}, {}, result};
    }
    if (source.kind == model::PartKind::Sketch || !source.shape || source.shape->isNull())
        throw CommandError("Part " + std::to_string(partId) + " has no exact surface; fit a surface first");
    // Clearance, trimming and thickness do not improve the fitted reference's quality.
    // Keep its fit report visible on the final part and in export checks (CAD-85).
    Json inherited = Json::object();
    if (source.kind == model::PartKind::Body) {
        const Json report = d.status(cad::featureOfBody(source.featureId)).report;
        if (report.is_object() && report.contains("fit")) {
            inherited["fit"] = report.at("fit");
            inherited["warnings"] = report.value("warnings", Json::array());
            inherited["fitInherited"] = true;
            inherited["fitSourceFeature"] = report.value("fitSourceFeature", cad::featureOfBody(source.featureId));
            if (!report.value("fitInherited", false))
                for (auto& warning : inherited["warnings"])
                    warning = "Reference fit: " + warning.get<std::string>();
        }
    }
    if (f.type == "trim_by_sketch") {
        const auto& sk = sketchOf(d, params);
        const auto outlines = sketchOutlines(sk, params);
        const std::string keep = params.value("keep", std::string("inside"));
        if (keep != "inside" && keep != "outside")
            throw CommandError("Keep must be inside or outside");
        geometry::Shape shape;
        try {
            shape = geometry::trimSurface(*source.shape, outlines, sk.plane.normal, keep != "outside",
                                          params.value("extend", true));
        } catch (const geometry::OffsetError& e) {
            throw CommandError(e.what());
        }
        Json result = inherited;
        result["area"] = geometry::surfaceArea(shape);
        result["faces"] = shape.faceCount();
        result["keep"] = keep;
        return {{{shape, params.value("name", source.name + " " + sk.name)}}, {}, result};
    }
    if (f.type == "offset_surface") {
        const double distance = d.evaluate(params.at("distance"));
        geometry::Shape shape;
        try {
            shape = geometry::offsetSurface(*source.shape, distance);
        } catch (const geometry::OffsetError& e) {
            throw CommandError(e.what());
        }
        Json result = inherited;
        result["distance"] = distance;
        result["area"] = geometry::surfaceArea(shape);
        return {{{shape, params.value("name", source.name + " offset")}}, {}, result};
    }
    const double thickness = positive(d, params, "thickness");
    const std::string sideName = params.value("side", std::string("outside"));
    if (sideName != "inside" && sideName != "outside" && sideName != "both")
        throw CommandError("Side must be inside, outside or both");
    const geometry::ThickenSide side = sideName == "inside" ? geometry::ThickenSide::Inside
                                       : sideName == "both" ? geometry::ThickenSide::Both
                                                            : geometry::ThickenSide::Outside;
    geometry::Shape shape;
    try {
        shape = geometry::thickenSurface(*source.shape, thickness, side);
    } catch (const geometry::OffsetError& e) {
        throw CommandError(e.what());
    }
    Json result = inherited;
    result["thickness"] = thickness;
    result["side"] = sideName;
    result["volume"] = shape.volume();
    return {{{shape, params.value("name", source.name + " solid")}}, {}, result};
}

/// Adds a feature built on a surface or scan to the
/// timeline as one undo step. Like the timeline's own features it can later be edited,
/// suppressed or deleted and follows edits of its sketch. The sketch is hidden (Fusion
/// hides used sketches); a surface fitted just to draw on is hidden and its scan shown.
Json addSurfaceFeature(CommandContext& ctx, const std::string& type, const Json& params, const std::string& label)
{
    if (ctx.design == nullptr)
        throw CommandError("No design in this session");
    cad::Design& d = *ctx.design;
    model::Document& doc = ctx.document;
    std::vector<VisibilityChange> changes;
    if (type == "part_on_surface" || type == "fin_on_surface") {
        const auto& sk = requireSketch(ctx, params.at("sketch"));
        const model::PartId partId = surfacePartOf(d, params);
        if (partId == 0)
            throw CommandError("Sketch " + sk.name + " was not made " + (type == "fin_on_surface" ? "on" : "over") +
                               " a surface; give the surface part `id`");
        const model::Part* source = doc.part(partId);
        if (source == nullptr)
            throw CommandError("No part with id " + std::to_string(partId));
        changes = replacedByPart(doc, sk.id, *source);
    }

    const cad::Design::State before = d.state();
    cad::FeatureId id = 0;
    try {
        id = d.addFeature(type, params);
    } catch (const cad::DesignError& e) {
        throw CommandError(e.what());
    }
    const cad::Design::State after = d.state();
    d.syncDocument(doc);
    std::vector<VisibilityChange> was;
    for (const auto& c : changes) {
        if (const model::Part* p = doc.part(c.id); p != nullptr && p->visible != c.visible) {
            was.push_back({c.id, p->visible});
            doc.modifyPart(c.id, [&](model::Part& q) { q.visible = c.visible; });
        }
    }
    cad::Design* dp = &d;
    ctx.undoStack.push({label + " " + d.feature(id)->name,
                        [dp, &doc, before, was]() {
                            dp->restore(before);
                            dp->syncDocument(doc);
                            for (const auto& c : was)
                                doc.modifyPart(c.id, [&](model::Part& q) { q.visible = c.visible; });
                        },
                        [dp, &doc, after, was]() {
                            dp->restore(after);
                            dp->syncDocument(doc);
                            for (const auto& c : was)
                                doc.modifyPart(c.id, [&](model::Part& q) { q.visible = !c.visible; });
                        }});

    // The bodies' parts, first one first, with what the feature reports.
    std::vector<const model::Part*> made;
    for (const auto& q : doc.parts())
        if (q.kind == model::PartKind::Body && cad::featureOfBody(q.featureId) == id)
            made.push_back(&q);
    std::sort(made.begin(), made.end(), [](const model::Part* a, const model::Part* b) { return a->featureId < b->featureId; });
    std::vector<Json> parts;
    for (const model::Part* p : made)
        parts.push_back(partToJson(*p));
    if (parts.empty())
        throw CommandError("The feature made no part");
    const Json report = d.status(id).report;
    Json result = parts.front();
    for (const auto& [key, value] : report.items())
        if (key != "fins")
            result[key] = value;
    result["feature"] = id;
    result["featureName"] = d.feature(id)->name;
    if (report.contains("fins")) {
        Json all = Json::array();
        for (std::size_t k = 0; k < parts.size(); ++k) {
            Json p = parts[k];
            if (k < report.at("fins").size())
                for (const auto& [key, value] : report.at("fins").at(k).items())
                    p[key] = value;
            all.push_back(std::move(p));
        }
        result["fins"] = all;
    }
    return result;
}

/// Create the persistent definition, including the region selected at creation time.
Json addSurfaceStep(CommandContext& ctx, const std::string& type, const Json& params, const std::string& label)
{
    Json stored = params;
    const model::Part& source = requirePart(ctx.document, params);
    if (type == "fit_selection" || type == "fit_regions") {
        const bool hasSelection = source.selection && !source.selection->empty();
        const bool useSelection = type == "fit_selection" || params.value("useSelection", hasSelection);
        if (useSelection && !hasSelection)
            throw CommandError("Select a region on part " + std::to_string(source.id) + " first");
        stored["useSelection"] = useSelection;
        if (hasSelection && source.mesh) {
            stored["triangles"] = *source.selection;
            stored["topology"] = topologyFingerprint(*source.mesh);
        }
    } else {
        (void)requireSurfacePart(ctx.document, params);
    }
    return addSurfaceFeature(ctx, type, stored, label);
}

} // namespace

void registerSurfaceFeatures(cad::Design& design, const model::Document& document)
{
    // Fingerprints are cached per part revision: hashing a large scan on every
    // regeneration would cost more than the cache saves.
    // The mesh itself is remembered too: an opened project replaces the parts, and its parts
    // start again at revision 0.
    struct Seen {
        std::uint64_t revision;
        std::weak_ptr<const core::TriangleMesh> mesh;
        geometry::Shape shape;
        std::string fingerprint;
    };
    auto seen = std::make_shared<std::map<model::PartId, Seen>>();
    const model::Document* doc = &document;
    auto fingerprint = [seen, doc](model::PartId id) -> std::string {
        const model::Part* p = doc->part(id);
        if (p == nullptr)
            return "missing";
        auto it = seen->find(id);
        const geometry::Shape shape = p->shape.value_or(geometry::Shape{});
        if (it == seen->end() || it->second.revision != p->revision || it->second.mesh.lock() != p->mesh ||
            it->second.shape.impl() != shape.impl()) {
            std::string fp = p->mesh ? meshFingerprint(*p->mesh) : std::string("empty");
            // Exact surface operations read the B-rep, not the display tessellation.
            // A replaced shape may deliberately retain the same mesh (or have no mesh).
            if (!shape.isNull())
                fp += ";brep:" + shapeFingerprint(shape);
            it = seen->insert_or_assign(id, Seen{p->revision, p->mesh, shape, fp}).first;
        }
        return it->second.fingerprint;
    };
    const cad::Design* dp = &design;
    auto inputs = [fingerprint, doc, dp](const cad::Feature& f) {
        const model::PartId partId = surfacePartOf(*dp, f.params);
        // A timeline body: its geometry as evaluated before this step (see standingPart).
        if (const cad::FeatureId body = timelineBodyOf(*dp, partId); body != 0) {
            const auto it = dp->bodies().find(body);
            std::string key = std::to_string(partId) + ":body:" +
                              (it == dp->bodies().end() ? std::string("missing") : shapeFingerprint(it->second.shape));
            if (f.type == "trim_by_sketch" || f.type == "offset_surface" || f.type == "thicken_surface")
                key += ";report:" + dp->status(cad::featureOfBody(body)).report.dump();
            // Part/fin-on-surface can extend past the fitted patch on its original scan.
            if (const auto* upstream = dp->feature(cad::featureOfBody(body));
                upstream != nullptr && upstream->type == "fit_selection") {
                const auto scan = upstream->params.at("id").get<model::PartId>();
                key += ";" + std::to_string(scan) + ":" + fingerprint(scan);
            }
            return key;
        }
        std::string key = std::to_string(partId) + ":" + fingerprint(partId);
        if (const model::Part* p = doc->part(partId); p != nullptr)
            if (const model::PartId scan = model::fittedScanId(*p); scan != 0)
                key += ";" + std::to_string(scan) + ":" + fingerprint(scan);
        return key;
    };
    design.setFeatureEvaluator("part_on_surface",
                               {inputs, [doc](const cad::Feature& f, const cad::Design& d) {
                                    return partOnSurface(*doc, d, f.params);
                                }});
    design.setFeatureEvaluator("fin_on_surface",
                               {inputs, [doc](const cad::Feature& f, const cad::Design& d) {
                                    return finOnSurface(*doc, d, f.params);
                                }});
    for (const std::string type : {"fit_selection", "fit_regions", "trim_by_sketch", "offset_surface", "thicken_surface"})
        design.setFeatureEvaluator(type, {inputs, [doc](const cad::Feature& f, const cad::Design& d) {
                                             return surfaceStep(*doc, d, f);
                                         }});

}

void registerSurfaceCommands(CommandRegistry& r)
{
    r.add({"surface.fit_selection", "Fit surface to selection",
           "Fits one smooth B-spline (NURBS) surface to the region selection of a mesh part and adds it as a new "
           "part with exact geometry, trimmed to the selection outline. Reports the deviation both ways (mesh to "
           "surface and surface to mesh) in mm. Holes of the selection are cut out. Any amount of wrap works as "
           "long as the region is a simple sheet (no handles); see method.",
           objectSchema(
               {{"id", idParam()},
                {"tolerance",
                 {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Fit tolerance in mm. Default 0.25."}}},
                {"gridSize",
                 {{"type", "integer"},
                  {"minimum", 4},
                  {"maximum", 200},
                  {"description", "Resampling grid nodes along the longer side. Default: from the point count."}}},
                {"method",
                 {{"type", "string"},
                  {"enum", {"auto", "plane", "unfold"}},
                  {"description", "unfold: region flattened by a conformal map, any amount of wrap (mirror "
                                  "housings, long curved strips), needs a simple sheet; plane: height field over "
                                  "the best-fit plane, no folds; auto (default): unfold, else plane."}}},
                {"name", {{"type", "string"}, {"description", "Name of the new part."}}}},
               {"id"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              return addSurfaceStep(ctx, "fit_selection", params, "Fit surface");
          });
    r.add({"surface.fit_regions", "Fit surfaces to mesh",
           "Turns a scanned mesh (or its region selection) into exact geometry: splits it into smooth patches at "
           "creases, fits a B-spline surface to each and sews them into one shell, a solid when it closes up "
           "(a watertight scan becomes a solid body). Tubes and strongly bent patches are split further. Adds "
           "the result as a new part and reports each patch's deviation.",
           objectSchema(
               {{"id", idParam()},
                {"creaseAngle",
                 {{"type", "number"},
                  {"exclusiveMinimum", 0},
                  {"maximum", 180},
                  {"description", "Angle between neighbouring triangles (degrees) that splits patches. Default 30."}}},
                {"tolerance",
                 {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Fit tolerance in mm. Default 0.25."}}},
                {"useSelection",
                 {{"type", "boolean"},
                  {"description", "Only the region selection (default when there is one); false: the whole mesh."}}},
                {"blendRings",
                 {{"type", "integer"},
                  {"minimum", 0},
                  {"maximum", 50},
                  {"description", "Where a smooth region is split into several patches, each is fit to this many "
                                  "rings of triangles past its seam so the patches meet with matching tangents. "
                                  "0: fit each patch alone. Default 6."}}},
                {"name", {{"type", "string"}, {"description", "Name of the new part."}}}},
               {"id"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              return addSurfaceStep(ctx, "fit_regions", params, "Fit surfaces");
          });
    r.add({"surface.trim_by_sketch", "Trim surface by sketch",
           "Projects the closed profiles of a sketch onto a curved surface part along the sketch normal and keeps "
           "the part of the surface inside them (keep: inside, default) or outside, as a new surface part that "
           "lies exactly on the original surface. Typical use: fit a surface to a scan (surface.fit_selection), "
           "sketch the outline of a part over it (sketch.create with surface: {part}), trim, then thicken "
           "(surface.thicken) into a solid that follows the scan, e.g. an aileron or a flap.",
           objectSchema({{"id", idParam()},
                         {"sketch", {{"type", "integer"}, {"minimum", 1}, {"description", "Sketch feature id."}}},
                         {"profiles",
                          {{"type", "array"},
                           {"items", {{"type", "integer"}, {"minimum", 0}}},
                           {"description", "Profile indices (sketch.profiles); default all."}}},
                         {"keep", {{"type", "string"}, {"enum", {"inside", "outside"}}}},
                         {"extend",
                          {{"type", "boolean"},
                           {"description",
                            "Use the whole fitted surface, past its own (ragged) trim outline and holes, so the "
                            "edges are the sketch outline's (default true; single-face surfaces)."}}},
                         {"name", {{"type", "string"}, {"description", "Name of the new part."}}}},
                        {"id", "sketch"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              return addSurfaceStep(ctx, "trim_by_sketch", params, "Trim surface");
          });
    r.add({"surface.part_from_sketch", "Part on surface",
           "One step from a sketch outline to a solid part that follows a curved surface or scan, e.g. an aileron "
           "or flap drawn over a scanned wing: the outline is projected onto the surface along the sketch normal, "
           "the surface inside it is cut out and thickened. On a scan (mesh) a smooth surface is first fitted to "
           "the area under the outline, so no region selection or separate fit is needed. The edges follow the "
           "sketch outline exactly. The sketch should be made with sketch.create surface: {part}; that part is "
           "used unless `id` is given.",
           objectSchema({{"sketch", {{"type", "integer"}, {"minimum", 1}, {"description", "Sketch feature id."}}},
                         {"id", idParam()},
                         {"thickness", {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "mm."}}},
                         {"side",
                          {{"type", "string"},
                           {"enum", {"inside", "outside", "both"}},
                           {"description", "inside (default): from the surface into the object, against the way "
                                           "the surface faces; outside: away from it; both: half each way."}}},
                         {"profiles",
                          {{"type", "array"},
                           {"items", {{"type", "integer"}, {"minimum", 0}}},
                           {"description", "Profile indices (sketch.profiles); default all."}}},
                         {"tolerance",
                          {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Fit tolerance (scan), mm. Default 0.25."}}},
                         {"name", {{"type", "string"}, {"description", "Name of the new part."}}}},
                        {"sketch", "thickness"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              return addSurfaceFeature(ctx, "part_on_surface", params, "Part on surface");
          });
    r.add({"surface.fin_from_sketch", "Fin on surface",
           "One step from a fin outline to a solid fin standing on a curved surface or scan, e.g. a canard (dive "
           "plane) on a scanned bumper or a winglet: the sketch is made across the surface (sketch.create with "
           "surface: {part, point, across: true}; sketch X points outward along the surface normal), the outline "
           "is extruded `thickness` mm symmetrically about the sketch plane, and its root is cut where it passes "
           "through the surface, so the root follows the surface exactly. Draw the root a little into the "
           "surface (x < 0). On a scan a smooth surface is first fitted under the root. With rootFillet the root "
           "flares into the surface with a concave fillet of that radius, tangent to it (a stronger joint, more "
           "area to glue a printed fin on).",
           objectSchema({{"sketch", {{"type", "integer"}, {"minimum", 1}, {"description", "Sketch feature id."}}},
                         {"id", idParam()},
                         {"thickness", {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "mm."}}},
                         {"profiles",
                          {{"type", "array"},
                           {"items", {{"type", "integer"}, {"minimum", 0}}},
                           {"description", "Profile indices (sketch.profiles); default all."}}},
                         {"tolerance",
                          {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Fit tolerance (scan), mm. Default 0.25."}}},
                         {"rootFillet",
                          {{"type", "number"},
                           {"minimum", 0},
                           {"description", "Radius of the fillet where the fin meets the surface, mm; 0 (default) for none. At most twice the thickness; when it does not fit, a smaller one is used and a warning says so."}}},
                         {"count",
                          {{"type", "integer"},
                           {"minimum", 1},
                           {"maximum", 20},
                           {"description", "Number of fins, e.g. 2 for a stacked pair of canards (default 1). The copies "
                                           "are spaced along the sketch normal, each with its root set on the surface "
                                           "where it lands."}}},
                         {"spacing",
                          {{"type", "number"},
                           {"description", "Distance between the fins along the sketch normal, mm (default 40; negative "
                                           "for the other side)."}}},
                         {"mirror",
                          {{"type", "boolean"},
                           {"description", "Also make the fins on the other side: mirrored in the symmetry plane, each "
                                           "set on the surface there (default false)."}}},
                         {"mirrorPlane",
                          {{"description", "auto (default: the scan's own symmetry plane, e.g. a car's centre plane), "
                                           "yz / xz / xy (world planes through the origin) or {point, normal}."}}},
                         {"name", {{"type", "string"}, {"description", "Name of the new part."}}}},
                        {"sketch", "thickness"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              return addSurfaceFeature(ctx, "fin_on_surface", params, "Fin on surface");
          });
    r.add({"surface.offset", "Offset surface",
           "Adds a copy of a surface part moved by a distance along its normal (negative: against it), for "
           "example a clearance surface that keeps 2 mm off a body. The fitted base surface's normal points to "
           "the side the mesh triangles face (usually outward).",
           objectSchema({{"id", idParam()},
                         {"distance", {{"type", "number"}, {"description", "Offset in mm along the normal; not 0."}}},
                         {"name", {{"type", "string"}, {"description", "Name of the new part."}}}},
                        {"id", "distance"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              return addSurfaceStep(ctx, "offset_surface", params, "Offset surface");
          });

    r.add({"surface.thicken", "Thicken surface",
           "Turns a surface part into a closed solid of the given thickness, with side walls along its boundary. "
           "side: outside (along the normal, default), inside (against it) or both (half each way). The solid "
           "can be used by the solid modelling commands.",
           objectSchema({{"id", idParam()},
                         {"thickness", {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "mm."}}},
                         {"side", {{"type", "string"}, {"enum", {"outside", "inside", "both"}}}},
                         {"name", {{"type", "string"}, {"description", "Name of the new part."}}}},
                        {"id", "thickness"}),
           true},
          [](CommandContext& ctx, const Json& params) {
              return addSurfaceStep(ctx, "thicken_surface", params, "Thicken surface");
          });
}

} // namespace aerocad::commands
