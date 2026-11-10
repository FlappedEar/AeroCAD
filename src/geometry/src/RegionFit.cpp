// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/RegionFit.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Geom2d_Curve.hxx>
#include <ShapeFix_Solid.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>

#include <gp_Vec.hxx>
#include <math_Jacobi.hxx>
#include <math_Matrix.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <thread>
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

/// Largest angle (degrees) between the normals of two faces sharing `edge`, sampled
/// along it; -1 when it cannot be evaluated.
double seamAngle(const TopoDS_Edge& edge, const TopoDS_Face& a, const TopoDS_Face& b)
{
    double worst = -1.0;
    constexpr int samples = 12;
    for (int i = 1; i < samples; ++i) {
        gp_Vec normals[2];
        bool ok = true;
        const TopoDS_Face* faces[2] = {&a, &b};
        for (int k = 0; k < 2 && ok; ++k) {
            double f = 0.0, l = 0.0;
            const Handle(Geom2d_Curve) pcurve = BRep_Tool::CurveOnSurface(edge, *faces[k], f, l);
            if (pcurve.IsNull()) {
                ok = false;
                break;
            }
            const gp_Pnt2d uv = pcurve->Value(f + (l - f) * i / samples);
            const BRepAdaptor_Surface surface(*faces[k], false);
            gp_Pnt p;
            gp_Vec du, dv;
            surface.D1(uv.X(), uv.Y(), p, du, dv);
            normals[k] = du.Crossed(dv);
            ok = normals[k].Magnitude() > 1e-12;
        }
        if (!ok)
            continue;
        // Orientation-free: a smooth seam has (anti)parallel normals either way.
        const double c = std::abs(normals[0].Normalized().Dot(normals[1].Normalized()));
        worst = std::max(worst, std::acos(std::min(1.0, c)) * 180.0 / std::numbers::pi);
    }
    return worst;
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
    // The original smooth region of every triangle (splits stay inside it).
    const std::vector<std::int64_t> regionOfTriangle = label;
    // `stop`: polled inside each patch fit, so a cancel does not wait for a large patch.
    auto fitWith = [&](Fitted& f, const std::function<bool()>& stop) {
        f.fit.reset();
        f.error.clear();
        SurfaceFitOptions o = fitOptions;
        o.outlineCorners = f.corners;
        o.cancelled = stop;
        // Data across smooth seams: grow into the same region only (never over a crease).
        core::TriangleSet data = f.triangles;
        if (options.blendRings > 0) {
            const auto own = regionOfTriangle[f.triangles.front()];
            data.clear();
            for (const auto t : core::growSelection(adjacency, f.triangles, options.blendRings))
                if (regionOfTriangle[t] == own)
                    data.push_back(t);
            if (data.size() > f.triangles.size())
                o.trimTo = f.triangles;
            else
                data = f.triangles;
        }
        try {
            f.fit = fitSurface(mesh, data, o);
        } catch (const SurfaceFitError& e) {
            f.error = e.what();
        } catch (const Standard_Failure& failure) {
            f.error = failure.GetMessageString() != nullptr ? failure.GetMessageString() : "fit failed";
        }
        if (!o.trimTo.empty() && !(f.fit && (f.fit->report.withinTolerance || f.fit->report.noiseLimited))) {
            // The grown data may not unfold (it can close a tube again): fit on its own.
            o.trimTo.clear();
            std::optional<SurfaceFitResult> plain;
            try {
                plain = fitSurface(mesh, f.triangles, o);
            } catch (const SurfaceFitError&) {
            } catch (const Standard_Failure&) {
            }
            if (plain && (!f.fit || plain->report.withinTolerance)) {
                f.fit = std::move(plain);
                f.error.clear();
            }
        }
    };
    // Patches are independent: they are fit on several threads at once, in waves (the
    // halves of a split patch go to the next wave). Progress and cancel run between patches
    // on this thread.
    const int threads = std::max(1, options.threads > 0 ? options.threads
                                                       : static_cast<int>(std::thread::hardware_concurrency()));
    int done = 0;
    int known = static_cast<int>(regions.size());
    auto checkCancel = [&] {
        if (options.cancelled && options.cancelled())
            throw SurfaceFitError("Cancelled");
    };
    auto report = [&] {
        if (options.progress)
            options.progress(done, known);
    };
    auto fitAll = [&](std::vector<Fitted*>& batch) {
        std::atomic<std::size_t> next{0};
        std::atomic<int> finished{0};
        std::atomic<bool> stop{false};
        const std::function<bool()> stopped = [&stop] { return stop.load(); };
        // Only this thread calls options.cancelled.
        const std::function<bool()> stopHere = [&] {
            if (!stop && options.cancelled && options.cancelled())
                stop = true;
            return stop.load();
        };
        auto work = [&] {
            for (std::size_t i = next++; i < batch.size() && !stop; i = next++) {
                fitWith(*batch[i], stopped);
                ++finished;
            }
        };
        std::vector<std::thread> pool;
        const auto n = std::min<std::size_t>(static_cast<std::size_t>(threads), batch.size());
        for (std::size_t k = 1; k < n; ++k)
            pool.emplace_back(work);
        // This thread fits too, and reports progress and polls cancel after each patch.
        const int before = done;
        for (std::size_t i = next++; i < batch.size() && !stopHere(); i = next++) {
            fitWith(*batch[i], stopHere);
            ++finished;
            done = before + finished.load();
            report();
        }
        // The other threads may still be fitting: keep reporting and polling until they finish.
        while (finished.load() < static_cast<int>(std::min(batch.size(), next.load())) && !stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            done = before + finished.load();
            report();
            stopHere();
        }
        for (auto& t : pool)
            t.join();
        done = before + finished.load();
        report();
        if (stop)
            throw SurfaceFitError("Cancelled");
    };
    report();
    std::vector<std::pair<core::TriangleSet, int>> wave;
    for (const auto& region : regions)
        wave.emplace_back(region, 0);
    std::vector<Fitted> fitted;
    bool split = false;
    while (!wave.empty()) {
        checkCancel();
        std::vector<Fitted> batch(wave.size());
        std::vector<Fitted*> work;
        for (std::size_t k = 0; k < wave.size(); ++k) {
            batch[k].triangles = std::move(wave[k].first);
            batch[k].corners = patchCorners(mesh, adjacency, label, batch[k].triangles);
            work.push_back(&batch[k]);
        }
        fitAll(work);
        std::vector<std::pair<core::TriangleSet, int>> nextWave;
        for (std::size_t k = 0; k < batch.size(); ++k) {
            Fitted& f = batch[k];
            const int depth = wave[k].second;
            // Splitting does not help against scan noise: smaller patches scatter as much.
            const bool good = f.fit && (f.fit->report.withinTolerance || f.fit->report.noiseLimited);
            if (!good && depth < 3 && f.triangles.size() >= 64) {
                auto halves = bisect(mesh, adjacency, f.triangles);
                if (halves.size() >= 2) {
                    split = true;
                    // The fit of the whole stays done; its halves are more work.
                    known += static_cast<int>(halves.size());
                    for (auto& h : halves)
                        nextWave.emplace_back(std::move(h), depth + 1);
                    continue;
                }
            }
            fitted.push_back(std::move(f));
        }
        wave = std::move(nextWave);
        report();
    }
    if (fitted.size() > regions.size())
        result.warnings.push_back("Split " + std::to_string(fitted.size() - regions.size()) +
                                  " extra times to fit tubes or strongly bent regions.");
    // After splits, patches next to a cut have new corners: refit those.
    if (split) {
        for (std::size_t k = 0; k < fitted.size(); ++k)
            for (const auto t : fitted[k].triangles)
                label[t] = static_cast<std::int64_t>(k);
        std::vector<Fitted*> refit;
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
                refit.push_back(&f);
            }
        }
        if (!refit.empty()) {
            // Refits count as more work: progress never goes back.
            done = static_cast<int>(fitted.size());
            known = static_cast<int>(fitted.size() + refit.size());
            fitAll(refit);
        }
    }

    BRepBuilderAPI_Sewing sewing(result.sewTolerance);
    std::vector<std::pair<TopoDS_Shape, std::int64_t>> faceRegion; // input face, its smooth region
    for (auto& f : fitted) {
        if (f.fit)
            faceRegion.emplace_back(f.fit->face.impl()->shape, regionOfTriangle[f.triangles.front()]);
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

    // Smoothness across seams inside a region (where a region was split).
    try {
        std::vector<std::pair<TopoDS_Shape, std::int64_t>> sewnRegion;
        for (const auto& [input, r] : faceRegion)
            sewnRegion.emplace_back(sewing.Modified(input), r);
        auto regionOf = [&](const TopoDS_Shape& face) -> std::int64_t {
            for (const auto& [sewn, r] : sewnRegion)
                if (sewn.IsSame(face))
                    return r;
            return -1;
        };
        TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
        TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
        for (int i = 1; i <= edgeFaces.Extent(); ++i) {
            const TopTools_ListOfShape& faces = edgeFaces.FindFromIndex(i);
            if (faces.Extent() != 2 || faces.First().IsSame(faces.Last()))
                continue;
            const auto r = regionOf(faces.First());
            if (r < 0 || r != regionOf(faces.Last()))
                continue;
            const double angle =
                seamAngle(TopoDS::Edge(edgeFaces.FindKey(i)), TopoDS::Face(faces.First()), TopoDS::Face(faces.Last()));
            if (angle < 0.0)
                continue;
            ++result.smoothSeams;
            result.seamAngleMaxDeg = std::max(result.seamAngleMaxDeg, angle);
        }
    } catch (const Standard_Failure&) {
        // leave the seam measure out
    }

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
