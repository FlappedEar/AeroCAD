// SPDX-License-Identifier: GPL-3.0-or-later
// STEP writer via Open CASCADE.
#include "aerocad/io/Export.hpp"

#include "ReaderUtil.hpp"
#include "WriterUtil.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Failure.hxx>

#include <mutex>
#include <string>
#include <system_error>

namespace aerocad::io {

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
            const IFSelect_ReturnStatus status = writer.Transfer(item.shape->impl()->shape, STEPControl_AsIs);
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
