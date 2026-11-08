// SPDX-License-Identifier: GPL-3.0-or-later
// Export and mesh validity commands.
#include "ExportCommands.hpp"

#include "aerocad/cad/Design.hpp"
#include "aerocad/core/MeshCheck.hpp"
#include "aerocad/core/MeshQuery.hpp"
#include "aerocad/core/Path.hpp"
#include "aerocad/geometry/Modeling.hpp"
#include "aerocad/geometry/Tessellation.hpp"
#include "aerocad/io/Export.hpp"
#include "aerocad/model/Document.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <random>
#include <string>
#include <memory>
#include <unordered_set>
#include <vector>

namespace aerocad::commands {

namespace {

/// The mesh written for a part: exact (B-rep) parts are tessellated finely for the file
/// (0.02 % of their size, about 8 degrees per facet; the view uses a coarser mesh), so a
/// printed fillet or curved skin is smooth. Kept in `store`.
const core::TriangleMesh* fileMesh(const model::Part& p, std::deque<core::TriangleMesh>& store)
{
    if (!p.shape || p.shape->isNull())
        return p.mesh.get();
    store.push_back(geometry::tessellate(*p.shape, {0.0002, 0.15}));
    return &store.back();
}

Json checkToJson(const core::MeshCheck& c)
{
    return Json{{"watertight", c.watertight()},
                {"closed", c.closed()},
                {"manifold", c.manifold()},
                {"oriented", c.oriented()},
                {"vertices", c.vertices},
                {"triangles", c.triangles},
                {"boundaryEdges", c.boundaryEdges},
                {"nonManifoldEdges", c.nonManifoldEdges},
                {"inconsistentEdges", c.inconsistentEdges},
                {"degenerateTriangles", c.degenerateTriangles},
                {"shells", c.shells},
                {"volume", c.closed() ? Json(c.signedVolume) : Json(nullptr)},
                {"problems", c.problems()}};
}

const model::Part& findPart(const model::Document& doc, model::PartId id)
{
    const model::Part* p = doc.part(id);
    if (p == nullptr)
        throw CommandError("No part with id " + std::to_string(id));
    return *p;
}

Json objectSchema(Json properties, Json required = Json::array())
{
    return Json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)},
                {"additionalProperties", false}};
}

io::FileFormat parseFormat(const Json& params, const std::filesystem::path& path)
{
    if (params.contains("format")) {
        const std::string f = params["format"].get<std::string>();
        if (f == "stl")
            return io::FileFormat::Stl;
        if (f == "3mf")
            return io::FileFormat::ThreeMf;
        return io::FileFormat::Step;
    }
    const io::FileFormat detected = io::formatFromExtension(path);
    if (!io::canExport(detected))
        throw CommandError("Cannot tell the export format from '" + core::pathToUtf8(path.filename()) +
                           "'. Use a .stl, .3mf or .step extension, or pass \"format\".");
    return detected;
}

/// Parts to export: the listed ids (in the given order, duplicates ignored) or all visible
/// bodies and imported parts.
std::vector<const model::Part*> selectParts(const model::Document& doc, const Json& params)
{
    std::vector<const model::Part*> parts;
    if (params.contains("ids")) {
        std::unordered_set<model::PartId> seen;
        for (const auto& id : params["ids"]) {
            const auto partId = id.get<model::PartId>();
            if (seen.insert(partId).second)
                parts.push_back(&findPart(doc, partId));
        }
    } else {
        // Sketch profiles and construction planes are helpers, not geometry to export.
        for (const auto& p : doc.parts()) {
            if (p.visible && p.kind != model::PartKind::Sketch && p.kind != model::PartKind::Plane)
                parts.push_back(&p);
        }
        if (parts.empty())
            throw CommandError("No visible parts to export");
    }
    return parts;
}


/// Parts for print export: the listed ids, or every visible solid with exact geometry
/// (not scans or surfaces).
std::vector<const model::Part*> selectPrintParts(const model::Document& doc, const Json& params)
{
    if (params.contains("ids"))
        return selectParts(doc, params);
    std::vector<const model::Part*> parts;
    for (const auto& p : doc.parts())
        if (p.visible && p.shape && !p.shape->isNull() && p.mesh && core::checkMesh(core::weldVertices(*p.mesh)).closed())
            parts.push_back(&p);
    if (parts.empty())
        throw CommandError("No visible solid parts to export");
    return parts;
}

/// Default minimum wall thickness for printing (mm): two perimeters of a 0.4 mm FDM nozzle.
constexpr double kDefaultMinWall = 0.8;

std::string mm(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.2f", value);
    return buffer;
}

/// Thin walls of a closed mesh: the thinnest wall, where it is, and the share of the
/// surface thinner than `limit`.
struct WallReport {
    bool measured = false;
    double thinnest = 0.0;
    core::Vec3 at;
    double thinAreaPercent = 0.0;
};

WallReport measureWalls(const core::TriangleMesh& welded, double limit)
{
    WallReport w;
    auto shared = std::make_shared<const core::TriangleMesh>(welded);
    const core::MeshBvh bvh(shared);
    const std::vector<double> t = core::triangleThickness(bvh);
    const auto& verts = welded.vertices();
    const auto& tris = welded.triangles();
    double total = 0.0;
    double thin = 0.0;
    for (std::size_t i = 0; i < tris.size(); ++i) {
        const core::Vec3& a = verts[tris[i][0]];
        const core::Vec3& b = verts[tris[i][1]];
        const core::Vec3& c = verts[tris[i][2]];
        const double area = 0.5 * (b - a).cross(c - a).length();
        total += area;
        if (!std::isfinite(t[i]))
            continue;
        if (!w.measured || t[i] < w.thinnest) {
            w.measured = true;
            w.thinnest = t[i];
            w.at = (a + b + c) * (1.0 / 3.0);
        }
        if (t[i] < limit)
            thin += area;
    }
    if (total > 0.0)
        w.thinAreaPercent = 100.0 * thin / total;
    return w;
}

double round3(double v)
{
    return std::round(v * 1000.0) / 1000.0;
}

/// Checks parts before they are written (see document.export_check). `format` decides what
/// is an error: printing needs a watertight mesh, STEP needs exact geometry.
Json exportCheck(const std::vector<const model::Part*>& parts, io::FileFormat format, double minWall, bool print = false,
                 const cad::Design* design = nullptr)
{
    Json results = Json::array();
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::deque<core::TriangleMesh> meshes;
    const bool step = format == io::FileFormat::Step;
    for (const model::Part* p : parts) {
        Json errorList = Json::array();
        Json warningList = Json::array();
        const bool exact = p->shape && !p->shape->isNull();
        Json r{{"id", p->id}, {"name", p->name}, {"exact", exact}};

        if (print && !exact)
            warningList.push_back("Mesh only, so only the STL file is written (no STEP).");
        if (step && !exact)
            errorList.push_back("Mesh only, STEP needs exact geometry. Export it as STL or 3MF, "
                                "or turn it into a body first (Mesh to BRep).");
        if (design != nullptr && p->kind == model::PartKind::Body && p->featureId != 0) {
            const cad::FeatureStatus st = design->status(cad::featureOfBody(p->featureId));
            if (st.report.is_object() && st.report.contains("fit") && st.report["fit"].is_object() &&
                !st.report["fit"].value("withinTolerance", true)) {
                const Json& fit = st.report["fit"];
                r["approximate"] = true;
                char text[200];
                std::snprintf(text, sizeof text,
                              "Approximation: its surface is up to %.3g mm from the scan, more than the requested "
                              "fit tolerance of %.3g mm.",
                              fit.value("maxDeviation", 0.0), fit.value("tolerance", 0.0));
                warningList.push_back(text);
            }
        }
        if (exact) {
            const bool valid = geometry::isValid(*p->shape);
            r["validBrep"] = valid;
            if (!valid)
                warningList.push_back("The exact geometry has defects (B-rep check failed); "
                                      "other CAD programs may not open it cleanly.");
        }

        if (const core::TriangleMesh* mesh = fileMesh(*p, meshes); mesh != nullptr) {
            const core::TriangleMesh welded = core::weldVertices(*mesh);
            const core::MeshCheck c = core::checkMesh(welded);
            r["watertight"] = c.watertight();
            r["closed"] = c.closed();
            r["manifold"] = c.manifold();
            r["oriented"] = c.oriented();
            r["shells"] = c.shells;
            r["volume"] = c.closed() ? Json(round3(c.signedVolume)) : Json(nullptr);
            // Only printing needs a watertight solid. A plain STL / 3MF / STEP export of an
            // open mesh (a scan to use in another program) is allowed with a warning (CAD-115).
            Json& meshProblems = print ? errorList : warningList;
            if (!c.closed()) {
                // An open surface: fine as STEP geometry or a scan file, but not a printable solid.
                const std::string msg = c.boundaryEdges > 0
                                            ? "Not closed: " + std::to_string(c.boundaryEdges) +
                                                  " open edges, so it is a surface, not a solid. Slicers cannot print it."
                                            : "Not a solid: " + std::to_string(c.nonManifoldEdges) +
                                                  " edges are shared by more than two faces.";
                meshProblems.push_back(msg);
            } else if (!c.watertight()) {
                for (const auto& problem : c.problems())
                    meshProblems.push_back(problem);
            }
            if (c.closed() && minWall > 0.0) {
                const WallReport w = measureWalls(welded, minWall);
                if (w.measured) {
                    r["minWall"] = Json{{"thinnest", round3(w.thinnest)},
                                        {"at", {round3(w.at.x), round3(w.at.y), round3(w.at.z)}},
                                        {"thinAreaPercent", round3(w.thinAreaPercent)},
                                        {"limit", minWall}};
                    if (w.thinnest < minWall) {
                        char share[32];
                        std::snprintf(share, sizeof share, "%.1f", std::max(w.thinAreaPercent, 0.1));
                        warningList.push_back("Walls thinner than " + mm(minWall) + " mm on " + share +
                                              " % of the surface (thinnest " + mm(w.thinnest) + " mm at " +
                                              mm(w.at.x) + ", " + mm(w.at.y) + ", " + mm(w.at.z) +
                                              "). They may not print.");
                    }
                }
            }
        } else if (!step) {
            errorList.push_back("Nothing to write: the part has no mesh.");
        }

        errors += errorList.size();
        warnings += warningList.size();
        r["status"] = !errorList.empty() ? "error" : !warningList.empty() ? "warning" : "ok";
        r["errors"] = std::move(errorList);
        r["warnings"] = std::move(warningList);
        results.push_back(std::move(r));
    }
    std::string summary;
    if (errors == 0 && warnings == 0)
        summary = parts.size() == 1 ? "Ready to export." : "All " + std::to_string(parts.size()) + " parts are ready to export.";
    else {
        auto count = [](std::size_t n, const char* one, const char* many) {
            return std::to_string(n) + " " + (n == 1 ? one : many);
        };
        if (errors > 0)
            summary = count(errors, "problem", "problems");
        if (warnings > 0)
            summary += (summary.empty() ? "" : " and ") + count(warnings, "warning", "warnings");
        summary += " in " + count(parts.size(), "part.", "parts.");
    }
    return Json{{"passed", errors == 0 && warnings == 0},
                {"canExport", errors == 0},
                {"format", std::string(io::formatName(format))},
                {"minWall", minWall},
                {"errors", errors},
                {"warnings", warnings},
                {"summary", summary},
                {"parts", std::move(results)}};
}

void registerCheck(CommandRegistry& r)
{
    r.add(
        {"part.check", "Check mesh",
         "Checks whether a part's mesh is a watertight solid: closed (no open edges), manifold "
         "(no edge shared by more than two triangles), consistently and outward oriented, without "
         "degenerate triangles. Reports counts, the enclosed volume (mm^3) and a list of problems.",
         objectSchema({{"id", {{"type", "integer"}, {"minimum", 1}, {"description", "Part id (see document.parts)."}}}},
                      {"id"})},
        [](CommandContext& ctx, const Json& params) {
            const model::Part& p = findPart(ctx.document, params.at("id").get<model::PartId>());
            if (!p.mesh)
                throw CommandError("Part '" + p.name + "' has no mesh");
            Json out = checkToJson(core::checkMesh(*p.mesh));
            out["id"] = p.id;
            out["name"] = p.name;
            return out;
        });

    r.add(
        {"document.export_check", "Check before export",
         "Checks parts before they are written to STL, 3MF or STEP and explains what is wrong: watertight "
         "and manifold mesh (printing needs a closed solid; an open mesh in a plain STL/3MF is a warning), exact "
         "geometry present and valid (STEP), and "
         "walls thinner than minWall for 3D printing (thinnest wall, where, share of the surface). "
         "status per part: ok, warning (can be exported, but check it) or error (the export would fail or "
         "the file is not printable). Writes nothing.",
         objectSchema(
             {{"format",
               {{"type", "string"}, {"enum", {"stl", "3mf", "step"}}, {"description", "Target format. Default stl."}}},
              {"ids",
               {{"type", "array"},
                {"items", {{"type", "integer"}, {"minimum", 1}}},
                {"minItems", 1},
                {"description",
                 "Parts to check; defaults to all visible bodies and imported parts, as document.export."}}},
              {"minWall",
               {{"type", "number"},
                {"minimum", 0},
                {"description", "Minimum wall thickness for printing, mm. Default 0.8; 0 skips the wall check."}}},
              {"print",
               {{"type", "boolean"},
                {"description",
                 "Check for document.export_for_print: the same default parts (visible solids), STL rules."}}}})},
        [](CommandContext& ctx, const Json& params) {
            const bool print = params.value("print", false);
            io::FileFormat format = io::FileFormat::Stl;
            if (params.contains("format") && !print)
                format = parseFormat(params, {});
            const auto parts = print ? selectPrintParts(ctx.document, params) : selectParts(ctx.document, params);
            return exportCheck(parts, format, params.value("minWall", kDefaultMinWall), print, ctx.design);
        });
}

void registerExport(CommandRegistry& r)
{
    r.add({"document.export", "Export",
           "Exports parts to a file in millimetres: STL (binary, all parts merged), 3MF (one object per "
           "part, unit millimeter) or STEP (exact B-rep, only parts that have it, e.g. imported from "
           "STEP/IGES). Meshes are checked first (see part.check); problems are reported as warnings, "
           "or abort the export when requireValid is true. An existing file is replaced.",
           objectSchema(
               {{"path", {{"type", "string"}, {"minLength", 1}, {"description", "Absolute path of the output file."}}},
                {"format",
                 {{"type", "string"},
                  {"enum", {"stl", "3mf", "step"}},
                  {"description", "Output format; defaults to the file extension."}}},
                {"ids",
                 {{"type", "array"},
                  {"items", {{"type", "integer"}, {"minimum", 1}}},
                  {"minItems", 1},
                  {"description", "Parts to export; defaults to all visible bodies and imported parts (not sketches or construction planes)."}}},
                {"requireValid",
                 {{"type", "boolean"},
                  {"description", "Fail instead of warning when a mesh is not watertight (STL/3MF). Default false."}}}},
               {"path"})},
          [](CommandContext& ctx, const Json& params) {
              const std::filesystem::path path = core::pathFromUtf8(params.at("path").get<std::string>());
              if (!path.is_absolute())
                  throw CommandError("Export path must be absolute: " + core::pathToUtf8(path));
              const io::FileFormat format = parseFormat(params, path);
              const bool requireValid = params.value("requireValid", false);
              const std::vector<const model::Part*> parts = selectParts(ctx.document, params);

              Json checks = Json::array();
              Json warnings = Json::array();
              std::vector<io::ExportItem> items;
              std::deque<core::TriangleMesh> meshes;
              std::size_t triangles = 0;
              for (const model::Part* p : parts) {
                  io::ExportItem item;
                  item.name = p->name;
                  item.mesh = format == io::FileFormat::Step ? p->mesh.get() : fileMesh(*p, meshes);
                  item.shape = p->shape ? &*p->shape : nullptr;
                  items.push_back(item);

                  if (format == io::FileFormat::Step || item.mesh == nullptr)
                      continue;
                  triangles += item.mesh->triangleCount();
                  // Check what is written: the writers drop triangles collapsed to an edge or
                  // a point (B-rep tessellations have them at singular points).
                  const core::MeshCheck check = core::checkMesh(core::weldVertices(*item.mesh));
                  Json c = checkToJson(check);
                  c["id"] = p->id;
                  c["name"] = p->name;
                  checks.push_back(c);
                  for (const auto& problem : check.problems())
                      warnings.push_back(p->name + ": " + problem);
              }
              if (requireValid && !warnings.empty()) {
                  std::string msg = "Export aborted, meshes are not watertight:";
                  for (const auto& w : warnings)
                      msg += "\n- " + w.get<std::string>();
                  throw CommandError(msg);
              }

              try {
                  io::exportFile(path, items, format);
              } catch (const io::ExportError& e) {
                  throw CommandError(e.what());
              }

              Json ids = Json::array();
              for (const model::Part* p : parts)
                  ids.push_back(p->id);
              Json out{{"path", core::pathToUtf8(path)},
                       {"format", std::string(io::formatName(format))},
                       {"ids", ids},
                       {"checks", checks},
                       {"warnings", warnings}};
              if (format != io::FileFormat::Step)
                  out["triangles"] = triangles;
              return out;
          });
}

/// A part name as a file name: characters file systems refuse become "-".
std::string fileStem(const std::string& name)
{
    std::string stem;
    for (const char c : name)
        stem += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
                 c == '|' || static_cast<unsigned char>(c) < 32)
                    ? '-'
                    : c;
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.'))
        stem.pop_back();
    return stem.empty() ? "part" : stem;
}

struct PrintFile {
    std::filesystem::path target;
    io::ExportItem item;
    io::FileFormat format;
};

/// Refuse directories, special files and even dangling links. Following a link here
/// would make the export's rollback affect a path outside its own output set.
bool checkPrintDestination(const std::filesystem::path& target)
{
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(target, ec);
    if (ec == std::errc::no_such_file_or_directory)
        return false;
    if (ec)
        throw CommandError("Cannot inspect export destination " + core::pathToUtf8(target) + ": " + ec.message());
    if (status.type() == std::filesystem::file_type::not_found)
        return false;
    if (!std::filesystem::is_regular_file(status))
        throw CommandError("Export destination is not a regular file: " + core::pathToUtf8(target));
    return true;
}

/// A unique directory on the destination filesystem keeps staged files and originals
/// out of the way of both other exports and the writers' per-file temporary names.
struct PrintWorkspace {
    std::filesystem::path path;
    bool keep = false;

    explicit PrintWorkspace(const std::filesystem::path& folder)
    {
        for (int attempt = 0; attempt < 16; ++attempt) {
            path = folder / (".aerocad-export-" + std::to_string(std::random_device{}()) + "-" +
                             std::to_string(std::random_device{}()));
            std::error_code ec;
            if (std::filesystem::create_directory(path, ec))
                return;
            if (ec && ec != std::errc::file_exists)
                throw CommandError("Cannot stage print export in " + core::pathToUtf8(folder) + ": " + ec.message());
        }
        throw CommandError("Cannot create a unique print-export staging folder in " + core::pathToUtf8(folder));
    }

    ~PrintWorkspace()
    {
        if (!keep) {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
};

/// All files are written before any destination changes. Rename originals into the
/// workspace, then publish; any reported failure rolls the changes back. This is a
/// command-level transaction, not an atomic directory swap or crash-recovery journal.
std::string writePrintSet(const std::filesystem::path& folder, const std::vector<PrintFile>& files)
{
    for (const auto& file : files)
        checkPrintDestination(file.target);
    PrintWorkspace workspace(folder);
    struct State {
        std::filesystem::path staged;
        std::filesystem::path backup;
        bool backedUp = false;
        bool installed = false;
    };
    std::vector<State> states;
    states.reserve(files.size());
    for (std::size_t i = 0; i < files.size(); ++i)
        states.push_back({workspace.path / ("new-" + std::to_string(i)),
                          workspace.path / ("previous-" + std::to_string(i))});

    try {
        for (std::size_t i = 0; i < files.size(); ++i)
            io::exportFile(states[i].staged, {files[i].item}, files[i].format);

        // Tessellation/writing can take time: recheck every destination before moving
        // any original, and recheck each again immediately before backing it up.
        for (const auto& file : files)
            checkPrintDestination(file.target);
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (checkPrintDestination(files[i].target)) {
                std::filesystem::rename(files[i].target, states[i].backup);
                states[i].backedUp = true;
            }
        }
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (checkPrintDestination(files[i].target))
                throw CommandError("Export destination changed during writing: " + core::pathToUtf8(files[i].target));
            std::filesystem::rename(states[i].staged, files[i].target);
            states[i].installed = true;
        }
    } catch (...) {
        // Preserve originals even if constructing a rollback diagnostic itself fails.
        workspace.keep = true;
        const std::exception_ptr cause = std::current_exception();
        std::string incomplete;
        for (std::size_t n = files.size(); n > 0; --n) {
            const std::size_t i = n - 1;
            std::error_code ec;
            if (states[i].installed) {
                const auto status = std::filesystem::symlink_status(files[i].target, ec);
                if (!ec && std::filesystem::is_regular_file(status))
                    std::filesystem::remove(files[i].target, ec);
                else if (ec == std::errc::no_such_file_or_directory)
                    ec.clear();
                else if (!ec)
                    ec = std::make_error_code(std::errc::operation_not_permitted);
            }
            if (!ec && states[i].backedUp) {
                const auto status = std::filesystem::symlink_status(files[i].target, ec);
                if (ec == std::errc::no_such_file_or_directory)
                    ec.clear();
                if (!ec && status.type() != std::filesystem::file_type::not_found)
                    ec = std::make_error_code(std::errc::file_exists);
                if (!ec)
                    std::filesystem::rename(states[i].backup, files[i].target, ec);
            }
            if (ec)
                incomplete += "\n- " + core::pathToUtf8(files[i].target) + ": " + ec.message() +
                              (states[i].backedUp ? " (original: " + core::pathToUtf8(states[i].backup) + ")" : "");
        }
        std::string message = "Print export failed";
        try {
            std::rethrow_exception(cause);
        } catch (const std::exception& e) {
            message += ": " + std::string(e.what());
        } catch (...) {
            message += ": unknown writer error";
        }
        if (!incomplete.empty()) {
            // Never delete the only surviving original when restoration itself fails.
            throw CommandError(message + "\nThe previous output set could not be fully restored:" + incomplete +
                               "\nRecovery files retained in " + core::pathToUtf8(workspace.path));
        }
        workspace.keep = false;
        throw CommandError(message + "\nThe previous output files were preserved; no new files remain.");
    }

    std::error_code ec;
    std::filesystem::remove_all(workspace.path, ec);
    workspace.keep = true;
    return ec ? "Files exported, but temporary backups could not be removed from " + core::pathToUtf8(workspace.path) +
                    ": " + ec.message()
              : std::string{};
}

void registerExportForPrint(CommandRegistry& r)
{
    r.add({"document.export_for_print", "Export for printing",
           "Writes each part to its own files in a folder, ready to print: <name>.stl (binary, mm, finely "
           "tessellated) and <name>.step (exact B-rep, for parts that have it), e.g. a set of canards. Every "
           "STL must be a watertight solid. All files are staged before existing files are replaced, and failed "
           "publication restores the previous set. If restoration fails, the error identifies retained recovery files.",
           objectSchema({{"folder", {{"type", "string"}, {"minLength", 1}, {"description", "Absolute path of the folder."}}},
                         {"ids",
                          {{"type", "array"},
                           {"items", {{"type", "integer"}, {"minimum", 1}}},
                           {"minItems", 1},
                           {"description", "Parts to write; defaults to all visible solids (not scans or surfaces)."}}},
                         {"formats",
                          {{"type", "array"},
                           {"items", {{"type", "string"}, {"enum", {"stl", "step"}}}},
                           {"minItems", 1},
                           {"description", "Default both."}}}},
                        {"folder"})},
          [](CommandContext& ctx, const Json& params) {
              const std::filesystem::path folder = core::pathFromUtf8(params.at("folder").get<std::string>());
              if (!folder.is_absolute())
                  throw CommandError("Folder must be an absolute path: " + core::pathToUtf8(folder));
              std::error_code ec;
              std::filesystem::create_directories(folder, ec);
              if (!std::filesystem::is_directory(folder))
                  throw CommandError("Cannot create the folder " + core::pathToUtf8(folder));
              bool stl = true, step = true;
              if (params.contains("formats")) {
                  stl = step = false;
                  for (const auto& f : params.at("formats")) {
                      stl = stl || f == "stl";
                      step = step || f == "step";
                  }
              }
              const std::vector<const model::Part*> parts = selectPrintParts(ctx.document, params);

              // Check everything before writing anything.
              struct Planned {
                  const model::Part* part;
                  const core::TriangleMesh* mesh;
                  std::string stem;
              };
              std::deque<core::TriangleMesh> meshes;
              std::vector<Planned> planned;
              std::unordered_set<std::string> stems;
              std::vector<std::string> problems;
              for (const model::Part* p : parts) {
                  // Compared without case: on macOS and Windows "Fin" and "fin" are one file.
                  auto folded = [](std::string s) {
                      std::transform(s.begin(), s.end(), s.begin(),
                                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                      return s;
                  };
                  std::string stem = fileStem(p->name);
                  for (int k = 2; !stems.insert(folded(stem)).second; ++k)
                      stem = fileStem(p->name) + " " + std::to_string(k);
                  const core::TriangleMesh* mesh = stl ? fileMesh(*p, meshes) : nullptr;
                  if (stl) {
                      if (mesh == nullptr) {
                          problems.push_back(p->name + ": no mesh");
                      } else {
                          for (const auto& problem : core::checkMesh(core::weldVertices(*mesh)).problems())
                              problems.push_back(p->name + ": " + problem);
                      }
                  }
                  planned.push_back({p, mesh, stem});
              }
              if (!problems.empty()) {
                  std::string msg = "Nothing was written; not every part is a watertight solid:";
                  for (const auto& w : problems)
                      msg += "\n- " + w;
                  throw CommandError(msg);
              }

              Json files = Json::array();
              Json warnings = Json::array();
              std::vector<PrintFile> outputs;
              for (const auto& pl : planned) {
                  if (stl) {
                      const auto path = folder / core::pathFromUtf8(pl.stem + ".stl");
                      outputs.push_back({path, {pl.part->name, pl.mesh, nullptr}, io::FileFormat::Stl});
                      files.push_back({{"id", pl.part->id}, {"format", "stl"}, {"path", core::pathToUtf8(path)},
                                       {"triangles", pl.mesh->triangleCount()}});
                  }
                  if (step) {
                      if (!pl.part->shape || pl.part->shape->isNull()) {
                          warnings.push_back(pl.part->name + ": no exact geometry, so no STEP file");
                          continue;
                      }
                      const auto path = folder / core::pathFromUtf8(pl.stem + ".step");
                      outputs.push_back({path, {pl.part->name, nullptr, &*pl.part->shape}, io::FileFormat::Step});
                      files.push_back({{"id", pl.part->id}, {"format", "step"}, {"path", core::pathToUtf8(path)}});
                  }
              }
              const auto cleanupWarning = writePrintSet(folder, outputs);
              if (!cleanupWarning.empty())
                  warnings.push_back(cleanupWarning);
              return Json{{"folder", core::pathToUtf8(folder)}, {"files", files}, {"warnings", warnings},
                          {"parts", planned.size()}};
          });
}

} // namespace

void registerExportCommands(CommandRegistry& registry)
{
    registerCheck(registry);
    registerExport(registry);
    registerExportForPrint(registry);
}

} // namespace aerocad::commands
