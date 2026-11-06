// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cfd/Results.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace aerocad::cfd {

namespace fs = std::filesystem;

namespace {

std::optional<double> toNumber(std::string_view s)
{
    double v = 0.0;
    const char* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(s.data(), end, v);
    if (ec != std::errc() || ptr != end)
        return std::nullopt;
    return v;
}

std::vector<std::string_view> split(std::string_view line)
{
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        const std::size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        if (i > start)
            out.push_back(line.substr(start, i - start));
    }
    return out;
}

} // namespace

const std::vector<double>* Table::column(std::string_view name) const
{
    for (std::size_t i = 0; i < columns.size(); ++i)
        if (columns[i] == name)
            return &values[i];
    return nullptr;
}

Table readTable(const fs::path& path)
{
    Table t;
    std::ifstream in(path);
    if (!in)
        return t;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        if (line[0] == '#') {
            auto words = split(std::string_view(line).substr(1));
            if (!words.empty() && words[0] == "Time" && words.size() > 1) {
                std::vector<std::string> cols(words.begin() + 1, words.end());
                if (cols != t.columns) {
                    // New header (first one, or the function object changed): restart.
                    t = Table{};
                    t.columns.assign(cols.begin(), cols.end());
                    t.values.assign(t.columns.size(), {});
                }
            }
            continue;
        }
        if (t.columns.empty())
            continue;
        const auto words = split(line);
        if (words.size() < 1 + t.columns.size())
            continue; // Partially written last line.
        const auto time = toNumber(words[0]);
        if (!time)
            continue;
        // Drop rows a restart is about to overwrite.
        while (!t.time.empty() && t.time.back() >= *time) {
            t.time.pop_back();
            for (auto& c : t.values)
                c.pop_back();
        }
        t.time.push_back(*time);
        for (std::size_t c = 0; c < t.columns.size(); ++c)
            t.values[c].push_back(toNumber(words[c + 1]).value_or(std::numeric_limits<double>::quiet_NaN()));
    }
    return t;
}

PolyData readVtk(const fs::path& path)
{
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("Cannot open " + path.string());
    PolyData data;
    std::string word;
    auto fail = [&](const std::string& what) {
        throw std::runtime_error("Malformed VTK file " + path.filename().string() + ": " + what);
    };
    auto readCount = [&]() {
        std::int64_t n = -1;
        if (!(in >> n) || n < 0)
            fail("bad count");
        return static_cast<std::size_t>(n);
    };
    auto readCells = [&](std::vector<std::vector<std::uint32_t>>& cells) {
        const std::size_t n = readCount();
        readCount(); // Total number of ints.
        cells.resize(n);
        for (auto& cell : cells) {
            const std::size_t k = readCount();
            cell.resize(k);
            for (auto& idx : cell) {
                std::int64_t v = -1;
                if (!(in >> v) || v < 0 || static_cast<std::size_t>(v) >= data.points.size())
                    fail("bad index");
                idx = static_cast<std::uint32_t>(v);
            }
        }
    };
    // Header: "# vtk DataFile Version x", title line, then keywords.
    std::string line;
    std::getline(in, line);
    if (line.rfind("# vtk", 0) != 0)
        fail("not a legacy VTK file");
    std::getline(in, line);
    std::size_t pointDataCount = 0;
    bool inPointData = false;
    while (in >> word) {
        if (word == "ASCII" || word == "DATASET" || word == "POLYDATA") {
            continue;
        }
        if (word == "BINARY") {
            fail("binary VTK is not supported");
        } else if (word == "POINTS") {
            const std::size_t n = readCount();
            in >> word; // float / double
            data.points.resize(n);
            for (auto& p : data.points)
                if (!(in >> p.x >> p.y >> p.z))
                    fail("bad point");
        } else if (word == "POLYGONS") {
            readCells(data.polygons);
        } else if (word == "LINES") {
            readCells(data.lines);
        } else if (word == "VERTICES") {
            std::vector<std::vector<std::uint32_t>> ignored;
            readCells(ignored);
        } else if (word == "POINT_DATA" || word == "CELL_DATA") {
            pointDataCount = readCount();
            inPointData = word == "POINT_DATA";
        } else if (word == "FIELD") {
            in >> word; // Name.
            const std::size_t arrays = readCount();
            for (std::size_t a = 0; a < arrays; ++a) {
                std::string name, type;
                in >> name;
                const std::size_t components = readCount();
                const std::size_t tuples = readCount();
                in >> type;
                std::vector<double> values(components * tuples);
                for (auto& v : values)
                    if (!(in >> v))
                        fail("bad value in " + name);
                // Point arrays only (FieldData holds e.g. the time value).
                if (!inPointData || tuples != pointDataCount || tuples != data.points.size())
                    continue;
                if (components == 1) {
                    data.scalars[name] = std::move(values);
                } else if (components == 3) {
                    std::vector<core::Vec3> vectors(tuples);
                    for (std::size_t i = 0; i < tuples; ++i)
                        vectors[i] = {values[3 * i], values[3 * i + 1], values[3 * i + 2]};
                    data.vectors[name] = std::move(vectors);
                }
            }
        } else {
            // Field data outside FIELD blocks (e.g. "TimeValue 1 1 float 300" is inside one),
            // or something we do not use: skip the rest of the line.
            std::getline(in, line);
        }
    }
    return data;
}

std::vector<fs::path> timeDirectories(const fs::path& dir)
{
    std::vector<std::pair<double, fs::path>> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_directory())
            continue;
        if (auto t = toNumber(entry.path().filename().string()))
            found.emplace_back(*t, entry.path());
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<fs::path> out;
    for (auto& f : found)
        out.push_back(std::move(f.second));
    return out;
}

// --- Quality ----------------------------------------------------------------------------

Json QualityReport::toJson() const
{
    Json j{{"status", status},
           {"solverConverged", solverConverged},
           {"failedMeshChecks", failedMeshChecks},
           {"finalResiduals", finalResiduals},
           {"warnings", warnings}};
    j["meshOk"] = meshOk ? Json(*meshOk) : Json(nullptr);
    j["cells"] = cells ? Json(*cells) : Json(nullptr);
    j["continuityError"] = continuityError ? Json(*continuityError) : Json(nullptr);
    j["cdFluctuation"] = cdFluctuation ? Json(*cdFluctuation) : Json(nullptr);
    return j;
}

namespace {

/// Reads a whole log file; empty when missing.
std::string readText(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::optional<double> numberAfter(std::string_view text, std::string_view key, std::size_t from = 0)
{
    const auto pos = text.find(key, from);
    if (pos == std::string_view::npos)
        return std::nullopt;
    const auto words = split(text.substr(pos + key.size(), 64));
    if (words.empty())
        return std::nullopt;
    std::string_view w = words[0];
    while (!w.empty() && (w.back() == ',' || w.back() == ';'))
        w.remove_suffix(1);
    return toNumber(w);
}

} // namespace

int lastSolverIteration(const fs::path& caseDir)
{
    const std::string log = readText(caseDir / "log.simpleFoam");
    const auto pos = log.rfind("\nTime = ");
    if (pos == std::string::npos)
        return 0;
    return static_cast<int>(numberAfter(log, "Time = ", pos).value_or(0.0));
}

QualityReport assessQuality(const fs::path& caseDir)
{
    QualityReport q;
    const std::string check = readText(caseDir / "log.checkMesh");
    if (!check.empty()) {
        if (auto cells = numberAfter(check, "    cells:"))
            q.cells = static_cast<std::int64_t>(*cells);
        if (check.find("Mesh OK.") != std::string::npos) {
            q.meshOk = true;
        } else if (const auto pos = check.find("Failed "); pos != std::string::npos) {
            q.meshOk = false;
            q.failedMeshChecks = static_cast<int>(numberAfter(check, "Failed ", pos).value_or(1.0));
            q.warnings.push_back("checkMesh: " + std::to_string(q.failedMeshChecks) + " mesh check(s) failed");
        }
    }

    const std::string solver = readText(caseDir / "log.simpleFoam");
    q.solverConverged = solver.find("SIMPLE solution converged") != std::string::npos;
    if (const auto pos = solver.rfind("time step continuity errors"); pos != std::string::npos)
        q.continuityError = numberAfter(solver, "sum local =", pos);

    const Table residuals = readTable(caseDir / "postProcessing" / "solverInfo" / "0" / "solverInfo.dat");
    for (const char* field : {"Ux", "Uy", "Uz", "p", "k", "omega"})
        if (const auto* col = residuals.column(std::string(field) + "_initial"); col && !col->empty())
            q.finalResiduals[field] = col->back();

    const Table coeffs = readTable(caseDir / "postProcessing" / "forceCoeffs" / "0" / "coefficient.dat");
    if (const auto* cd = coeffs.column("Cd"); cd && cd->size() >= 20) {
        const std::size_t start = cd->size() - std::max<std::size_t>(cd->size() / 5, 10);
        double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest(), sum = 0;
        for (std::size_t i = start; i < cd->size(); ++i) {
            lo = std::min(lo, (*cd)[i]);
            hi = std::max(hi, (*cd)[i]);
            sum += (*cd)[i];
        }
        const double mean = sum / static_cast<double>(cd->size() - start);
        if (std::abs(mean) > 1e-12)
            q.cdFluctuation = (hi - lo) / std::abs(mean);
    }

    // Thresholds of the preset. They flag results to be read with care; they are not a
    // validation of the physics (that is CAD-52).
    if (q.continuityError && *q.continuityError > 1e-3)
        q.warnings.push_back("mass balance: continuity error above 1e-3");
    if (q.cdFluctuation && *q.cdFluctuation > 0.02)
        q.warnings.push_back("Cd still changes by more than 2 % over the last 20 % of the iterations");
    if (auto it = q.finalResiduals.find("p"); it != q.finalResiduals.end() && it->second > 1e-2)
        q.warnings.push_back("pressure residual above 1e-2");

    const bool enough = q.meshOk.has_value() && q.cdFluctuation.has_value();
    q.status = !enough ? "unknown" : q.warnings.empty() ? "ok" : "warning";
    return q;
}

} // namespace aerocad::cfd
