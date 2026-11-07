// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cfd/RunManager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace aerocad::cfd {

namespace fs = std::filesystem;
using core::Vec3;

std::string_view backendName(Backend backend) { return backend == Backend::Docker ? "docker" : "local"; }

std::optional<Backend> parseBackend(std::string_view name)
{
    if (name == "docker")
        return Backend::Docker;
    if (name == "local")
        return Backend::Local;
    return std::nullopt;
}

std::string_view runStateName(RunState state)
{
    switch (state) {
    case RunState::Running: return "running";
    case RunState::Finished: return "finished";
    case RunState::Failed: return "failed";
    case RunState::Cancelled: return "cancelled";
    case RunState::Interrupted: return "interrupted";
    }
    return "running";
}

// --- Environment ------------------------------------------------------------------------

namespace {

std::string env(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

fs::path home() { return env("HOME"); }

/// Apps started from the macOS Finder get a minimal PATH (/usr/bin:/bin:/usr/sbin:/sbin),
/// without the directories Docker Desktop, Homebrew and OrbStack install into. The docker
/// CLI also needs its credential helper from there to pull images. Append them once.
void extendToolPath(const fs::path& docker)
{
    static bool done = false;
    if (done)
        return;
    done = true;
#ifndef _WIN32
    std::string path = env("PATH");
    std::vector<std::string> dirs;
    if (!docker.empty())
        dirs.push_back(docker.parent_path().string());
    for (const char* d : {"/usr/local/bin", "/opt/homebrew/bin", "/Applications/Docker.app/Contents/Resources/bin"})
        dirs.emplace_back(d);
    if (!home().empty()) {
        dirs.push_back((home() / ".docker" / "bin").string());
        dirs.push_back((home() / ".orbstack" / "bin").string());
    }
    for (const auto& d : dirs) {
        const std::string padded = ":" + path + ":";
        if (padded.find(":" + d + ":") == std::string::npos)
            path += (path.empty() ? "" : ":") + d;
    }
    ::setenv("PATH", path.c_str(), 1);
#else
    (void)docker;
#endif
}

fs::path findDocker()
{
    if (const std::string over = env("AEROCAD_DOCKER"); !over.empty())
        return findExecutable(over);
    std::vector<fs::path> candidates{"/usr/local/bin/docker", "/opt/homebrew/bin/docker",
                                     "/Applications/Docker.app/Contents/Resources/bin/docker"};
    if (!home().empty()) {
        candidates.push_back(home() / ".docker" / "bin" / "docker");
        candidates.push_back(home() / ".orbstack" / "bin" / "docker");
    }
    return findExecutable("docker", candidates);
}

fs::path findOpenFoamBashrc()
{
    std::error_code ec;
    if (const std::string over = env("AEROCAD_OPENFOAM_BASHRC"); !over.empty())
        return fs::is_regular_file(over, ec) ? fs::path(over) : fs::path();
    std::vector<fs::path> candidates;
    if (const std::string wm = env("WM_PROJECT_DIR"); !wm.empty())
        candidates.push_back(fs::path(wm) / "etc" / "bashrc");
    // Install locations of the openfoam.com packages for this version.
    candidates.emplace_back("/usr/lib/openfoam/openfoam2506/etc/bashrc");
    candidates.emplace_back("/opt/openfoam2506/etc/bashrc");
    for (const auto& c : candidates)
        if (fs::is_regular_file(c, ec))
            return c;
    return {};
}

} // namespace

Json Environment::toJson() const
{
    Json j{{"docker", docker.string()},
           {"dockerRunning", dockerRunning},
           {"imagePresent", imagePresent},
           {"image", kDockerImage},
           {"openfoamVersion", kOpenFoamVersion},
           {"openfoamBashrc", openfoamBashrc.string()},
           {"message", message}};
    j["recommendedBackend"] = recommended ? Json(backendName(*recommended)) : Json(nullptr);
    return j;
}

Environment detectEnvironment()
{
    Environment e;
    e.docker = findDocker();
    extendToolPath(e.docker);
    if (!e.docker.empty()) {
        const auto info = runCommand({e.docker.string(), "info", "--format", "{{.ServerVersion}}"},
                                     std::chrono::seconds(10));
        e.dockerRunning = info.exitCode == 0;
        if (e.dockerRunning) {
            const auto image = runCommand({e.docker.string(), "image", "inspect", std::string(kDockerImage),
                                           "--format", "{{.Id}}"},
                                          std::chrono::seconds(10));
            e.imagePresent = image.exitCode == 0;
        }
    }
    e.openfoamBashrc = findOpenFoamBashrc();
    if (e.dockerRunning) {
        e.recommended = Backend::Docker;
        e.message = e.imagePresent ? "Docker is running and the OpenFOAM image is present."
                                   : "Docker is running. The first run downloads the OpenFOAM image " +
                                         std::string(kDockerImage) + " (about 0.4-0.5 GB download).";
    } else if (!e.openfoamBashrc.empty()) {
        e.recommended = Backend::Local;
        e.message = "Using the local OpenFOAM installation " + e.openfoamBashrc.string() + ".";
    } else if (!e.docker.empty()) {
        e.message = "Docker is installed but not running. Start Docker Desktop (or colima / OrbStack) and try again.";
    } else {
        e.message = "No simulation engine found. Install Docker Desktop (https://docs.docker.com/desktop/) and start "
                    "it; AeroCAD runs OpenFOAM " +
                    std::string(kOpenFoamVersion) + " in the official image " + std::string(kDockerImage) + ".";
    }
    return e;
}

// --- Run --------------------------------------------------------------------------------

struct RunManager::Run {
    std::string id;
    fs::path dir;     ///< Run directory (run.json, log.aerocad).
    fs::path caseDir; ///< OpenFOAM case; equal to dir unless imported.
    std::string created;
    RunSource source;
    Backend backend = Backend::Docker;
    bool imported = false;
    std::optional<Process> process; ///< Engine started by this RunManager.
    std::optional<Process> stopper; ///< `docker kill` started by cancel().
    fs::path docker;
    RunStatus status;
};

namespace {

std::string readFirstLine(const fs::path& path)
{
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.pop_back();
    return line;
}

std::string nowIso()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string slug(const std::string& name)
{
    std::string s;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!s.empty() && s.back() != '-')
            s += '-';
        if (s.size() >= 24)
            break;
    }
    while (!s.empty() && s.back() == '-')
        s.pop_back();
    return s.empty() ? "part" : s;
}

Json sourceJson(const RunSource& s)
{
    return Json{{"partId", s.partId},
                {"partName", s.partName},
                {"partRevision", s.partRevision},
                // As a string: JSON numbers above 2^53 lose precision in JavaScript.
                {"partFingerprint", std::to_string(s.partFingerprint)},
                {"contextPartIds", s.contextPartIds},
                {"label", s.label}};
}

RunSource sourceFrom(const Json& j)
{
    RunSource s;
    s.partId = j.value("partId", std::uint64_t{0});
    s.partName = j.value("partName", std::string());
    s.partRevision = j.value("partRevision", std::uint64_t{0});
    if (const auto it = j.find("partFingerprint"); it != j.end() && it->is_string()) {
        try {
            s.partFingerprint = std::stoull(it->get<std::string>());
        } catch (const std::exception&) {
            s.partFingerprint = 0;
        }
    }
    s.contextPartIds = j.value("contextPartIds", std::vector<std::uint64_t>{});
    s.label = j.value("label", std::string());
    return s;
}

std::string containerName(const std::string& id) { return "aerocad-cfd-" + id; }

/// Progress of the Allrun steps, before / after each.
double stageProgress(const std::string& stage)
{
    if (stage == "features") return 0.01;
    if (stage == "background") return 0.03;
    if (stage == "decompose") return 0.05;
    if (stage == "mesh") return 0.07;
    if (stage == "check") return 0.25;
    if (stage == "initialise") return 0.27;
    if (stage == "solve") return 0.30;
    if (stage == "reconstruct") return 0.96;
    if (stage == "done") return 1.0;
    return 0.0;
}

/// Log file of the application an Allrun step runs.
std::string stageLog(const std::string& stage)
{
    if (stage == "features") return "log.surfaceFeatureExtract";
    if (stage == "background") return "log.blockMesh";
    if (stage == "decompose") return "log.decomposePar";
    if (stage == "mesh") return "log.snappyHexMesh";
    if (stage == "initialise") return "log.potentialFoam";
    if (stage == "solve") return "log.simpleFoam";
    if (stage == "reconstruct") return "log.reconstructPar";
    return "log.aerocad";
}

/// First line of an OpenFOAM fatal error message in a log, or the last non-empty line.
std::string errorSummary(const fs::path& log)
{
    std::ifstream in(log);
    std::string line, last, fatal;
    bool next = false;
    while (std::getline(in, line)) {
        // Strip MPI rank prefixes such as "[1] ".
        if (line.size() > 2 && line[0] == '[') {
            const auto close = line.find("] ");
            if (close != std::string::npos && close < 8)
                line = line.substr(close + 2);
        }
        if (next && fatal.empty() && line.find_first_not_of(' ') != std::string::npos) {
            fatal = line;
            next = false;
        }
        if (line.find("FOAM FATAL") != std::string::npos)
            next = true;
        if (line.find_first_not_of(" \t") != std::string::npos)
            last = line;
    }
    return fatal.empty() ? last : fatal;
}

/// Newest write time of the logs, or nullopt when there are none.
std::optional<fs::file_time_type> newestLogTime(const fs::path& dir)
{
    // file_time_type{} is not "long ago": libstdc++'s file clock has a recent epoch, so
    // current times are negative. Start from min().
    auto newest = fs::file_time_type::min();
    bool any = false;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const auto name = e.path().filename().string();
        if (name.rfind("log.", 0) == 0 || name == "aerocad.stage") {
            const auto t = e.last_write_time(ec);
            if (!ec) {
                newest = std::max(newest, t);
                any = true;
            }
        }
    }
    return any ? std::optional(newest) : std::nullopt;
}

std::optional<double> lastOf(const Table& t, std::string_view column)
{
    const auto* c = t.column(column);
    if (c == nullptr || c->empty() || !std::isfinite(c->back()))
        return std::nullopt;
    return c->back();
}

} // namespace

Json RunStatus::toJson() const
{
    auto opt = [](const std::optional<double>& v) { return v ? Json(*v) : Json(nullptr); };
    return Json{{"id", id},
                {"state", runStateName(state)},
                {"stage", stage},
                {"progress", std::round(progress * 1000.0) / 1000.0},
                {"iteration", iteration},
                {"maxIterations", maxIterations},
                {"message", message},
                {"created", created},
                {"backend", backendName(backend)},
                {"caseDir", caseDir.string()},
                {"source", sourceJson(source)},
                {"latest",
                 {{"cd", opt(cd)},
                  {"cl", opt(cl)},
                  {"cs", opt(cs)},
                  {"dragN", opt(dragN)},
                  {"liftN", opt(liftN)},
                  {"sideN", opt(sideN)}}}};
}

// --- RunManager -------------------------------------------------------------------------

fs::path RunManager::defaultRoot()
{
    if (const std::string over = env("AEROCAD_CFD_DIR"); !over.empty())
        return over;
    const fs::path h = home();
    return (h.empty() ? fs::temp_directory_path() : h) / ".aerocad" / "cfd";
}

RunManager::RunManager(fs::path root) : m_root(std::move(root)) { rescan(); }

RunManager::~RunManager() = default;

RunManager::Run* RunManager::find(const std::string& id)
{
    const auto it = m_runs.find(id);
    return it == m_runs.end() ? nullptr : it->second.get();
}

void RunManager::rescan()
{
    std::error_code ec;
    std::vector<std::string> seen;
    for (const auto& e : fs::directory_iterator(m_root, ec)) {
        if (!e.is_directory() || !fs::is_regular_file(e.path() / "run.json", ec))
            continue;
        const std::string id = e.path().filename().string();
        seen.push_back(id);
        if (!m_runs.count(id))
            loadRun(e.path());
    }
    for (auto it = m_runs.begin(); it != m_runs.end();) {
        const bool owned = it->second->process.has_value();
        if (!owned && std::find(seen.begin(), seen.end(), it->first) == seen.end())
            it = m_runs.erase(it);
        else
            ++it;
    }
}

void RunManager::loadRun(const fs::path& dir)
{
    std::ifstream in(dir / "run.json");
    Json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        return;
    }
    auto run = std::make_unique<Run>();
    run->id = dir.filename().string();
    run->dir = dir;
    run->caseDir = j.contains("caseDir") ? fs::path(j.at("caseDir").get<std::string>()) : dir;
    run->imported = j.value("imported", false);
    run->created = j.value("created", std::string());
    run->backend = parseBackend(j.value("backend", std::string("docker"))).value_or(Backend::Docker);
    run->source = sourceFrom(j.value("source", Json::object()));
    run->docker = j.value("docker", std::string());
    m_runs[run->id] = std::move(run);
}

std::string RunManager::newRunId(const std::string& partName)
{
    const std::string stamp = nowIso();
    std::string base;
    for (const char c : stamp)
        if (std::isdigit(static_cast<unsigned char>(c)))
            base += c;
    base = base.substr(0, 8) + "-" + base.substr(8, 6) + "-" + slug(partName);
    std::string id = base;
    std::error_code ec;
    for (int i = 2; m_runs.count(id) || fs::exists(m_root / id, ec); ++i)
        id = base + "-" + std::to_string(i);
    return id;
}

std::string RunManager::create(const CaseGeometry& geometry, const Settings& settings, const RunSource& source,
                               std::optional<Backend> backend)
{
    const std::string id = newRunId(source.partName);
    const fs::path dir = m_root / id;
    try {
        writeCase(dir, geometry, settings);
    } catch (...) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        throw;
    }
    auto run = std::make_unique<Run>();
    run->id = id;
    run->dir = dir;
    run->caseDir = dir;
    run->created = nowIso();
    run->source = source;
    run->backend = backend.value_or(Backend::Docker);
    Json j{{"id", id},
           {"created", run->created},
           {"backend", backendName(run->backend)},
           {"source", sourceJson(source)},
           {"engine", backend ? "started" : "none"}};
    std::ofstream(dir / "run.json") << j.dump(2) << "\n";
    m_runs[id] = std::move(run);
    return id;
}

std::string RunManager::prepare(const CaseGeometry& geometry, const Settings& settings, const RunSource& source)
{
    const std::string id = create(geometry, settings, source, std::nullopt);
    std::ofstream(m_root / id / "aerocad.status") << "prepared\n";
    return id;
}

std::string RunManager::start(const CaseGeometry& geometry, const Settings& settings, const RunSource& source,
                              std::optional<Backend> backend)
{
    const Environment e = detectEnvironment();
    const Backend chosen = backend ? *backend : e.recommended.value_or(Backend::Docker);
    if (chosen == Backend::Docker && !e.dockerRunning)
        throw std::runtime_error(e.message.empty() ? "Docker is not running" : e.message);
    if (chosen == Backend::Local && e.openfoamBashrc.empty())
        throw std::runtime_error("No local OpenFOAM " + std::string(kOpenFoamVersion) +
                                 " found (set AEROCAD_OPENFOAM_BASHRC to its etc/bashrc)");

    const std::string id = create(geometry, settings, source, chosen);
    Run& run = *m_runs.at(id);
    std::vector<std::string> argv;
    if (chosen == Backend::Docker) {
        run.docker = e.docker;
        argv = {e.docker.string(), "run", "--rm", "--name", containerName(id)};
#ifndef _WIN32
        const auto uid = ::getuid();
        const auto gid = ::getgid();
        argv.insert(argv.end(), {"--user", std::to_string(uid) + ":" + std::to_string(gid)});
        if (uid == 0) // Open MPI refuses to run as root unless told otherwise.
            argv.insert(argv.end(), {"-e", "OMPI_ALLOW_RUN_AS_ROOT=1", "-e", "OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1"});
#endif
        argv.insert(argv.end(), {"-v", fs::absolute(run.caseDir).string() + ":/case", std::string(kDockerImage), "-c",
                                 "cd /case && ./Allrun"});
    } else {
        argv = {"/bin/bash", "-c", ". '" + e.openfoamBashrc.string() + "' && exec ./Allrun"};
    }
    // Keep the docker path for cancel() after a restart.
    Json j;
    {
        std::ifstream in(run.dir / "run.json");
        in >> j;
    }
    j["docker"] = run.docker.string();
    j["command"] = argv;
    std::ofstream(run.dir / "run.json") << j.dump(2) << "\n";
    std::ofstream(run.caseDir / "aerocad.stage") << "starting\n";
    run.process = Process::start(argv, run.caseDir, run.dir / "log.aerocad");
    return id;
}

std::string RunManager::importCase(const fs::path& caseDir, const std::string& label)
{
    std::error_code ec;
    const fs::path abs = fs::absolute(caseDir, ec);
    if (!fs::is_regular_file(abs / "case.json", ec))
        throw std::runtime_error(abs.string() + " is not an AeroCAD case (no case.json)");
    RunSource source;
    source.partName = abs.filename().string();
    source.label = label;
    const std::string id = newRunId(source.partName);
    const fs::path dir = m_root / id;
    fs::create_directories(dir);
    Json j{{"id", id},
           {"created", nowIso()},
           {"backend", "local"},
           {"imported", true},
           {"caseDir", abs.string()},
           {"source", sourceJson(source)}};
    std::ofstream(dir / "run.json") << j.dump(2) << "\n";
    loadRun(dir);
    return id;
}

void RunManager::refresh(Run& run)
{
    RunStatus& s = run.status;
    s.id = run.id;
    s.created = run.created;
    s.source = run.source;
    s.backend = run.backend;
    s.caseDir = run.caseDir;
    s.message.clear();

    if (run.stopper)
        run.stopper->running();

    std::error_code ec;
    const std::string status = readFirstLine(run.caseDir / "aerocad.status");
    s.stage = readFirstLine(run.caseDir / "aerocad.stage");
    const bool cancelled = fs::exists(run.caseDir / "aerocad.cancelled", ec);
    const bool processRunning = run.process && run.process->running();

    if (const auto info = caseInfo(run.id))
        s.maxIterations = info->settings.effectiveIterations();

    const Table coeffs = readTable(run.caseDir / "postProcessing" / "forceCoeffs" / "0" / "coefficient.dat");
    const Table forces = readTable(run.caseDir / "postProcessing" / "forces" / "0" / "force.dat");
    s.iteration = coeffs.empty() ? 0 : static_cast<int>(coeffs.time.back());
    s.cd = lastOf(coeffs, "Cd");
    s.cl = lastOf(coeffs, "Cl");
    s.cs = lastOf(coeffs, "Cs");
    s.dragN = lastOf(forces, "total_x");
    s.sideN = lastOf(forces, "total_y");
    s.liftN = lastOf(forces, "total_z");

    if (status == "finished") {
        s.state = RunState::Finished;
    } else if (status == "prepared") {
        s.state = RunState::Interrupted;
        s.message = "Case written only; run ./Allrun in an OpenFOAM " + std::string(kOpenFoamVersion) +
                    " environment, then refresh.";
    } else if (status.rfind("failed", 0) == 0) {
        s.state = RunState::Failed;
        const std::string step = status.size() > 7 ? status.substr(7) : std::string("unknown");
        const std::string log = stageLog(step);
        s.message = "Step '" + step + "' failed (" + log + "): " + errorSummary(run.caseDir / log);
    } else if (cancelled) {
        s.state = processRunning ? RunState::Running : RunState::Cancelled;
        s.message = processRunning ? "Stopping..." : "Cancelled";
    } else if (processRunning) {
        s.state = RunState::Running;
    } else if (run.process) {
        s.state = RunState::Failed;
        s.message = "Simulation engine exited with code " + std::to_string(run.process->exitCode().value_or(-1)) +
                    ": " + errorSummary(run.dir / "log.aerocad");
    } else if (run.imported && !coeffs.empty()) {
        s.state = RunState::Finished;
    } else {
        // Started by an earlier session of the app: still running if it wrote recently.
        const auto newest = newestLogTime(run.caseDir);
        const bool recent = newest && fs::file_time_type::clock::now() - *newest < std::chrono::minutes(3);
        s.state = recent ? RunState::Running : RunState::Interrupted;
        if (s.state == RunState::Interrupted)
            s.message = "The run stopped without finishing (the app or the engine was closed).";
    }

    double p = stageProgress(s.stage);
    if (s.stage == "solve" && s.maxIterations > 0)
        p = 0.30 + 0.66 * std::min(1.0, static_cast<double>(s.iteration) / s.maxIterations);
    if (s.state == RunState::Finished)
        p = 1.0;
    if (s.stage == "starting" && s.state == RunState::Running)
        s.message = "Starting OpenFOAM (the first run downloads the image, this can take a few minutes)...";
    s.progress = p;
}

std::vector<RunStatus> RunManager::list()
{
    std::vector<RunStatus> out;
    for (auto& [id, run] : m_runs) {
        refresh(*run);
        out.push_back(run->status);
    }
    std::sort(out.begin(), out.end(), [](const RunStatus& a, const RunStatus& b) { return a.id < b.id; });
    return out;
}

std::optional<RunStatus> RunManager::status(const std::string& id)
{
    Run* run = find(id);
    if (run == nullptr)
        return std::nullopt;
    refresh(*run);
    return run->status;
}

bool RunManager::cancel(const std::string& id)
{
    Run* run = find(id);
    if (run == nullptr)
        return false;
    refresh(*run);
    if (run->status.state != RunState::Running)
        return false;
    // A local engine started by an earlier session of the app cannot be identified
    // safely (process ids are reused), so it is not ours to stop. Docker containers are
    // found by name.
    if (run->backend == Backend::Local && !run->process)
        return false;
    std::ofstream(run->caseDir / "aerocad.cancelled") << nowIso() << "\n";
    if (run->backend == Backend::Docker) {
        // Stopping the docker CLI does not reliably stop the container; kill it by name.
        fs::path docker = run->docker.empty() ? findDocker() : run->docker;
        if (!docker.empty()) {
            try {
                run->stopper = Process::start({docker.string(), "kill", containerName(id)}, run->dir,
                                              run->dir / "log.aerocad");
            } catch (const std::exception&) {
            }
        }
    }
    if (run->process)
        run->process->terminate();
    return true;
}

bool RunManager::remove(const std::string& id)
{
    Run* run = find(id);
    if (run == nullptr)
        return false;
    refresh(*run);
    if (run->status.state == RunState::Running)
        return false;
    std::error_code ec;
    fs::remove_all(run->dir, ec); // An imported case directory itself is left alone.
    m_runs.erase(id);
    return !ec;
}

std::optional<CaseInfo> RunManager::caseInfo(const std::string& id)
{
    Run* run = find(id);
    if (run == nullptr)
        return std::nullopt;
    std::ifstream in(run->caseDir / "case.json");
    if (!in)
        return std::nullopt;
    try {
        Json j;
        in >> j;
        return CaseInfo::fromJson(j);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

namespace {

std::vector<std::size_t> sampleIndices(std::size_t n, int maxPoints)
{
    std::vector<std::size_t> idx;
    if (n == 0)
        return idx;
    const auto m = static_cast<std::size_t>(std::max(2, maxPoints));
    if (n <= m) {
        for (std::size_t i = 0; i < n; ++i)
            idx.push_back(i);
        return idx;
    }
    for (std::size_t k = 0; k < m; ++k)
        idx.push_back(k * (n - 1) / (m - 1));
    return idx;
}

Json sampled(const std::vector<double>* values, const std::vector<std::size_t>& idx)
{
    Json out = Json::array();
    for (const auto i : idx) {
        const double v = values != nullptr && i < values->size() ? (*values)[i] : std::nan("");
        out.push_back(std::isfinite(v) ? Json(v) : Json(nullptr));
    }
    return out;
}

/// Mean over the last 20 % of the iterations (at least the last one).
std::optional<double> tailMean(const std::vector<double>* v)
{
    if (v == nullptr || v->empty())
        return std::nullopt;
    const std::size_t start = v->size() - std::max<std::size_t>(1, v->size() / 5);
    double sum = 0;
    for (std::size_t i = start; i < v->size(); ++i)
        sum += (*v)[i];
    return sum / static_cast<double>(v->size() - start);
}

Json opt(const std::optional<double>& v) { return v ? Json(*v) : Json(nullptr); }

std::pair<double, double> range(const std::vector<double>& v)
{
    double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
    for (const double x : v)
        if (std::isfinite(x)) {
            lo = std::min(lo, x);
            hi = std::max(hi, x);
        }
    return {lo, hi};
}

} // namespace

Json RunManager::results(const std::string& id, int maxPoints)
{
    const auto st = status(id);
    if (!st)
        throw std::runtime_error("No simulation run '" + id + "'");
    const Run& run = *find(id);
    const auto info = caseInfo(id);
    const Table coeffs = readTable(run.caseDir / "postProcessing" / "forceCoeffs" / "0" / "coefficient.dat");
    const Table forces = readTable(run.caseDir / "postProcessing" / "forces" / "0" / "force.dat");
    const Table residuals = readTable(run.caseDir / "postProcessing" / "solverInfo" / "0" / "solverInfo.dat");

    Json j{{"run", st->toJson()}};
    if (info) {
        j["conditions"] = {{"settings", toJson(info->settings)},
                           {"openfoam", kOpenFoamVersion},
                           {"referenceArea", info->referenceArea},
                           {"referenceLength", info->referenceLength},
                           {"characteristicLength", info->characteristicLength},
                           {"baseCellSize", info->baseCellSize},
                           {"geometryHash", std::to_string(info->geometryHash)},
                           {"axes", "drag = along the flow, lift = +Z (model up), side = 90 degrees left of the flow"}};
    }
    j["summary"] = {{"cd", opt(tailMean(coeffs.column("Cd")))},
                    {"cl", opt(tailMean(coeffs.column("Cl")))},
                    {"cs", opt(tailMean(coeffs.column("Cs")))},
                    {"dragN", opt(tailMean(forces.column("total_x")))},
                    {"liftN", opt(tailMean(forces.column("total_z")))},
                    {"sideN", opt(tailMean(forces.column("total_y")))},
                    {"pressureDragN", opt(tailMean(forces.column("pressure_x")))},
                    {"viscousDragN", opt(tailMean(forces.column("viscous_x")))},
                    {"iterations", coeffs.time.size()},
                    {"averaging", "mean over the last 20 % of the iterations"}};

    const auto ci = sampleIndices(coeffs.time.size(), maxPoints);
    const auto fi = sampleIndices(forces.time.size(), maxPoints);
    j["history"] = {{"iteration", sampled(&coeffs.time, ci)},
                    {"cd", sampled(coeffs.column("Cd"), ci)},
                    {"cl", sampled(coeffs.column("Cl"), ci)},
                    {"cs", sampled(coeffs.column("Cs"), ci)},
                    {"forceIteration", sampled(&forces.time, fi)},
                    {"dragN", sampled(forces.column("total_x"), fi)},
                    {"liftN", sampled(forces.column("total_z"), fi)},
                    {"sideN", sampled(forces.column("total_y"), fi)}};
    const auto ri = sampleIndices(residuals.time.size(), maxPoints);
    Json res{{"iteration", sampled(&residuals.time, ri)}};
    for (const char* f : {"Ux", "Uy", "Uz", "p", "k", "omega"})
        if (const auto* col = residuals.column(std::string(f) + "_initial"))
            res[f] = sampled(col, ri);
    j["residuals"] = std::move(res);
    j["quality"] = assessQuality(run.caseDir).toJson();

    Json fields = Json::object();
    for (const char* kind : {"surface", "sectionHorizontal", "sectionVertical", "streamlines"}) {
        const bool streamlines = std::string_view(kind) == "streamlines";
        const fs::path base = streamlines ? run.caseDir / "postProcessing" / "sets" / "streamLines"
                                          : run.caseDir / "postProcessing" / "surfaces";
        const auto dirs = timeDirectories(base);
        const std::string file = streamlines ? "track0.vtk" : std::string_view(kind) == "surface"
                                                                  ? "part.vtk"
                                                                  : std::string(kind) + ".vtk";
        std::error_code ec;
        fields[kind] = !dirs.empty() && fs::is_regular_file(dirs.back() / file, ec);
    }
    j["fields"] = std::move(fields);
    return j;
}

PolyData RunManager::field(const std::string& id, std::string_view kind)
{
    Run* run = find(id);
    if (run == nullptr)
        throw std::runtime_error("No simulation run '" + id + "'");
    const auto info = caseInfo(id);
    if (!info)
        throw std::runtime_error("Run '" + id + "' has no case.json");
    fs::path base;
    std::string file;
    if (kind == "streamlines") {
        base = run->caseDir / "postProcessing" / "sets" / "streamLines";
        file = "track0.vtk";
    } else if (kind == "surface" || kind == "sectionHorizontal" || kind == "sectionVertical") {
        base = run->caseDir / "postProcessing" / "surfaces";
        file = kind == "surface" ? "part.vtk" : std::string(kind) + ".vtk";
    } else {
        throw std::runtime_error("Unknown field '" + std::string(kind) +
                                 "' (surface, sectionHorizontal, sectionVertical, streamlines)");
    }
    const auto dirs = timeDirectories(base);
    std::error_code ec;
    if (dirs.empty() || !fs::is_regular_file(dirs.back() / file, ec))
        return {};
    PolyData d = readVtk(dirs.back() / file);
    for (auto& p : d.points)
        p = info->frame.toModel(p);
    const double q = 0.5 * info->settings.speed * info->settings.speed;
    if (auto it = d.vectors.find("U"); it != d.vectors.end()) {
        std::vector<double> speed(it->second.size());
        for (std::size_t i = 0; i < it->second.size(); ++i) {
            speed[i] = it->second[i].length();
            it->second[i] = info->frame.unrotate(it->second[i]);
        }
        d.scalars["speed"] = std::move(speed);
    }
    if (auto it = d.scalars.find("p"); it != d.scalars.end()) {
        // p is kinematic (p / rho) in incompressible solvers.
        std::vector<double> cp(it->second.size()), pa(it->second.size());
        for (std::size_t i = 0; i < cp.size(); ++i) {
            cp[i] = it->second[i] / q;
            pa[i] = it->second[i] * info->settings.density;
        }
        d.scalars["Cp"] = std::move(cp);
        d.scalars["pressure"] = std::move(pa);
    }
    return d;
}

Json RunManager::compare(const std::string& a, const std::string& b)
{
    const Json ra = results(a);
    const Json rb = results(b);
    Json j{{"a", ra.at("run").at("id")}, {"b", rb.at("run").at("id")}};

    const Json ca = ra.value("conditions", Json::object());
    const Json cb = rb.value("conditions", Json::object());
    const auto diff = differingConditions(ca.value("settings", Json::object()), cb.value("settings", Json::object()));
    j["sameConditions"] = diff.empty();
    j["differingConditions"] = diff;
    std::vector<std::string> notes;
    const double areaA = ca.value("referenceArea", 0.0), areaB = cb.value("referenceArea", 0.0);
    const double lenA = ca.value("characteristicLength", 0.0), lenB = cb.value("characteristicLength", 0.0);
    if (lenA > 0 && lenB > 0 && std::abs(lenA - lenB) > 0.1 * std::max(lenA, lenB))
        notes.emplace_back("The parts differ in size by more than 10 %, so the domain and mesh sizes differ too.");

    const Json sa = ra.at("summary"), sb = rb.at("summary");
    Json deltas = Json::object();
    for (const char* key : {"dragN", "liftN", "sideN", "pressureDragN", "viscousDragN"}) {
        if (sa.at(key).is_number() && sb.at(key).is_number()) {
            const double va = sa.at(key).get<double>(), vb = sb.at(key).get<double>();
            Json d{{"a", va}, {"b", vb}, {"difference", vb - va}};
            d["percent"] = std::abs(va) > 1e-12 ? Json(100.0 * (vb - va) / std::abs(va)) : Json(nullptr);
            deltas[key] = std::move(d);
        }
    }
    j["forces"] = std::move(deltas);
    // Coefficients of B on A's reference area, so Cd/Cl compare like the forces do.
    Json coeffs = Json::object();
    for (const char* key : {"cd", "cl", "cs"}) {
        if (sa.at(key).is_number() && sb.at(key).is_number() && areaA > 0 && areaB > 0) {
            const double va = sa.at(key).get<double>();
            const double vb = sb.at(key).get<double>() * areaB / areaA;
            coeffs[key] = {{"a", va}, {"b", vb}, {"difference", vb - va}};
        }
    }
    j["coefficientsOnReferenceAreaOfA"] = std::move(coeffs);
    if (areaA > 0 && areaB > 0 && std::abs(areaA - areaB) > 1e-9 * std::max(areaA, areaB))
        notes.emplace_back("Reference areas differ (" + std::to_string(areaA) + " vs " + std::to_string(areaB) +
                           " m^2); coefficients of B are rescaled to the area of A.");
    j["quality"] = {{"a", ra.at("quality")}, {"b", rb.at("quality")}};
    // Mesh-refinement sensitivity of the difference is not assessed yet (CAD-54).
    j["verdict"] = !diff.empty() ? "not comparable"
                   : (ra.at("quality").at("status") == "ok" && rb.at("quality").at("status") == "ok")
                       ? "comparable"
                       : "check quality";

    // Shared colour scales for the field views.
    Json scales = Json::object();
    for (const auto& [kind, scalar] :
         std::vector<std::pair<const char*, const char*>>{{"surface", "Cp"}, {"sectionHorizontal", "speed"}}) {
        double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
        for (const auto& id : {a, b}) {
            const PolyData d = field(id, kind);
            if (auto it = d.scalars.find(scalar); it != d.scalars.end()) {
                const auto [l, h] = range(it->second);
                lo = std::min(lo, l);
                hi = std::max(hi, h);
            }
        }
        if (lo <= hi)
            scales[scalar] = {lo, hi};
    }
    j["colorScales"] = std::move(scales);
    j["notes"] = notes;
    return j;
}

} // namespace aerocad::cfd
