// SPDX-License-Identifier: GPL-3.0-or-later
// B-rep CAD formats via Open CASCADE: STEP, IGES and native BREP.
#include "aerocad/io/Import.hpp"

#include "ReaderUtil.hpp"

#include "aerocad/geometry/OcctShape.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESControl_Reader.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Reader.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>

#include <fstream>
#include <mutex>
#include <string>

namespace aerocad::io {

namespace {

// The OCCT data exchange readers share global state (Interface_Static parameters,
// static controllers), so serialize them.
std::mutex& occtMutex()
{
    static std::mutex mutex;
    return mutex;
}

const char* statusText(IFSelect_ReturnStatus status)
{
    switch (status) {
    case IFSelect_RetVoid:
        return "nothing to read";
    case IFSelect_RetDone:
        return "ok";
    case IFSelect_RetError:
        return "file not readable or not recognized";
    case IFSelect_RetFail:
        return "read failed";
    case IFSelect_RetStop:
        return "read aborted";
    }
    return "unknown status";
}

ImportResult finish(const TopoDS_Shape& shape, FileFormat format, const std::string& name)
{
    if (shape.IsNull())
        throw ImportError(std::string(formatName(format)) + " file contains no shapes: " + name);
    ImportResult result;
    result.format = format;
    result.shape = geometry::wrapShape(shape);
    result.mesh = geometry::tessellate(*result.shape);
    if (result.mesh.isEmpty())
        throw ImportError(std::string(formatName(format)) + " file contains no surfaces to display: " + name);
    return result;
}

/// Runs an OCCT reader, converting OCCT exceptions into ImportError.
template <typename F>
ImportResult guarded(FileFormat format, const std::filesystem::path& path, F&& body)
{
    const std::string name = detail::pathToUtf8(path);
    try {
        const std::lock_guard<std::mutex> lock(occtMutex());
        return body(name);
    } catch (const ImportError&) {
        throw;
    } catch (const Standard_Failure& e) {
        const char* msg = e.GetMessageString();
        throw ImportError("Failed to read " + std::string(formatName(format)) + " file " + name + ": " +
                          (msg && *msg ? msg : e.DynamicType()->Name()));
    }
}

template <typename Reader>
TopoDS_Shape transfer(Reader& reader, IFSelect_ReturnStatus status, FileFormat format, const std::string& name)
{
    if (status != IFSelect_RetDone)
        throw ImportError("Cannot read " + std::string(formatName(format)) + " file " + name + " (" +
                          statusText(status) + ")");
    if (reader.TransferRoots() <= 0 || reader.NbShapes() <= 0)
        throw ImportError(std::string(formatName(format)) + " file contains no transferable shapes: " + name);
    return reader.OneShape();
}

} // namespace

ImportResult readStep(const std::filesystem::path& path)
{
    return guarded(FileFormat::Step, path, [&](const std::string& name) {
        // Read through a stream so non-ASCII paths work on every platform.
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw ImportError("Cannot open file: " + name);
        STEPControl_Reader reader;
        // Output length unit; this is OCCT's default, set explicitly so the result
        // does not depend on what other code changed in the global parameters.
        Interface_Static::SetCVal("xstep.cascade.unit", "MM");
        const IFSelect_ReturnStatus status = reader.ReadStream(name.c_str(), in);
        return finish(transfer(reader, status, FileFormat::Step, name), FileFormat::Step, name);
    });
}

ImportResult readIges(const std::filesystem::path& path)
{
    return guarded(FileFormat::Iges, path, [&](const std::string& name) {
        IGESControl_Reader reader;
        Interface_Static::SetCVal("xstep.cascade.unit", "MM");
        // The IGES reader does not support streams in OCCT 7.6; OCCT accepts UTF-8 paths.
        const IFSelect_ReturnStatus status = reader.ReadFile(name.c_str());
        return finish(transfer(reader, status, FileFormat::Iges, name), FileFormat::Iges, name);
    });
}

ImportResult readBrep(const std::filesystem::path& path)
{
    return guarded(FileFormat::Brep, path, [&](const std::string& name) {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw ImportError("Cannot open file: " + name);
        TopoDS_Shape shape;
        BRep_Builder builder;
        BRepTools::Read(shape, in, builder);
        if (shape.IsNull())
            throw ImportError("Not a valid BREP file: " + name);
        return finish(shape, FileFormat::Brep, name);
    });
}

} // namespace aerocad::io
