// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cfd/Results.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace aerocad::cfd {

namespace fs = std::filesystem;

namespace {

std::optional<double> toNumber(std::string_view s)
{
    if (s.empty())
        return std::nullopt;
    double v = 0.0;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    const char* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(s.data(), end, v);
    if (ec != std::errc() || ptr != end)
        return std::nullopt;
#else
    // Older libc++ (Apple) has no floating-point from_chars.
    std::istringstream stream{std::string(s)};
    stream.imbue(std::locale::classic());
    stream >> v;
    if (stream.fail())
        return std::nullopt;
    stream.peek();
    if (!stream.eof())
        return std::nullopt;
#endif
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
    std::vector<double> incompleteTimes;
    bool partialTail = false;
    std::ifstream in(path);
    if (!in)
        return t;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        // A live writer may have flushed only a prefix of a header, timestamp or
        // last value. Until the newline arrives, none of it can replace history.
        if (in.eof()) {
            partialTail = !split(line).empty();
            break;
        }
        if (line[0] == '#') {
            auto words = split(std::string_view(line).substr(1));
            if (!words.empty() && words[0] == "Time" && words.size() > 1) {
                std::vector<std::string> cols(words.begin() + 1, words.end());
                if (cols != t.columns) {
                    // New header (first one, or the function object changed): restart.
                    t = Table{};
                    incompleteTimes.clear();
                    t.columns.assign(cols.begin(), cols.end());
                    t.values.assign(t.columns.size(), {});
                }
            }
            continue;
        }
        if (t.columns.empty())
            continue;
        const auto words = split(line);
        if (words.empty())
            continue;
        const auto time = toNumber(words[0]);
        if (!time || !std::isfinite(*time)) {
            t.invalidTime = true;
            continue;
        }
        // A repeated/restarted time replaces both old samples and truncated rows.
        // A later time alone cannot repair a malformed row in the middle of a history.
        while (!incompleteTimes.empty() && incompleteTimes.back() >= *time)
            incompleteTimes.pop_back();
        while (!t.time.empty() && t.time.back() >= *time) {
            t.time.pop_back();
            for (auto& c : t.values)
                c.pop_back();
        }
        if (words.size() < 1 + t.columns.size()) {
            incompleteTimes.push_back(*time);
            continue;
        }
        t.time.push_back(*time);
        for (std::size_t c = 0; c < t.columns.size(); ++c)
            t.values[c].push_back(toNumber(words[c + 1]).value_or(std::numeric_limits<double>::quiet_NaN()));
    }
    t.incomplete = partialTail || !incompleteTimes.empty();
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

PolyData cropPolygons(const PolyData& data, const core::BoundingBox& box)
{
    PolyData out;
    constexpr auto unused = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> remap(data.points.size(), unused);
    std::vector<std::uint32_t> kept; // Old indices of the kept points, in new order.
    auto inside = [&](const core::Vec3& p) {
        return p.x >= box.min.x && p.x <= box.max.x && p.y >= box.min.y && p.y <= box.max.y &&
               p.z >= box.min.z && p.z <= box.max.z;
    };
    for (const auto& poly : data.polygons) {
        if (poly.empty())
            continue;
        core::Vec3 c;
        for (const auto i : poly)
            c = c + data.points[i];
        if (!inside(c * (1.0 / static_cast<double>(poly.size()))))
            continue;
        std::vector<std::uint32_t> mapped;
        mapped.reserve(poly.size());
        for (const auto i : poly) {
            if (remap[i] == unused) {
                remap[i] = static_cast<std::uint32_t>(kept.size());
                kept.push_back(i);
            }
            mapped.push_back(remap[i]);
        }
        out.polygons.push_back(std::move(mapped));
    }
    out.points.reserve(kept.size());
    for (const auto i : kept)
        out.points.push_back(data.points[i]);
    for (const auto& [name, values] : data.scalars) {
        auto& v = out.scalars[name];
        v.reserve(kept.size());
        for (const auto i : kept)
            v.push_back(i < values.size() ? values[i] : std::nan(""));
    }
    for (const auto& [name, values] : data.vectors) {
        auto& v = out.vectors[name];
        v.reserve(kept.size());
        for (const auto i : kept)
            v.push_back(i < values.size() ? values[i] : core::Vec3{});
    }
    return out;
}

PolyData cropLines(const PolyData& data, const core::BoundingBox& box)
{
    PolyData out;
    constexpr auto unused = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> remap(data.points.size(), unused);
    std::vector<std::uint32_t> kept;
    auto inside = [&](const core::Vec3& p) {
        return p.x >= box.min.x && p.x <= box.max.x && p.y >= box.min.y && p.y <= box.max.y &&
               p.z >= box.min.z && p.z <= box.max.z;
    };
    for (const auto& line : data.lines) {
        std::vector<std::uint32_t> piece;
        auto flush = [&] {
            if (piece.size() >= 2)
                out.lines.push_back(piece);
            piece.clear();
        };
        for (const auto i : line) {
            if (i >= data.points.size() || !inside(data.points[i])) {
                flush();
                continue;
            }
            if (remap[i] == unused) {
                remap[i] = static_cast<std::uint32_t>(kept.size());
                kept.push_back(i);
            }
            piece.push_back(remap[i]);
        }
        flush();
    }
    out.points.reserve(kept.size());
    for (const auto i : kept)
        out.points.push_back(data.points[i]);
    for (const auto& [name, values] : data.scalars) {
        auto& v = out.scalars[name];
        for (const auto i : kept)
            v.push_back(i < values.size() ? values[i] : std::nan(""));
    }
    for (const auto& [name, values] : data.vectors) {
        auto& v = out.vectors[name];
        for (const auto i : kept)
            v.push_back(i < values.size() ? values[i] : core::Vec3{});
    }
    return out;
}

void appendPolyData(PolyData& data, const PolyData& other)
{
    if (other.empty())
        return;
    if (data.empty()) {
        data = other;
        return;
    }
    const auto offset = static_cast<std::uint32_t>(data.points.size());
    const std::size_t before = data.points.size();
    data.points.insert(data.points.end(), other.points.begin(), other.points.end());
    auto shifted = [offset](std::vector<std::uint32_t> v) {
        for (auto& i : v)
            i += offset;
        return v;
    };
    for (const auto& p : other.polygons)
        data.polygons.push_back(shifted(p));
    for (const auto& l : other.lines)
        data.lines.push_back(shifted(l));
    for (auto it = data.scalars.begin(); it != data.scalars.end();) {
        auto o = other.scalars.find(it->first);
        if (o == other.scalars.end()) {
            it = data.scalars.erase(it);
            continue;
        }
        it->second.resize(before, std::nan(""));
        it->second.insert(it->second.end(), o->second.begin(), o->second.end());
        it->second.resize(data.points.size(), std::nan(""));
        ++it;
    }
    for (auto it = data.vectors.begin(); it != data.vectors.end();) {
        auto o = other.vectors.find(it->first);
        if (o == other.vectors.end()) {
            it = data.vectors.erase(it);
            continue;
        }
        it->second.resize(before);
        it->second.insert(it->second.end(), o->second.begin(), o->second.end());
        it->second.resize(data.points.size());
        ++it;
    }
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
           {"checks", checks},
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
    auto record = [&](const std::string& name, const std::string& state, const std::string& reason = {}) {
        q.checks[name] = state;
        if (!reason.empty())
            q.warnings.push_back(reason);
    };
    const std::string check = readText(caseDir / "log.checkMesh");
    if (auto cells = numberAfter(check, "    cells:");
        cells && std::isfinite(*cells) && *cells >= 0 &&
        *cells < static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        q.cells = static_cast<std::int64_t>(*cells);
    const auto meshPass = check.rfind("Mesh OK.");
    const auto meshFail = check.rfind("Failed ");
    if (meshFail != std::string::npos && (meshPass == std::string::npos || meshFail > meshPass)) {
        q.meshOk = false;
        const auto count = numberAfter(check, "Failed ", meshFail);
        q.failedMeshChecks = count && std::isfinite(*count) && *count > 0 && *count < std::numeric_limits<int>::max()
                                 ? static_cast<int>(*count)
                                 : 1;
        record("mesh", "failed", "checkMesh: " + std::to_string(q.failedMeshChecks) + " mesh check(s) failed");
    } else if (meshPass != std::string::npos) {
        q.meshOk = true;
        record("mesh", "passed");
    } else {
        record("mesh", "missing", "checkMesh: no completed mesh verdict");
    }

    const std::string solver = readText(caseDir / "log.simpleFoam");
    // An earlier convergence marker must not validate a later, incomplete restart,
    // including one interrupted during startup before its first iteration.
    std::size_t stepStart = 0;
    for (std::size_t start = 0; start < solver.size();) {
        const auto end = solver.find('\n', start);
        const auto line = std::string_view(solver).substr(start, end == std::string::npos ? end : end - start);
        const auto execColon = line.find_first_not_of(" \t", 4);
        const bool startup = line == "Create time" ||
                             (line.starts_with("Exec") && execColon != std::string_view::npos &&
                              line[execColon] == ':');
        if (startup || line.starts_with("Time = "))
            stepStart = start;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    const std::string_view lastStep = std::string_view(solver).substr(stepStart);
    std::optional<double> iteration;
    if (lastStep.starts_with("Time = "))
        iteration = numberAfter(lastStep, "Time = ");
    q.solverConverged = lastStep.find("SIMPLE solution converged") != std::string_view::npos;
    const bool ended = lastStep.find("\nEnd\n") != std::string_view::npos || lastStep.ends_with("\nEnd");
    if (lastStep.find("FOAM FATAL") != std::string_view::npos) {
        q.solverConverged = false;
        record("convergence", "failed", "solver: fatal error in the final solver step");
    } else if (!ended || !iteration || !std::isfinite(*iteration)) {
        record("convergence", "missing", "solver: missing or incomplete final solver log");
    } else if (!q.solverConverged) {
        record("convergence", "failed", "solver: finished without reaching the preset convergence targets");
    } else {
        record("convergence", "passed");
    }
    const auto continuityPos = lastStep.rfind("time step continuity errors");
    if (continuityPos != std::string_view::npos)
        q.continuityError = numberAfter(lastStep, "sum local =", continuityPos);
    if (!q.continuityError && continuityPos != std::string_view::npos &&
        lastStep.find("sum local =", continuityPos) != std::string_view::npos) {
        record("massBalance", "failed", "mass balance: invalid continuity error");
    } else if (!q.continuityError) {
        record("massBalance", "missing", "mass balance: no continuity error for the final solver step");
    } else if (!std::isfinite(*q.continuityError) || *q.continuityError < 0) {
        record("massBalance", "failed", "mass balance: invalid continuity error");
    } else if (*q.continuityError > 1e-3) {
        record("massBalance", "failed", "mass balance: continuity error above 1e-3");
    } else {
        record("massBalance", "passed");
    }

    const Table residuals = readTable(caseDir / "postProcessing" / "solverInfo" / "0" / "solverInfo.dat");
    for (const char* field : {"Ux", "Uy", "Uz", "p", "k", "omega"}) {
        const std::string name = std::string("residual.") + field;
        const auto* col = residuals.column(std::string(field) + "_initial");
        if (col && !col->empty())
            q.finalResiduals[field] = col->back(); // Retain diagnostics, including rejected values.
        if (residuals.invalidTime || (col && !col->empty() && (!std::isfinite(col->back()) || col->back() < 0))) {
            record(name, "failed", std::string(field) + ": invalid residual data");
        } else if (!col || col->empty() || residuals.incomplete || !iteration || residuals.time.back() != *iteration) {
            record(name, "missing", std::string(field) + ": no complete residual for the final solver step");
        } else if (const double target = std::string_view(field) == "p" ? 1e-4 : 1e-5; col->back() >= target) {
            // Match external-flow-v1 residualControl in CaseWriter, including velocity and turbulence.
            record(name, "failed", std::string(field) + ": residual has not reached the preset target " +
                                      (std::string_view(field) == "p" ? "1e-4" : "1e-5"));
        } else {
            record(name, "passed");
        }
    }

    const Table coeffs = readTable(caseDir / "postProcessing" / "forceCoeffs" / "0" / "coefficient.dat");
    bool invalidCoefficients = coeffs.invalidTime;
    bool missingCoefficients = coeffs.incomplete || coeffs.time.size() < 20 || !iteration ||
                               (!coeffs.time.empty() && coeffs.time.back() != *iteration);
    for (const char* field : {"Cd", "Cl", "Cs"}) {
        const auto* col = coeffs.column(field);
        missingCoefficients = missingCoefficients || !col || col->empty();
        if (col)
            invalidCoefficients = invalidCoefficients ||
                                  std::any_of(col->begin(), col->end(), [](double v) { return !std::isfinite(v); });
    }
    // Keep a usable Cd diagnostic even when other required evidence is missing.
    if (const auto* cd = coeffs.column("Cd"); !invalidCoefficients && cd && cd->size() >= 20) {
        const std::size_t start = cd->size() - std::max<std::size_t>(cd->size() / 5, 10);
        double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest(), sum = 0;
        for (std::size_t i = start; i < cd->size(); ++i) {
            lo = std::min(lo, (*cd)[i]);
            hi = std::max(hi, (*cd)[i]);
            sum += (*cd)[i];
        }
        const double mean = sum / static_cast<double>(cd->size() - start);
        if (!std::isfinite(sum) || !std::isfinite(hi - lo)) {
            invalidCoefficients = true;
        } else if (std::abs(mean) > 1e-12) {
            q.cdFluctuation = (hi - lo) / std::abs(mean);
            invalidCoefficients = !std::isfinite(*q.cdFluctuation);
        }
    }
    if (invalidCoefficients) {
        record("forceStability", "failed", "force coefficients: non-finite, invalid or overflowing data");
    } else if (missingCoefficients) {
        record("forceStability", "missing",
               "force coefficients: need at least 20 complete Cd/Cl/Cs samples through the final solver step");
    } else if (!q.cdFluctuation) {
        record("forceStability", "missing",
               "force coefficients: Cd is too close to zero to assess relative fluctuation");
    } else if (*q.cdFluctuation > 0.02) {
        record("forceStability", "failed",
               "Cd still changes by more than 2 % over the last 20 % of the iterations");
    } else {
        record("forceStability", "passed");
    }

    // Failed evidence takes precedence over missing evidence. An OK verdict needs every
    // required check to pass; neither this nor the thresholds validate the physics.
    const bool failed =
        std::any_of(q.checks.begin(), q.checks.end(), [](const auto& c) { return c.second == "failed"; });
    const bool missing =
        std::any_of(q.checks.begin(), q.checks.end(), [](const auto& c) { return c.second == "missing"; });
    q.status = failed ? "warning" : missing ? "unknown" : "ok";
    return q;
}

} // namespace aerocad::cfd
