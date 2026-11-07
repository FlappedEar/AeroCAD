// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/io/Import.hpp"

#include "ReaderUtil.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <system_error>

namespace aerocad::io {

namespace detail {

std::string readFileBytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw ImportError("Cannot open file: " + pathToUtf8(path));
    std::string data;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size > 0) {
        data.resize(static_cast<std::size_t>(size));
        in.seekg(0, std::ios::beg);
        in.read(data.data(), static_cast<std::streamsize>(size));
        if (in.gcount() != static_cast<std::streamsize>(size))
            throw ImportError("Failed to read file: " + pathToUtf8(path));
    }
    return data;
}

bool parseDouble(std::string_view text, double& out)
{
    if (!text.empty() && text.front() == '+')
        text.remove_prefix(1);
    if (text.empty())
        return false;
    {
        // nan / inf spelled out: from_chars accepts them, the stream fallback (Apple libc++)
        // does not, so handle them here for identical behaviour on every platform.
        std::string_view word = text;
        const bool negative = word.front() == '-';
        if (negative)
            word.remove_prefix(1);
        if (iequals(word, "nan")) {
            out = std::numeric_limits<double>::quiet_NaN();
            return true;
        }
        if (iequals(word, "inf") || iequals(word, "infinity")) {
            out = negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
            return true;
        }
    }
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc{} && ptr == end;
#else
    // Fallback for standard libraries without floating-point from_chars (older libc++).
    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());
    double value = 0.0;
    stream >> value;
    if (stream.fail())
        return false;
    stream.peek();
    if (!stream.eof())
        return false;
    out = value;
    return true;
#endif
}

bool parseInt(std::string_view text, long long& out)
{
    if (!text.empty() && text.front() == '+')
        text.remove_prefix(1);
    if (text.empty())
        return false;
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc{} && ptr == end;
}

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb))
            return false;
    }
    return true;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && isSpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back()))
        s.remove_suffix(1);
    return s;
}

std::string_view nextToken(std::string_view& s)
{
    std::size_t i = 0;
    while (i < s.size() && isSpace(s[i]))
        ++i;
    std::size_t j = i;
    while (j < s.size() && !isSpace(s[j]))
        ++j;
    const std::string_view token = s.substr(i, j - i);
    s.remove_prefix(j);
    return token;
}

void WarningSink::add(std::string message)
{
    if (m_count < m_limit)
        m_out.push_back(std::move(message));
    ++m_count;
}

WarningSink::~WarningSink()
{
    if (m_count > m_limit)
        m_out.push_back(std::to_string(m_count - m_limit) + " more similar warning(s) suppressed");
}

ProgressReporter::ProgressReporter(const ImportOptions& options)
    : m_callback(options.progress), m_next(options.progress ? 1 : std::numeric_limits<std::size_t>::max())
{
    report(0.0);
}

void ProgressReporter::step(std::size_t done, std::size_t total)
{
    if (total == 0)
        return;
    m_next = done + std::max<std::size_t>(1, total / 100);
    report(std::min(1.0, static_cast<double>(done) / static_cast<double>(total)));
}

void ProgressReporter::finish()
{
    report(1.0);
}

void ProgressReporter::report(double fraction)
{
    if (!m_callback)
        return;
    fraction = std::max(fraction, m_last); // never go backwards
    m_last = fraction;
    if (!m_callback(fraction))
        throw ImportCancelled();
}

} // namespace detail

std::string_view formatName(FileFormat format)
{
    switch (format) {
    case FileFormat::Stl:
        return "STL";
    case FileFormat::Obj:
        return "OBJ";
    case FileFormat::Ply:
        return "PLY";
    case FileFormat::ThreeMf:
        return "3MF";
    case FileFormat::Step:
        return "STEP";
    case FileFormat::Iges:
        return "IGES";
    case FileFormat::Brep:
        return "BREP";
    case FileFormat::Unknown:
        break;
    }
    return "Unknown";
}

FileFormat formatFromExtension(const std::filesystem::path& path)
{
    std::string ext = detail::pathToUtf8(path.extension());
    if (!ext.empty() && ext.front() == '.')
        ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });

    if (ext == "stl")
        return FileFormat::Stl;
    if (ext == "obj")
        return FileFormat::Obj;
    if (ext == "ply")
        return FileFormat::Ply;
    if (ext == "3mf")
        return FileFormat::ThreeMf;
    if (ext == "step" || ext == "stp")
        return FileFormat::Step;
    if (ext == "iges" || ext == "igs")
        return FileFormat::Iges;
    if (ext == "brep" || ext == "brp")
        return FileFormat::Brep;
    return FileFormat::Unknown;
}

std::vector<std::string> supportedImportExtensions()
{
    return {"stl", "obj", "ply", "3mf", "step", "stp", "iges", "igs", "brep", "brp"};
}

ImportResult importFile(const std::filesystem::path& path)
{
    return importFile(path, ImportOptions{});
}

ImportResult importFile(const std::filesystem::path& path, const ImportOptions& options)
{
    const std::string name = detail::pathToUtf8(path);

    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        throw ImportError("File not found: " + name);
    if (!std::filesystem::is_regular_file(path, ec))
        throw ImportError("Not a regular file: " + name);

    const FileFormat format = formatFromExtension(path);
    ImportResult result;
    try {
        // Readers without incremental progress: report only the start and the end.
        const auto whole = [&options](auto&& read) {
            detail::ProgressReporter progress(options);
            ImportResult r = read();
            progress.finish();
            return r;
        };
        switch (format) {
        case FileFormat::Stl:
            result = readStl(path, options);
            break;
        case FileFormat::Obj:
            result = readObj(path, options);
            break;
        case FileFormat::Ply:
            result = readPly(path, options);
            break;
        case FileFormat::ThreeMf:
            result = whole([&path] { return read3mf(path); });
            break;
        case FileFormat::Step:
            result = whole([&path] { return readStep(path); });
            break;
        case FileFormat::Iges:
            result = whole([&path] { return readIges(path); });
            break;
        case FileFormat::Brep:
            result = whole([&path] { return readBrep(path); });
            break;
        case FileFormat::Unknown:
            throw ImportError("Unsupported file type: " + name);
        }
    } catch (const ImportError&) {
        throw;
    } catch (const std::exception& e) {
        throw ImportError("Failed to import " + name + ": " + e.what());
    }

    result.format = format;
    if (result.mesh.isEmpty())
        throw ImportError("No geometry found in " + name);
    return result;
}

} // namespace aerocad::io
