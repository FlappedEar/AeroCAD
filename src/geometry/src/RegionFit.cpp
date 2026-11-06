// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/RegionFit.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <ShapeFix_Solid.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>

#include <math_Jacobi.hxx>
#include <math_Matrix.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace aerocad::geometry {

namespace {

/// Outline points of `patch` where the neighbour on the other side changes (another
/// patch, or the open border): there the shared boundary is split into separate edges.
/// `label` gives the patch of every triangle (-1 outside the region fit); triangles of
/// the same label but not in `patch` count as one more neighbour.
std::vector<core::Vec3> patchCorners(const core::TriangleMesh& mesh, const core::MeshAdjacency& adjacency,
                                     const std::vector<std::int64_t>& label, const core::TriangleSet& patch)
{
    std::unordered_map<std::uint32_t, char> inside;
    for (const auto t : patch)
        inside.emplace(t, 1);
    std::map<std::array<double, 3>, std::set<std::int64_t>> sides;
    for (const auto t : patch) {
        const auto& tri = mesh.triangles()[t];
        for (int s = 0; s < 3; ++s) {
            const auto nb = adjacency.neighbours(t)[static_cast<std::size_t>(s)];
            if (nb != core::MeshAdjacency::none && inside.count(nb) != 0)
                continue;
            std::int64_t other = nb == core::MeshAdjacency::none ? -2 : label[nb];
            if (nb != core::MeshAdjacency::none && other == label[t])
                other = -3; // rest of the same region (a split)
            for (const auto vi : {tri[static_cast<std::size_t>(s)], tri[static_cast<std::size_t>((s + 1) % 3)]}) {
                const auto& p = mesh.vertices()[vi];
                sides[{p.x, p.y, p.z}].insert(other);
            }
        }
    }
    std::vector<core::Vec3> out;
    for (const auto& [p, labels] : sides)
        if (labels.size() >= 2)
            out.push_back({p[0], p[1], p[2]});
    return out;
}

/// Splits a patch in two by the plane through its centroid across the direction in which
/// its normals turn most (for a tube: a plane containing the axis).
std::vector<core::TriangleSet> bisect(const core::TriangleMesh& mesh, const core::MeshAdjacency& adjacency,
                                      const core::TriangleSet& patch)
{
    core::Vec3 centroid;
    math_Matrix cov(1, 3, 1, 3, 0.0);
    std::vector<core::Vec3> centres;
    for (const auto t : patch) {
        const auto& tri = mesh.triangles()[t];
        const core::Vec3 c =
            (mesh.vertices()[tri[0]] + mesh.vertices()[tri[1]] + mesh.vertices()[tri[2]]) * (1.0 / 3.0);
        centres.push_back(c);
        centroid = centroid + c;
        const core::Vec3 n = mesh.faceNormal(t);
        const std::array<double, 3> v{n.x, n.y, n.z};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                cov(i + 1, j + 1) += v[static_cast<std::size_t>(i)] * v[static_cast<std::size_t>(j)];
    }
    centroid = centroid * (1.0 / static_cast<double>(patch.size()));
    // The normals of a bent patch spread in one direction across the bend; the cut plane
    // takes that direction as its normal. Normals point both ways round a tube, so their
    // second moments (not their mean) are used.
    math_Jacobi jacobi(cov);
    int best = 1;
    for (int i = 2; i <= 3; ++i)
        if (jacobi.Value(i) > jacobi.Value(best))
            best = i;
    math_Vector dir(1, 3);
    jacobi.Vector(best, dir);
    // The mean normal is the other large direction for a cap-like patch; cut across the
    // spread that is perpendicular to it.
    const core::Vec3 cut{dir(1), dir(2), dir(3)};
    core::TriangleSet a, b;
    for (std::size_t k = 0; k < patch.size(); ++k)
        ((centres[k] - centroid).dot(cut) < 0.0 ? a : b).push_back(patch[k]);
    std::vector<core::TriangleSet> out;
    for (auto* half : {&a, &b}) {
        if (half->empty())
            continue;
        for (auto& part : core::segmentSmoothRegions(mesh, adjacency, *half, 180.0, 0.0))
            out.push_back(std::move(part));
    }
    return out;
}

} // namespace

RegionFitResult fitRegions(const core::TriangleMesh& mesh, const core::TriangleSet& selection,
                           const RegionFitOptions& options)
{
    const auto t0 = std::chrono::steady_clock::now();
    RegionFitResult result;
    const core::MeshAdjacency adjacency(mesh);

    core::TriangleSet all = selection;
    if (all.empty()) {
        all.resize(mesh.triangleCount());
        for (std::uint32_t t = 0; t < all.size(); ++t)
            all[t] = t;
    }
    const double totalArea = core::selectionArea(mesh, all);
    if (!(totalArea > 0.0))
        throw SurfaceFitError("The selection has no area");
    const auto regions =
        core::segmentSmoothRegions(mesh, adjacency, all, options.creaseAngleDeg, options.minRegionFraction * totalArea);

    double edgeSum = 0.0;
    for (const auto t : all) {
        const auto& tri = mesh.triangles()[t];
        edgeSum += (mesh.vertices()[tri[1]] - mesh.vertices()[tri[0]]).length();
    }
    const double meanEdge = edgeSum / static_cast<double>(all.size());
    result.sewTolerance =
        options.sewTolerance > 0.0 ? options.sewTolerance : std::max(2.0 * options.fit.tolerance, 0.5 * meanEdge);

    // Patches that do not fit (a tube, which does not unfold onto a disc, or one bent too
    // far for the tolerance) are cut in two and tried again, a few times.
    SurfaceFitOptions fitOptions = options.fit;
    fitOptions.outlineSmoothing = 0; // exact shared borders, so neighbours sew
    std::vector<std::int64_t> label(mesh.triangleCount(), -1);
    for (std::size_t r = 0; r < regions.size(); ++r)
        for (const auto t : regions[r])
            label[t] = static_cast<std::int64_t>(r);

    // Fit all regions with corners where neighbours meet, splitting the ones that do not fit.
    struct Fitted {
        core::TriangleSet triangles;
        std::vector<core::Vec3> corners;
        std::optional<SurfaceFitResult> fit;
        std::string error;
    };
    auto fitWith = [&](Fitted& f) {
        f.fit.reset();
        f.error.clear();
        SurfaceFitOptions o = fitOptions;
        o.outlineCorners = f.corners;
        try {
            f.fit = fitSurface(mesh, f.triangles, o);
        } catch (const SurfaceFitError& e) {
            f.error = e.what();
        } catch (const Standard_Failure& failure) {
            f.error = failure.GetMessageString() != nullptr ? failure.GetMessageString() : "fit failed";
        }
    };
    std::vector<std::pair<core::TriangleSet, int>> queue;
    for (const auto& region : regions)
        queue.emplace_back(region, 0);
    std::vector<Fitted> fitted;
    bool split = false;
    while (!queue.empty()) {
        auto [patch, depth] = std::move(queue.back());
        queue.pop_back();
        Fitted f;
        f.triangles = std::move(patch);
        f.corners = patchCorners(mesh, adjacency, label, f.triangles);
        fitWith(f);
        const bool good = f.fit && f.fit->report.withinTolerance;
        if (!good && depth < 3 && f.triangles.size() >= 64) {
            auto halves = bisect(mesh, adjacency, f.triangles);
            if (halves.size() >= 2) {
                split = true;
                for (auto& h : halves)
                    queue.emplace_back(std::move(h), depth + 1);
                continue;
            }
        }
        fitted.push_back(std::move(f));
    }
    if (fitted.size() > regions.size())
        result.warnings.push_back("Split " + std::to_string(fitted.size() - regions.size()) +
                                  " extra times to fit tubes or strongly bent regions.");
    // After splits, patches next to a cut have new corners: refit those.
    if (split) {
        for (std::size_t k = 0; k < fitted.size(); ++k)
            for (const auto t : fitted[k].triangles)
                label[t] = static_cast<std::int64_t>(k);
        for (auto& f : fitted) {
            auto corners = patchCorners(mesh, adjacency, label, f.triangles);
            auto key = [](const core::Vec3& p) { return std::array<double, 3>{p.x, p.y, p.z}; };
            auto sorted = [&](const std::vector<core::Vec3>& c) {
                std::vector<std::array<double, 3>> k;
                for (const auto& p : c)
                    k.push_back(key(p));
                std::sort(k.begin(), k.end());
                return k;
            };
            if (sorted(corners) != sorted(f.corners)) {
                f.corners = std::move(corners);
                fitWith(f);
            }
        }
    }

    BRepBuilderAPI_Sewing sewing(result.sewTolerance);
    for (auto& f : fitted) {
        RegionPatch patch;
        patch.triangles = std::move(f.triangles);
        patch.error = f.error;
        if (f.fit) {
            patch.report = f.fit->report;
            sewing.Add(f.fit->face.impl()->shape);
            ++result.faces;
        }
        result.patches.push_back(std::move(patch));
    }
    if (result.faces == 0)
        throw SurfaceFitError("No patch of the selection could be fit (" +
                              (result.patches.empty() ? std::string("empty") : result.patches.front().error) + ")");
    const int failed = static_cast<int>(result.patches.size()) - result.faces;
    if (failed > 0)
        result.warnings.push_back(std::to_string(failed) + " of " + std::to_string(result.patches.size()) +
                                  " patches could not be fit and were left out.");

    TopoDS_Shape shape;
    try {
        sewing.Perform();
        shape = sewing.SewedShape();
        result.freeEdges = sewing.NbFreeEdges();
    } catch (const Standard_Failure&) {
        shape.Nullify();
    }
    if (shape.IsNull())
        throw SurfaceFitError("Sewing the fitted patches failed");

    // Closed shells become solids: each one outward (positive volume); a shell inside
    // another one is a cavity of it. Separate parts give a compound of solids.
    if (result.freeEdges == 0) {
        try {
            struct Closed {
                TopoDS_Shell shell; // oriented outward
                TopoDS_Solid solid;
                double volume = 0.0;
                gp_Pnt point;
                std::vector<TopoDS_Shell> cavities;
                bool cavity = false;
            };
            std::vector<Closed> closed;
            bool ok = true;
            for (TopExp_Explorer ex(shape, TopAbs_SHELL); ex.More() && ok; ex.Next()) {
                Closed c;
                c.shell = TopoDS::Shell(ex.Current());
                BRepBuilderAPI_MakeSolid make(c.shell);
                ok = make.IsDone();
                if (!ok)
                    break;
                GProp_GProps props;
                BRepGProp::VolumeProperties(make.Solid(), props, 1e-6);
                if (props.Mass() < 0.0)
                    c.shell.Reverse();
                c.volume = std::abs(props.Mass());
                c.solid = BRepBuilderAPI_MakeSolid(c.shell).Solid();
                TopExp_Explorer vertex(c.shell, TopAbs_VERTEX);
                ok = vertex.More();
                if (ok)
                    c.point = BRep_Tool::Pnt(TopoDS::Vertex(vertex.Current()));
                closed.push_back(std::move(c));
            }
            ok = ok && !closed.empty();
            std::sort(closed.begin(), closed.end(),
                      [](const Closed& x, const Closed& y) { return x.volume > y.volume; });
            for (std::size_t k = 1; ok && k < closed.size(); ++k)
                for (std::size_t outer = 0; outer < k; ++outer) {
                    if (closed[outer].cavity)
                        continue;
                    BRepClass3d_SolidClassifier inside(closed[outer].solid, closed[k].point, result.sewTolerance);
                    if (inside.State() == TopAbs_IN) {
                        closed[k].cavity = true;
                        closed[outer].cavities.push_back(TopoDS::Shell(closed[k].shell.Reversed()));
                        break;
                    }
                }
            BRep_Builder builder;
            TopoDS_Compound compound;
            builder.MakeCompound(compound);
            int solids = 0;
            TopoDS_Shape single;
            double volume = 0.0;
            for (const auto& c : closed) {
                if (!ok || c.cavity)
                    continue;
                BRepBuilderAPI_MakeSolid make(c.shell);
                for (const auto& cavity : c.cavities)
                    make.Add(cavity);
                ShapeFix_Solid fix(make.Solid());
                fix.Perform();
                const TopoDS_Shape solid = fix.Solid();
                GProp_GProps props;
                BRepGProp::VolumeProperties(solid, props, 1e-6);
                ok = BRepCheck_Analyzer(solid).IsValid() && props.Mass() > 0.0;
                volume += props.Mass();
                builder.Add(compound, solid);
                single = solid;
                ++solids;
            }
            if (ok && solids > 0) {
                shape = solids == 1 ? single : TopoDS_Shape(compound);
                result.solid = true;
                result.volume = volume;
                if (solids > 1)
                    result.warnings.push_back("The mesh closes into " + std::to_string(solids) + " separate solids.");
            }
        } catch (const Standard_Failure&) {
            result.solid = false;
        }
        if (!result.solid)
            result.warnings.push_back("The shell is closed but could not be turned into a valid solid.");
    }
    result.shape = wrapShape(shape);
    result.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return result;
}

} // namespace aerocad::geometry
