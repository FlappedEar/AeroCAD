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
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_PrinterOStream.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPControl_Reader.hxx>
#include <TCollection_AsciiString.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <string>

namespace aerocad::io {

namespace detail {

namespace {

/// OCCT's default messenger prints to stdout, and the STEP/IGES translators report
/// transfer statistics there. stdout carries command results (aerocad-cmd) and the MCP
/// stdio protocol, so send OCCT messages to stderr, warnings and above only.
void routeOcctMessagesToStderr()
{
    const opencascade::handle<Message_Messenger>& messenger = Message::DefaultMessenger();
    messenger->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));
    messenger->AddPrinter(new Message_PrinterOStream("cerr", false, Message_Warning));
}

} // namespace

std::mutex& occtMutex()
{
    static std::mutex mutex;
    static const bool configured = (routeOcctMessagesToStderr(), true);
    (void)configured;
    return mutex;
}

} // namespace detail

namespace {

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
template <typename F> ImportResult guarded(FileFormat format, const std::filesystem::path& path, F&& body)
{
    const std::string name = detail::pathToUtf8(path);
    try {
        const std::lock_guard<std::mutex> lock(detail::occtMutex());
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

/// A body of an XDE document: a simple (non-assembly) shape placed in the model.
struct StepLeaf {
    std::string name;
    std::string color;
    TopoDS_Shape shape;
};

std::string labelName(const TDF_Label& label)
{
    Handle(TDataStd_Name) name;
    if (!label.FindAttribute(TDataStd_Name::GetID(), name))
        return {};
    // Converts to UTF-8.
    const std::string text = TCollection_AsciiString(name->Get()).ToCString();
    // OCCT's own placeholder when a writer had no name (also in files AeroCAD wrote before
    // CAD-227): not a name.
    if (text.rfind("Open CASCADE STEP translator", 0) == 0)
        return {};
    return text;
}

std::string labelColor(const Handle(XCAFDoc_ColorTool)& colors, const TDF_Label& label)
{
    Quantity_Color c;
    if (!colors->GetColor(label, XCAFDoc_ColorGen, c) && !colors->GetColor(label, XCAFDoc_ColorSurf, c))
        return {};
    double r = 0;
    double g = 0;
    double b = 0;
    c.Values(r, g, b, Quantity_TOC_sRGB);
    const auto byte = [](double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    char text[8];
    std::snprintf(text, sizeof text, "#%02x%02x%02x", byte(r), byte(g), byte(b));
    return text;
}

/// Walks an assembly down to its simple shapes, composing the placements. Instance names
/// and colours win over the product's.
void collectLeaves(const Handle(XCAFDoc_ShapeTool)& shapes, const Handle(XCAFDoc_ColorTool)& colors,
                   const TDF_Label& label, const TopLoc_Location& parent, const std::string& parentColor,
                   std::vector<StepLeaf>& out)
{
    TDF_Label ref = label;
    TopLoc_Location location = parent;
    std::string color = labelColor(colors, label);
    std::string name;
    if (XCAFDoc_ShapeTool::IsReference(label)) {
        XCAFDoc_ShapeTool::GetReferredShape(label, ref);
        location = parent * XCAFDoc_ShapeTool::GetLocation(label);
        if (color.empty())
            color = labelColor(colors, ref);
    }
    if (color.empty())
        color = parentColor;
    if (XCAFDoc_ShapeTool::IsAssembly(ref)) {
        TDF_LabelSequence components;
        XCAFDoc_ShapeTool::GetComponents(ref, components);
        for (Standard_Integer i = 1; i <= components.Length(); ++i)
            collectLeaves(shapes, colors, components.Value(i), location, color, out);
        return;
    }
    const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(ref);
    if (shape.IsNull())
        return;
    name = labelName(ref);
    if (name.empty() && ref != label)
        name = labelName(label);
    out.push_back({name, color, shape.Moved(location)});
}

/// Reads a STEP file through XDE, which keeps product names and colours.
ImportResult readStepXde(std::istream& in, const std::string& name)
{
    Handle(TDocStd_Document) doc;
    XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", doc);
    struct CloseDocument {
        Handle(TDocStd_Document)& doc;
        ~CloseDocument() { XCAFApp_Application::GetApplication()->Close(doc); }
    } closeDocument{doc};

    STEPCAFControl_Reader reader;
    reader.SetNameMode(Standard_True);
    reader.SetColorMode(Standard_True);
    const IFSelect_ReturnStatus status = reader.ChangeReader().ReadStream(name.c_str(), in);
    if (status != IFSelect_RetDone)
        throw ImportError("Cannot read STEP file " + name + " (" + statusText(status) + ")");
    if (!reader.Transfer(doc))
        throw ImportError("STEP file contains no transferable shapes: " + name);

    const Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    const Handle(XCAFDoc_ColorTool) colors = XCAFDoc_DocumentTool::ColorTool(doc->Main());
    TDF_LabelSequence roots;
    shapes->GetFreeShapes(roots);
    std::vector<StepLeaf> leaves;
    for (Standard_Integer i = 1; i <= roots.Length(); ++i)
        collectLeaves(shapes, colors, roots.Value(i), TopLoc_Location(), {}, leaves);
    if (leaves.empty())
        throw ImportError("STEP file contains no shapes: " + name);

    if (leaves.size() == 1) {
        ImportResult result = finish(leaves.front().shape, FileFormat::Step, name);
        result.color = leaves.front().color;
        return result;
    }
    ImportResult result;
    result.format = FileFormat::Step;
    TopoDS_Compound all;
    BRep_Builder builder;
    builder.MakeCompound(all);
    for (auto& leaf : leaves) {
        ImportBody body;
        body.name = leaf.name;
        body.color = leaf.color;
        body.shape = geometry::wrapShape(leaf.shape);
        body.mesh = geometry::tessellate(*body.shape);
        if (body.mesh.isEmpty())
            continue; // e.g. a lone wire or point: nothing to show
        builder.Add(all, leaf.shape);
        result.mesh.append(body.mesh);
        result.bodies.push_back(std::move(body));
    }
    if (result.bodies.empty())
        throw ImportError("STEP file contains no surfaces to display: " + name);
    result.shape = geometry::wrapShape(all);
    if (result.bodies.size() == 1) {
        result.color = result.bodies.front().color;
        result.bodies.clear();
    }
    return result;
}

} // namespace

ImportResult readStep(const std::filesystem::path& path)
{
    return guarded(FileFormat::Step, path, [&](const std::string& name) {
        // Read through a stream so non-ASCII paths work on every platform.
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw ImportError("Cannot open file: " + name);
        // Output length unit; this is OCCT's default, set explicitly so the result
        // does not depend on what other code changed in the global parameters.
        Interface_Static::SetCVal("xstep.cascade.unit", "MM");
        return readStepXde(in, name);
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
