// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Tessellation.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <TopoDS_Edge.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>

namespace aerocad::geometry {

core::TriangleMesh tessellate(const Shape& shape, const TessellationParams& params)
{
    core::TriangleMesh mesh;
    if (shape.isNull())
        return mesh;

    const TopoDS_Shape& s = shape.impl()->shape;
    const double diag = shape.bounds().diagonal();
    const double linear = std::max(diag * params.relativeLinearDeflection, 1e-4);
    BRepMesh_IncrementalMesh mesher(s, linear, false, params.angularDeflection, true);

    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face& face = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        const opencascade::handle<Poly_Triangulation> tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull())
            continue;

        const gp_Trsf trsf = loc.Transformation();
        const bool reversed = face.Orientation() == TopAbs_REVERSED;
        const auto base = static_cast<std::uint32_t>(mesh.vertexCount());

        for (int i = 1; i <= tri->NbNodes(); ++i) {
            const gp_Pnt p = tri->Node(i).Transformed(trsf);
            mesh.addVertex({p.X(), p.Y(), p.Z()});
        }
        for (int i = 1; i <= tri->NbTriangles(); ++i) {
            int n1 = 0, n2 = 0, n3 = 0;
            tri->Triangle(i).Get(n1, n2, n3);
            if (reversed)
                std::swap(n2, n3);
            mesh.addTriangle(base + static_cast<std::uint32_t>(n1 - 1),
                             base + static_cast<std::uint32_t>(n2 - 1),
                             base + static_cast<std::uint32_t>(n3 - 1));
        }
    }
    return mesh;
}

std::vector<core::Vec3> edgeSegments(const Shape& shape, const TessellationParams& params)
{
    std::vector<core::Vec3> out;
    if (shape.isNull())
        return out;
    const double diag = shape.bounds().diagonal();
    const double linear = std::max(diag * params.relativeLinearDeflection, 1e-4);
    for (TopExp_Explorer ex(shape.impl()->shape, TopAbs_EDGE); ex.More(); ex.Next()) {
        const TopoDS_Edge& edge = TopoDS::Edge(ex.Current());
        if (BRep_Tool::Degenerated(edge))
            continue;
        const BRepAdaptor_Curve curve(edge);
        const GCPnts_TangentialDeflection points(curve, params.angularDeflection / 2, linear);
        for (int i = 2; i <= points.NbPoints(); ++i) {
            const gp_Pnt a = points.Value(i - 1);
            const gp_Pnt b = points.Value(i);
            out.push_back({a.X(), a.Y(), a.Z()});
            out.push_back({b.X(), b.Y(), b.Z()});
        }
    }
    return out;
}

} // namespace aerocad::geometry
