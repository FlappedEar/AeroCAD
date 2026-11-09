// SPDX-License-Identifier: GPL-3.0-or-later
// 3MF writer (core specification 1.x): one mesh object per item, unit millimetre.
#include "aerocad/io/Export.hpp"

#include "WriterUtil.hpp"

#include "aerocad/core/MeshCheck.hpp"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace aerocad::io {

namespace {

constexpr const char* kContentTypes =
    R"(<?xml version="1.0" encoding="UTF-8"?>)"
    "\n"
    R"(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">)"
    R"(<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>)"
    R"(<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>)"
    "</Types>\n";

constexpr const char* kRels =
    R"(<?xml version="1.0" encoding="UTF-8"?>)"
    "\n"
    R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
    R"(<Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>)"
    "</Relationships>\n";

bool isHexColor(const std::string& c)
{
    return c.size() == 7 && c[0] == '#' &&
           std::all_of(c.begin() + 1, c.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; });
}

std::string buildModel(const std::vector<ExportItem>& items)
{
    std::string xml;
    std::size_t estimate = 512;
    for (const auto& item : items)
        estimate += item.mesh->vertexCount() * 64 + item.mesh->triangleCount() * 48;
    xml.reserve(estimate);

    xml +=
        R"(<?xml version="1.0" encoding="UTF-8"?>)"
        "\n"
        R"(<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">)"
        "\n"
        R"(<metadata name="Application">AeroCAD</metadata>)"
        "\n<resources>\n";

    // Appearance colours as a core-spec material group (CAD-228), declared before the
    // objects that use it. Its id follows the object ids.
    std::vector<std::string> colors;
    std::vector<int> colorIndex(items.size(), -1);
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!isHexColor(items[i].color))
            continue;
        auto it = std::find(colors.begin(), colors.end(), items[i].color);
        if (it == colors.end())
            it = colors.insert(colors.end(), items[i].color);
        colorIndex[i] = static_cast<int>(it - colors.begin());
    }
    const std::string materialsId = std::to_string(items.size() + 1);
    if (!colors.empty()) {
        xml += R"(<basematerials id=")" + materialsId + "\">\n";
        for (const std::string& c : colors) {
            xml += R"(<base name=")" + c + R"(" displaycolor=")";
            for (const char ch : c)
                xml += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            xml += "FF\"/>\n";
        }
        xml += "</basematerials>\n";
    }

    for (std::size_t i = 0; i < items.size(); ++i) {
        // 3MF requires shared vertices (B-rep tessellations have one vertex set per face).
        const core::TriangleMesh mesh = core::weldVertices(*items[i].mesh);
        xml += R"(<object id=")" + std::to_string(i + 1) + R"(" type="model" name=")";
        detail::appendXmlEscaped(xml, items[i].name);
        xml += "\"";
        if (colorIndex[i] >= 0)
            xml += R"( pid=")" + materialsId + R"(" pindex=")" + std::to_string(colorIndex[i]) + "\"";
        xml += ">\n<mesh>\n<vertices>\n";
        for (const core::Vec3& v : mesh.vertices()) {
            xml += R"(<vertex x=")";
            detail::appendNumber(xml, v.x);
            xml += R"(" y=")";
            detail::appendNumber(xml, v.y);
            xml += R"(" z=")";
            detail::appendNumber(xml, v.z);
            xml += "\"/>\n";
        }
        xml += "</vertices>\n<triangles>\n";
        for (const auto& t : mesh.triangles()) {
            xml += R"(<triangle v1=")" + std::to_string(t[0]) + R"(" v2=")" + std::to_string(t[1]) + R"(" v3=")" +
                   std::to_string(t[2]) + "\"/>\n";
        }
        xml += "</triangles>\n</mesh>\n</object>\n";
    }
    xml += "</resources>\n<build>\n";
    for (std::size_t i = 0; i < items.size(); ++i)
        xml += R"(<item objectid=")" + std::to_string(i + 1) + "\"/>\n";
    xml += "</build>\n</model>\n";
    return xml;
}

class ZipWriter {
public:
    ZipWriter()
    {
        if (!mz_zip_writer_init_heap(&m_zip, 0, 1 << 16))
            throw ExportError("Cannot initialise 3MF package");
    }
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;
    ~ZipWriter() { mz_zip_writer_end(&m_zip); }

    void add(const char* name, const std::string& data)
    {
        if (!mz_zip_writer_add_mem(&m_zip, name, data.data(), data.size(), MZ_DEFAULT_COMPRESSION))
            throw ExportError(std::string("Cannot add ") + name + " to 3MF package");
    }

    std::string finish()
    {
        void* buffer = nullptr;
        std::size_t size = 0;
        if (!mz_zip_writer_finalize_heap_archive(&m_zip, &buffer, &size))
            throw ExportError("Cannot finalise 3MF package");
        // Ownership of the buffer moves to the caller; release it with the archive's allocator.
        std::string bytes(static_cast<const char*>(buffer), size);
        m_zip.m_pFree(m_zip.m_pAlloc_opaque, buffer);
        return bytes;
    }

private:
    mz_zip_archive m_zip{};
};

} // namespace

void write3mf(const std::filesystem::path& path, const std::vector<ExportItem>& items)
{
    detail::requireMeshes(items, "3MF");
    const std::string model = buildModel(items);

    ZipWriter zip;
    zip.add("[Content_Types].xml", kContentTypes);
    zip.add("_rels/.rels", kRels);
    zip.add("3D/3dmodel.model", model);
    detail::writeFileAtomic(path, zip.finish());
}

} // namespace aerocad::io
