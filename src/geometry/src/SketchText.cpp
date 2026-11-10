// SPDX-License-Identifier: GPL-3.0-or-later
// Sketch text (Fusion: Create > Text): the letters of a font as planar faces.
#include "SketchText.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_UniformDeflection.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>
#include <gp_Trsf.hxx>

#if defined(AEROCAD_HAVE_FONTS)
#include <Font_BRepFont.hxx>
#include <Font_BRepTextBuilder.hxx>
#include <NCollection_String.hxx>
#endif

#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>

namespace aerocad::geometry {

namespace {

#if defined(AEROCAD_HAVE_FONTS)
// Letters are built once per font, style and text at this size and scaled to the height
// asked for (loading a font and building its glyphs is the slow part).
constexpr double kNominalSize = 100.0;

using Key = std::tuple<std::string, bool, bool, std::string>;

std::vector<TopoDS_Face> nominalFaces(const SketchCurve& c)
{
    static std::mutex mutex;
    static std::map<Key, std::vector<TopoDS_Face>> cache;
    static std::map<std::tuple<std::string, bool, bool>, std::shared_ptr<Font_BRepFont>> fonts;
    const Key key{c.font, c.bold, c.italic, c.text};
    std::lock_guard lock(mutex);
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    auto& font = fonts[{c.font, c.bold, c.italic}];
    if (!font) {
        auto f = std::make_shared<Font_BRepFont>();
        const Font_FontAspect aspect = c.bold && c.italic ? Font_FontAspect_BoldItalic
                                       : c.bold           ? Font_FontAspect_Bold
                                       : c.italic         ? Font_FontAspect_Italic
                                                          : Font_FontAspect_Regular;
        const std::string family = c.font.empty() ? std::string("sans-serif") : c.font;
        if (!f->FindAndInit(family.c_str(), aspect, kNominalSize, Font_StrictLevel_Any))
            throw ModelingError("Font '" + family + "' was not found");
        font = f;
    }
    Font_BRepTextBuilder builder;
    const TopoDS_Shape shape = builder.Perform(*font, NCollection_String(c.text.c_str()), gp_Ax3());
    std::vector<TopoDS_Face> faces;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
        faces.push_back(TopoDS::Face(ex.Current()));
    if (cache.size() > 256)
        cache.clear();
    cache[key] = faces;
    return faces;
}
#endif

/// Baseline start, direction (unit) and height of a Text curve.
struct TextFrame {
    Vec2 at, u;
    double height = 0.0;
};

TextFrame frameOf(const SketchCurve& c)
{
    if (c.points.size() < 2)
        throw ModelingError("Sketch text needs a start point and a height");
    TextFrame f;
    f.at = c.points[0];
    const Vec2 d{c.points[1].x - f.at.x, c.points[1].y - f.at.y};
    f.height = std::hypot(d.x, d.y);
    if (!(f.height > 1e-6))
        throw ModelingError("Sketch text height must be positive");
    f.u = {d.x / f.height, d.y / f.height};
    return f;
}

gp_Pnt pnt(const core::Vec3& v)
{
    return {v.x, v.y, v.z};
}

gp_Dir dir(const core::Vec3& v)
{
    return {v.x, v.y, v.z};
}

} // namespace

bool textSupported()
{
#if defined(AEROCAD_HAVE_FONTS)
    return true;
#else
    return false;
#endif
}

namespace detail {

std::vector<TopoDS_Face> textFaces(const Plane& plane, const SketchCurve& c)
{
#if defined(AEROCAD_HAVE_FONTS)
    const TextFrame f = frameOf(c);
    if (c.text.find_first_not_of(" \t\r\n") == std::string::npos)
        throw ModelingError("Sketch text is empty");
    try {
        const std::vector<TopoDS_Face> nominal = nominalFaces(c);
        // Font units (x along the baseline, y up, at the origin) to the sketch plane.
        const core::Vec3 x = plane.xDir * f.u.x + plane.yDir() * f.u.y;
        gp_Trsf place;
        place.SetTransformation(gp_Ax3(pnt(plane.toWorld(f.at)), dir(plane.normal), dir(x)), gp_Ax3());
        gp_Trsf scale;
        scale.SetScale(gp_Pnt(0, 0, 0), f.height / kNominalSize);
        const gp_Trsf t = place * scale;
        std::vector<TopoDS_Face> out;
        for (const auto& face : nominal)
            out.push_back(TopoDS::Face(BRepBuilderAPI_Transform(face, t, true).Shape()));
        if (out.empty())
            throw ModelingError("Sketch text has no letters in this font");
        return out;
    } catch (const Standard_Failure& e) {
        throw ModelingError(std::string("Sketch text failed: ") + e.GetMessageString());
    }
#else
    (void)plane;
    (void)c;
    throw ModelingError("Sketch text is not available in this build (Open CASCADE without fonts)");
#endif
}

} // namespace detail

std::vector<std::vector<Vec2>> textOutlines(const SketchCurve& c)
{
    const Plane xy;
    const auto faces = detail::textFaces(xy, c);
    const double deflection = std::max(frameOf(c).height * 2e-3, 1e-6);
    std::vector<std::vector<Vec2>> out;
    for (const auto& face : faces) {
        for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
            for (TopExp_Explorer e(w.Current(), TopAbs_EDGE); e.More(); e.Next()) {
                const TopoDS_Edge edge = TopoDS::Edge(e.Current());
                if (BRep_Tool::Degenerated(edge))
                    continue;
                BRepAdaptor_Curve curve(edge);
                GCPnts_UniformDeflection sample(curve, deflection);
                std::vector<Vec2> line;
                if (sample.IsDone() && sample.NbPoints() >= 2) {
                    for (int i = 1; i <= sample.NbPoints(); ++i) {
                        const gp_Pnt p = sample.Value(i);
                        line.push_back({p.X(), p.Y()});
                    }
                } else {
                    const gp_Pnt a = curve.Value(curve.FirstParameter()), b = curve.Value(curve.LastParameter());
                    line = {{a.X(), a.Y()}, {b.X(), b.Y()}};
                }
                out.push_back(std::move(line));
            }
        }
    }
    return out;
}

} // namespace aerocad::geometry
