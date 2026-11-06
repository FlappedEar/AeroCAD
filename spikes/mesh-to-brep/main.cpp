// SPDX-License-Identifier: GPL-3.0-or-later
// CAD-66 spike: turn a user-selected region of an imperfect mesh into a B-rep surface
// with Open CASCADE, then run typical CAD operations on it.
//
// Approaches compared on the same noisy mesh region:
//   A) Faceted B-rep: one planar face per triangle, sewn together (BRepBuilderAPI_Sewing).
//   B) NURBS fit: project onto the PCA plane, resample a regular height grid,
//      approximate with GeomAPI_PointsToBSplineSurface (tolerance-driven or smoothing).
//   C) GeomPlate: plate surface with point constraints, converted by GeomPlate_MakeApprox.
// The best B result is then trimmed to the selection, thickened, used as the "up to"
// target of a sketch extrusion, booleaned, filleted and exported to STEP and STL.
//
// Usage:
//   mesh_to_brep                          synthetic noisy mesh (Gaussian noise, sigma 0.1 mm)
//   mesh_to_brep file.stl cx cy cz r      real mesh, selection = sphere (cx,cy,cz), radius r
// Exit code is 0 only if the full CAD chain produced a valid solid and both exports succeeded.

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_PointsToBSplineSurface.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomPlate_BuildPlateSurface.hxx>
#include <GeomPlate_MakeApprox.hxx>
#include <GeomPlate_PointConstraint.hxx>
#include <GeomPlate_Surface.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_Plane.hxx>
#include <Poly_Triangulation.hxx>
#include <RWStl.hxx>
#include <STEPControl_Writer.hxx>
#include <StlAPI_Writer.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <math_Jacobi.hxx>
#include <math_Matrix.hxx>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Mesh {
    std::vector<gp_Pnt> v;
    std::vector<std::array<int, 3>> t;
};

// Ground-truth freeform surface used for the synthetic case.
double truthZ(double x, double y) {
    return 8.0 * std::sin(x / 18.0) * std::cos(y / 23.0) + 0.002 * x * x;
}
bool gSynthetic = false;

Mesh syntheticMesh(int n, double size, double sigma, unsigned seed) {
    Mesh m;
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, sigma);
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
            double x = -size / 2 + size * i / n;
            double y = -size / 2 + size * j / n;
            m.v.emplace_back(x + noise(rng), y + noise(rng), truthZ(x, y) + noise(rng));
        }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            int a = j * (n + 1) + i, b = a + 1, c = a + n + 1, d = c + 1;
            m.t.push_back({a, b, d});
            m.t.push_back({a, d, c});
        }
    return m;
}

Mesh fromTriangulation(const Handle(Poly_Triangulation) & tri) {
    Mesh m;
    for (int i = 1; i <= tri->NbNodes(); ++i) m.v.push_back(tri->Node(i));
    for (int i = 1; i <= tri->NbTriangles(); ++i) {
        int a, b, c;
        tri->Triangle(i).Get(a, b, c);
        m.t.push_back({a - 1, b - 1, c - 1});
    }
    return m;
}

// Stand-in for the UI brush: every triangle whose centroid lies inside sphere (c, r).
std::vector<int> selectTriangles(const Mesh& m, const gp_Pnt& c, double r) {
    std::vector<int> sel;
    for (int i = 0; i < (int)m.t.size(); ++i) {
        const auto& t = m.t[i];
        gp_XYZ g = (m.v[t[0]].XYZ() + m.v[t[1]].XYZ() + m.v[t[2]].XYZ()) / 3.0;
        if (gp_Pnt(g).Distance(c) <= r) sel.push_back(i);
    }
    return sel;
}

std::vector<gp_Pnt> selectedPoints(const Mesh& m, const std::vector<int>& sel) {
    std::vector<char> used(m.v.size(), 0);
    std::vector<gp_Pnt> pts;
    for (int ti : sel)
        for (int k : m.t[ti])
            if (!used[k]) {
                used[k] = 1;
                pts.push_back(m.v[k]);
            }
    return pts;
}

// Best-fit plane by PCA: Z = direction of least variance, X = direction of most variance.
gp_Ax2 pcaFrame(const std::vector<gp_Pnt>& pts) {
    gp_XYZ c(0, 0, 0);
    for (const auto& p : pts) c += p.XYZ();
    c /= (double)pts.size();
    math_Matrix cov(1, 3, 1, 3, 0.0);
    for (const auto& p : pts) {
        gp_XYZ d = p.XYZ() - c;
        double a[3] = {d.X(), d.Y(), d.Z()};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) cov(i + 1, j + 1) += a[i] * a[j];
    }
    math_Jacobi jac(cov);
    int imin = 1, imax = 1;
    for (int i = 2; i <= 3; ++i) {
        if (jac.Value(i) < jac.Value(imin)) imin = i;
        if (jac.Value(i) > jac.Value(imax)) imax = i;
    }
    math_Vector vn(1, 3), vx(1, 3);
    jac.Vector(imin, vn);
    jac.Vector(imax, vx);
    gp_Dir n(vn(1), vn(2), vn(3));
    if (n.Z() < 0) n.Reverse();  // only to keep "up" readable in the output
    return gp_Ax2(gp_Pnt(c), n, gp_Dir(vx(1), vx(2), vx(3)));
}

// Points in the local PCA frame: (u, v) in the plane, w = height along the normal.
struct LocalCloud {
    gp_Ax2 frame;
    std::vector<std::array<double, 3>> uvw;
    double half = 0;  // half-size of the square that contains the selection
};

LocalCloud toLocal(const std::vector<gp_Pnt>& pts, const gp_Ax2& fr) {
    LocalCloud lc;
    lc.frame = fr;
    gp_Vec X(fr.XDirection()), Y(fr.YDirection()), Z(fr.Direction());
    for (const auto& p : pts) {
        gp_Vec d(fr.Location(), p);
        double u = d.Dot(X), v = d.Dot(Y);
        lc.uvw.push_back({u, v, d.Dot(Z)});
        lc.half = std::max({lc.half, std::abs(u), std::abs(v)});
    }
    return lc;
}

// Uniform bucket grid over the local square for neighbour queries.
struct Buckets {
    int n;
    double half;
    std::vector<std::vector<int>> cells;
    Buckets(const LocalCloud& lc, int n_) : n(n_), half(lc.half), cells(n_ * n_) {
        for (int i = 0; i < (int)lc.uvw.size(); ++i) cells[idx(lc.uvw[i][1]) * n + idx(lc.uvw[i][0])].push_back(i);
    }
    int idx(double s) const { return std::clamp((int)((s + half) / (2 * half) * n), 0, n - 1); }
};

struct Deviation {
    double dataToSurfMax = 0, dataToSurfRms = 0;  // scan points -> surface
    double surfToDataMax = 0;                     // surface samples inside selection -> nearest scan point
    double truthMax = -1;                         // synthetic only: surface vs. noise-free ground truth
};

// Two-sided check. Data->surface alone misses wiggles and folds between data points,
// so the surface is also sampled and each sample is compared with the nearest scan point.
Deviation measure(const Handle(Geom_Surface) & s, const LocalCloud& lc, const std::vector<gp_Pnt>& pts,
                  double regionRadius) {
    Deviation d;
    GeomAPI_ProjectPointOnSurf proj;
    double sum = 0;
    int n = 0;
    size_t stride = std::max<size_t>(1, pts.size() / 3000);
    for (size_t i = 0; i < pts.size(); i += stride) {
        proj.Init(pts[i], s);
        if (proj.NbPoints() == 0) continue;
        double dist = proj.LowerDistance();
        d.dataToSurfMax = std::max(d.dataToSurfMax, dist);
        sum += dist * dist;
        ++n;
    }
    d.dataToSurfRms = n ? std::sqrt(sum / n) : 0;

    Buckets b(lc, 64);
    const gp_Ax2& fr = lc.frame;
    gp_Vec X(fr.XDirection()), Y(fr.YDirection()), Z(fr.Direction());
    double u0, u1, v0, v1;
    s->Bounds(u0, u1, v0, v1);
    for (int j = 0; j <= 80; ++j)
        for (int i = 0; i <= 80; ++i) {
            gp_Pnt p = s->Value(u0 + (u1 - u0) * i / 80, v0 + (v1 - v0) * j / 80);
            gp_Vec dv(fr.Location(), p);
            double u = dv.Dot(X), v = dv.Dot(Y), w = dv.Dot(Z);
            if (std::hypot(u, v) > regionRadius) continue;
            double best = 1e300;
            int iu = b.idx(u), iv = b.idx(v);
            for (int r = 1; r <= b.n && best == 1e300; ++r)
                for (int bj = std::max(0, iv - r); bj <= std::min(b.n - 1, iv + r); ++bj)
                    for (int bi = std::max(0, iu - r); bi <= std::min(b.n - 1, iu + r); ++bi)
                        for (int k : b.cells[bj * b.n + bi]) {
                            const auto& q = lc.uvw[k];
                            best = std::min(best, std::sqrt((q[0] - u) * (q[0] - u) + (q[1] - v) * (q[1] - v) +
                                                            (q[2] - w) * (q[2] - w)));
                        }
            d.surfToDataMax = std::max(d.surfToDataMax, best);
            if (gSynthetic) d.truthMax = std::max(d.truthMax, std::abs(p.Z() - truthZ(p.X(), p.Y())));
        }
    return d;
}

void printDeviation(const Deviation& d) {
    std::printf("    scan->surface max %.3f / RMS %.3f mm, surface->scan max %.3f mm", d.dataToSurfMax,
                d.dataToSurfRms, d.surfToDataMax);
    if (d.truthMax >= 0) std::printf(", vs. ground truth max %.3f mm", d.truthMax);
    std::printf("\n");
}

int countSubShapes(const TopoDS_Shape& s, TopAbs_ShapeEnum type) {
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(s, type, map);
    return map.Extent();
}

bool isValid(const TopoDS_Shape& s) { return !s.IsNull() && BRepCheck_Analyzer(s).IsValid(); }
const char* validStr(const TopoDS_Shape& s) { return isValid(s) ? "valid" : "INVALID"; }

// ---- A: faceted B-rep ---------------------------------------------------------
void approachFaceted(const Mesh& m, const std::vector<int>& sel) {
    auto t0 = Clock::now();
    BRepBuilderAPI_Sewing sew(1e-3);
    for (int ti : sel) {
        const auto& t = m.t[ti];
        BRepBuilderAPI_MakePolygon poly(m.v[t[0]], m.v[t[1]], m.v[t[2]], Standard_True);
        if (!poly.IsDone()) continue;
        BRepBuilderAPI_MakeFace f(poly.Wire(), Standard_True);
        if (f.IsDone()) sew.Add(f.Face());
    }
    sew.Perform();
    TopoDS_Shape shell = sew.SewedShape();
    double tSew = msSince(t0);

    // One CAD operation on the facets: split the shell with a cylinder.
    auto t1 = Clock::now();
    GProp_GProps g;
    BRepGProp::SurfaceProperties(shell, g);
    gp_Pnt c = g.CentreOfMass();
    TopoDS_Shape cyl =
        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(c.X(), c.Y(), c.Z() - 30), gp::DZ()), 5.0, 60.0).Shape();
    BRepAlgoAPI_Splitter split;
    TopTools_ListOfShape args, tools;
    args.Append(shell);
    tools.Append(cyl);
    split.SetArguments(args);
    split.SetTools(tools);
    split.Build();
    std::printf("[A] faceted: %zu triangles -> %d faces, sewing %.0f ms, boolean split %.0f ms (%s)\n", sel.size(),
                countSubShapes(shell, TopAbs_FACE), tSew, msSince(t1), split.IsDone() ? "ok" : "FAILED");
}

// ---- B: NURBS fit on a resampled height grid ----------------------------------
enum class FitMode { Tolerance, Smoothing };

Handle(Geom_BSplineSurface) approachGridFit(const LocalCloud& lc, const std::vector<gp_Pnt>& pts, int grid,
                                           FitMode mode, double tol3d, double regionRadius, Deviation* out) {
    auto t0 = Clock::now();
    const gp_Ax2& fr = lc.frame;
    gp_XYZ o = fr.Location().XYZ(), X = fr.XDirection().XYZ(), Y = fr.YDirection().XYZ(), Z = fr.Direction().XYZ();
    double half = lc.half;
    double h = 2 * half / (grid - 1);
    Buckets b(lc, grid);

    // Each node: inverse-distance-weighted mean height of scan points within 1.5 h (averages
    // noise). Nodes outside the data (corners of the square) grow the radius until they hit data.
    TColgp_Array2OfPnt arr(1, grid, 1, grid);
    for (int j = 0; j < grid; ++j)
        for (int i = 0; i < grid; ++i) {
            double u = -half + i * h, v = -half + j * h;
            double sw = 0, sh = 0;
            for (double rr = 1.5 * h; sw == 0 && rr < 4 * half; rr *= 1.5) {
                int i0 = b.idx(u - rr), i1 = b.idx(u + rr), j0 = b.idx(v - rr), j1 = b.idx(v + rr);
                for (int bj = j0; bj <= j1; ++bj)
                    for (int bi = i0; bi <= i1; ++bi)
                        for (int k : b.cells[bj * b.n + bi]) {
                            const auto& q = lc.uvw[k];
                            double d2 = (q[0] - u) * (q[0] - u) + (q[1] - v) * (q[1] - v);
                            if (d2 > rr * rr) continue;
                            double w = 1.0 / (d2 + 1e-6);
                            sw += w;
                            sh += w * q[2];
                        }
            }
            arr.SetValue(i + 1, j + 1, gp_Pnt(o + u * X + v * Y + (sh / sw) * Z));
        }
    double tGrid = msSince(t0);

    auto t1 = Clock::now();
    GeomAPI_PointsToBSplineSurface fit;
    if (mode == FitMode::Tolerance)
        fit.Init(arr, Approx_ChordLength, 3, 8, GeomAbs_C2, tol3d);
    else
        fit.Init(arr, 1e-3, 1e-4, 0.0, 8, GeomAbs_C2, tol3d);  // weights: length, curvature, torsion
    double tFit = msSince(t1);
    std::printf("[B] grid %dx%d, %s, tol %.2f mm: resample %.0f ms, fit %.0f ms", grid, grid,
                mode == FitMode::Tolerance ? "tolerance" : "smoothing", tol3d, tGrid, tFit);
    if (!fit.IsDone()) {
        std::printf(" -> FAILED\n");
        return nullptr;
    }
    Handle(Geom_BSplineSurface) s = fit.Surface();
    std::printf(", poles %dx%d\n", s->NbUPoles(), s->NbVPoles());
    Deviation d = measure(s, lc, pts, regionRadius);
    printDeviation(d);
    if (out) *out = d;
    return s;
}

// ---- C: GeomPlate with point constraints ---------------------------------------
void approachPlate(const LocalCloud& lc, const std::vector<gp_Pnt>& pts, int nConstraints, double regionRadius) {
    auto t0 = Clock::now();
    GeomPlate_BuildPlateSurface plate(3, 10, 3, 1e-5, 1e-3, 0.01, 0.1, Standard_False);
    plate.LoadInitSurface(new Geom_Plane(gp_Pln(gp_Ax3(lc.frame))));
    int stride = std::max<int>(1, (int)pts.size() / nConstraints);
    int added = 0;
    for (size_t i = 0; i < pts.size(); i += stride, ++added) plate.Add(new GeomPlate_PointConstraint(pts[i], 0, 0.05));
    plate.Perform();
    double tPlate = msSince(t0);
    if (!plate.IsDone()) {
        std::printf("[C] GeomPlate (%d points) FAILED after %.0f ms\n", added, tPlate);
        return;
    }
    auto t1 = Clock::now();
    GeomPlate_MakeApprox approx(plate.Surface(), 0.05, 16, 8, 0.1, 0, GeomAbs_C2);
    Handle(Geom_BSplineSurface) s = approx.Surface();
    if (s.IsNull()) {
        std::printf("[C] GeomPlate_MakeApprox FAILED\n");
        return;
    }
    std::printf("[C] GeomPlate, %d point constraints: plate %.0f ms, approx %.0f ms, poles %dx%d\n", added, tPlate,
                msSince(t1), s->NbUPoles(), s->NbVPoles());
    printDeviation(measure(s, lc, pts, regionRadius));
}

// ---- CAD operations on the fitted surface --------------------------------------
bool cadOnSurface(const Handle(Geom_BSplineSurface) & s, const gp_Ax2& fr, double radius, const std::string& outDir) {
    // 1. Trim the surface to the selection: common with a cylinder along the PCA normal.
    auto t0 = Clock::now();
    TopoDS_Face full = BRepBuilderAPI_MakeFace(s, 1e-6);
    gp_Ax2 cylAx(fr.Location().Translated(gp_Vec(fr.Direction()) * -50), fr.Direction());
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(cylAx, radius, 100).Shape();
    TopoDS_Shape trimmed = BRepAlgoAPI_Common(full, cyl).Shape();
    TopoDS_Face base;
    for (TopExp_Explorer ex(trimmed, TopAbs_FACE); ex.More(); ex.Next()) {
        base = TopoDS::Face(ex.Current());
        break;
    }
    std::printf("[CAD] trim to selection: %.0f ms, face %s\n", msSince(t0), base.IsNull() ? "MISSING" : validStr(base));
    if (base.IsNull()) return false;

    // 2. Thicken by 2 mm.
    auto t1 = Clock::now();
    BRepOffsetAPI_MakeThickSolid thick;
    thick.MakeThickSolidBySimple(base, 2.0);
    TopoDS_Shape plate = thick.IsDone() ? thick.Shape() : TopoDS_Shape();
    if (!plate.IsNull()) {
        GProp_GProps g;
        BRepGProp::VolumeProperties(plate, g);
        if (g.Mass() < 0) plate.Reverse();  // MakeThickSolidBySimple can return an inside-out solid
    }
    std::printf("[CAD] thicken 2 mm: %.0f ms, solid %s\n", msSince(t1), validStr(plate));
    if (!isValid(plate)) return false;

    // 3. Sketch a rectangle on a plane above the surface, extrude it down "up to surface"
    //    (split the prism with the surface, keep the upper piece), then fuse with the plate.
    auto t2 = Clock::now();
    gp_Pnt c = fr.Location();
    gp_Dir X = fr.XDirection(), Y = fr.YDirection(), Z = fr.Direction();
    double a = radius * 0.25, top = 25.0;
    auto P = [&](double u, double v) { return gp_Pnt(c.XYZ() + u * X.XYZ() + v * Y.XYZ() + top * Z.XYZ()); };
    BRepBuilderAPI_MakePolygon rect(P(-a, -a), P(a, -a), P(a, a), P(-a, a), Standard_True);
    TopoDS_Face sketch = BRepBuilderAPI_MakeFace(rect.Wire(), Standard_True);
    TopoDS_Shape prism = BRepPrimAPI_MakePrism(sketch, gp_Vec(Z) * -(top + 30)).Shape();
    BRepAlgoAPI_Splitter split;
    TopTools_ListOfShape args, tools;
    args.Append(prism);
    tools.Append(base);
    split.SetArguments(args);
    split.SetTools(tools);
    split.SetRunParallel(Standard_True);
    split.Build();
    TopoDS_Shape boss;
    double best = -1e300;
    for (TopExp_Explorer ex(split.Shape(), TopAbs_SOLID); ex.More(); ex.Next()) {
        GProp_GProps g;
        BRepGProp::VolumeProperties(ex.Current(), g);
        double hgt = gp_Vec(c, g.CentreOfMass()).Dot(gp_Vec(Z));
        if (hgt > best) {
            best = hgt;
            boss = ex.Current();
        }
    }
    double tSplit = msSince(t2);
    auto t2b = Clock::now();
    TopoDS_Shape part;
    if (!boss.IsNull()) {
        BRepAlgoAPI_Fuse fuse;
        TopTools_ListOfShape fa, ft;
        fa.Append(plate);
        ft.Append(boss);
        fuse.SetArguments(fa);
        fuse.SetTools(ft);
        fuse.SetRunParallel(Standard_True);
        fuse.SetFuzzyValue(1e-4);
        fuse.Build();
        if (fuse.IsDone()) part = fuse.Shape();
    }
    std::printf("[CAD] sketch + extrude up to surface: split %.0f ms, fuse %.0f ms, solid %s\n", tSplit,
                msSince(t2b), validStr(part));
    if (!isValid(part)) return false;

    // 4. Fillet the top edges of the extrusion (r = 1.5 mm).
    auto t3 = Clock::now();
    BRepFilletAPI_MakeFillet fillet(part);
    for (TopExp_Explorer ex(part, TopAbs_EDGE); ex.More(); ex.Next()) {
        TopoDS_Edge e = TopoDS::Edge(ex.Current());
        double h1 = gp_Vec(c, BRep_Tool::Pnt(TopExp::FirstVertex(e))).Dot(gp_Vec(Z));
        double h2 = gp_Vec(c, BRep_Tool::Pnt(TopExp::LastVertex(e))).Dot(gp_Vec(Z));
        if (std::abs(h1 - top) < 1e-3 && std::abs(h2 - top) < 1e-3) fillet.Add(1.5, e);
    }
    TopoDS_Shape filleted;
    try {
        fillet.Build();
        if (fillet.IsDone()) filleted = fillet.Shape();
    } catch (const Standard_Failure&) {
    }
    std::printf("[CAD] fillet 1.5 mm on %d edges: %.0f ms, solid %s\n", fillet.NbContours(), msSince(t3),
                validStr(filleted));
    if (!isValid(filleted)) return false;
    part = filleted;

    // 5. Export.
    auto t4 = Clock::now();
    STEPControl_Writer w;
    w.Transfer(part, STEPControl_AsIs);
    bool stepOk = w.Write((outDir + "/spike_part.step").c_str()) == IFSelect_RetDone;
    BRepMesh_IncrementalMesh(part, 0.05, Standard_False, 0.2);
    StlAPI_Writer stl;
    bool stlOk = stl.Write(part, (outDir + "/spike_part.stl").c_str());
    GProp_GProps g;
    BRepGProp::VolumeProperties(part, g);
    std::printf("[CAD] export STEP %s, STL %s (%.0f ms); part volume %.1f mm3, %d faces\n", stepOk ? "ok" : "FAILED",
                stlOk ? "ok" : "FAILED", msSince(t4), g.Mass(), countSubShapes(part, TopAbs_FACE));
    return stepOk && stlOk && g.Mass() > 0;
}

}  // namespace

int main(int argc, char** argv) {
    Mesh m;
    gp_Pnt center(0, 0, 0);
    double radius = 30.0;
    if (argc >= 2) {
        Handle(Poly_Triangulation) tri = RWStl::ReadFile(argv[1]);
        if (tri.IsNull()) {
            std::fprintf(stderr, "Cannot read %s\n", argv[1]);
            return 2;
        }
        m = fromTriangulation(tri);
        if (argc >= 6) {
            center = gp_Pnt(std::atof(argv[2]), std::atof(argv[3]), std::atof(argv[4]));
            radius = std::atof(argv[5]);
        }
    } else {
        m = syntheticMesh(300, 120.0, 0.1, 42);  // 180k triangles, 0.4 mm spacing
        gSynthetic = true;
    }
    std::printf("Mesh: %zu vertices, %zu triangles\n", m.v.size(), m.t.size());
    auto sel = selectTriangles(m, center, radius);
    auto pts = selectedPoints(m, sel);
    std::printf("Selection: sphere r = %.1f mm -> %zu triangles, %zu points\n\n", radius, sel.size(), pts.size());
    if (pts.size() < 16) return 2;

    LocalCloud lc = toLocal(pts, pcaFrame(pts));
    double region = radius * 0.9;  // evaluate away from the ragged selection border

    approachFaceted(m, sel);

    // Several grid/tolerance settings; keep the one with the smallest surface->scan error.
    struct Cfg {
        int grid;
        FitMode mode;
        double tol;
    };
    const Cfg cfgs[] = {{25, FitMode::Tolerance, 0.5}, {30, FitMode::Tolerance, 0.25}, {40, FitMode::Tolerance, 0.1},
                        {30, FitMode::Smoothing, 0.25}, {40, FitMode::Smoothing, 0.1}};
    Handle(Geom_BSplineSurface) bestSurf;
    double bestErr = 1e300;
    for (const auto& c : cfgs) {
        Deviation d;
        auto s = approachGridFit(lc, pts, c.grid, c.mode, c.tol, region, &d);
        if (!s.IsNull() && d.surfToDataMax < bestErr) {
            bestErr = d.surfToDataMax;
            bestSurf = s;
        }
    }
    approachPlate(lc, pts, 400, region);
    std::printf("\nCAD chain on the best B surface (surface->scan max %.3f mm):\n", bestErr);
    bool ok = !bestSurf.IsNull() && cadOnSurface(bestSurf, lc.frame, region, ".");
    std::printf("\nRESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
