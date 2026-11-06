// SPDX-License-Identifier: GPL-3.0-or-later
// STEP writer via Open CASCADE.
#include "aerocad/io/Export.hpp"

#include "ReaderUtil.hpp"
#include "WriterUtil.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRep_Tool.hxx>
#include <Geom_SurfaceOfRevolution.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeCustom.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

#include <mutex>
#include <string>
#include <system_error>

namespace aerocad::io {

namespace {

/// True when a face lies on a general surface of revolution and closes at a pole on the
/// axis (a degenerated edge), e.g. a dome revolved from an arc that ends on the axis.
bool hasRevolutionPole(const TopoDS_Shape& shape)
{
    for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
        const TopoDS_Face& face = TopoDS::Face(f.Current());
        if (BRep_Tool::Surface(face).IsNull() ||
            !BRep_Tool::Surface(face)->IsKind(STANDARD_TYPE(Geom_SurfaceOfRevolution)))
            continue;
        for (TopExp_Explorer e(face, TopAbs_EDGE); e.More(); e.Next())
            if (BRep_Tool::Degenerated(TopoDS::Edge(e.Current())))
                return true;
    }
    return false;
}

/// Shape as it goes into the STEP file. OCCT (7.6 to at least 7.8) writes a pole of a
/// surface of revolution without its degenerated edge, and reading such a file back loses
/// the face (a revolved dome comes back as an open, flat shell). Those shapes get their
/// surfaces of revolution converted to exact B-spline surfaces first; planes, cylinders
/// and the other analytic surfaces stay as they are.
TopoDS_Shape stepShape(const TopoDS_Shape& shape)
{
    if (!hasRevolutionPole(shape))
        return shape;
    return ShapeCustom::ConvertToBSpline(shape, Standard_False, Standard_True, Standard_False);
}

} // namespace

void writeStep(const std::filesystem::path& path, const std::vector<ExportItem>& items)
{
    if (items.empty())
        throw ExportError("Nothing to export");
    std::string missing;
    for (const auto& item : items) {
        if (item.shape == nullptr || item.shape->isNull() || item.shape->impl() == nullptr)
            missing += (missing.empty() ? "'" : ", '") + item.name + "'";
    }
    if (!missing.empty())
        throw ExportError("STEP needs exact (B-rep) geometry; mesh-only parts cannot be written: " + missing +
                          ". Export them as STL or 3MF instead.");

    const std::string name = core::pathToUtf8(path);
    const std::filesystem::path tmp = detail::temporarySibling(path);
    const std::string tmpName = core::pathToUtf8(tmp);
    try {
        const std::lock_guard<std::mutex> lock(detail::occtMutex());
        STEPControl_Writer writer;
        Interface_Static::SetCVal("write.step.unit", "MM");
        Interface_Static::SetCVal("write.step.schema", "AP214IS");
        for (const auto& item : items) {
            const IFSelect_ReturnStatus status = writer.Transfer(stepShape(item.shape->impl()->shape), STEPControl_AsIs);
            if (status != IFSelect_RetDone)
                throw ExportError("Cannot convert '" + item.name + "' to STEP");
        }
        // OCCT accepts UTF-8 file names on every platform.
        if (writer.Write(tmpName.c_str()) != IFSelect_RetDone)
            throw ExportError("Cannot write STEP file: " + name);
    } catch (const Standard_Failure& e) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        const char* msg = e.GetMessageString();
        throw ExportError("Failed to write STEP file " + name + ": " + (msg && *msg ? msg : e.DynamicType()->Name()));
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        throw;
    }
    detail::commitTemporary(tmp, path);
}

} // namespace aerocad::io
