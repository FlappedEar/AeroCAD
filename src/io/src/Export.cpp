// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/io/Export.hpp"

#include "WriterUtil.hpp"

#include "aerocad/core/Path.hpp"

#include <charconv>
#include <cmath>
#include <fstream>
#include <locale>
#include <sstream>
#include <system_error>

namespace aerocad::io {

namespace detail {

std::filesystem::path temporarySibling(const std::filesystem::path& target)
{
    std::filesystem::path tmp = target;
    tmp += ".aerocad-tmp";
    return tmp;
}

void commitTemporary(const std::filesystem::path& temporary, const std::filesystem::path& target)
{
    std::error_code ec;
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw ExportError("Cannot write file " + core::pathToUtf8(target) + ": " + ec.message());
    }
}

void writeFileAtomic(const std::filesystem::path& target, std::string_view bytes)
{
    const std::filesystem::path tmp = temporarySibling(target);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw ExportError("Cannot create file: " + core::pathToUtf8(target));
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            out.close();
            std::error_code ignored;
            std::filesystem::remove(tmp, ignored);
            throw ExportError("Failed to write file: " + core::pathToUtf8(target));
        }
    }
    commitTemporary(tmp, target);
}

void appendNumber(std::string& out, double value)
{
    if (!std::isfinite(value))
        throw ExportError("Cannot export non-finite coordinate");
    if (value == 0.0)
        value = 0.0; // normalise -0
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    char buffer[32];
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (ec == std::errc{}) {
        out.append(buffer, ptr);
        return;
    }
#endif
    // Fallback for standard libraries without floating-point to_chars.
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream.precision(17);
    stream << value;
    out += stream.str();
}

void appendXmlEscaped(std::string& out, std::string_view text)
{
    for (const char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            // Control characters are not allowed in XML 1.0 (except tab, LF, CR).
            if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n' && c != '\r')
                out += ' ';
            else
                out += c;
        }
    }
}

void requireMeshes(const std::vector<ExportItem>& items, std::string_view formatName)
{
    if (items.empty())
        throw ExportError("Nothing to export");
    for (const auto& item : items) {
        if (item.mesh == nullptr || item.mesh->isEmpty())
            throw ExportError("'" + item.name + "' has no mesh to write as " + std::string(formatName));
        if (!item.mesh->isValid())
            throw ExportError("'" + item.name + "' has an invalid mesh (bad triangle indices)");
    }
}

} // namespace detail

bool canExport(FileFormat format)
{
    return format == FileFormat::Stl || format == FileFormat::ThreeMf || format == FileFormat::Step;
}

std::vector<std::string> supportedExportExtensions()
{
    return {"stl", "3mf", "step", "stp"};
}

void exportFile(const std::filesystem::path& path, const std::vector<ExportItem>& items, FileFormat format)
{
    if (format == FileFormat::Unknown)
        format = formatFromExtension(path);
    if (path.filename().empty())
        throw ExportError("Export path has no file name: " + core::pathToUtf8(path));
    const std::filesystem::path parent = path.parent_path();
    std::error_code ec;
    if (!parent.empty() && !std::filesystem::is_directory(parent, ec))
        throw ExportError("Folder does not exist: " + core::pathToUtf8(parent));

    switch (format) {
    case FileFormat::Stl:
        writeStl(path, items);
        return;
    case FileFormat::ThreeMf:
        write3mf(path, items);
        return;
    case FileFormat::Step:
        writeStep(path, items);
        return;
    default:
        break;
    }
    throw ExportError("Unsupported export format for " + core::pathToUtf8(path) + " (supported: STL, 3MF, STEP)");
}

} // namespace aerocad::io
