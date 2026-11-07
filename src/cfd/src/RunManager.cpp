// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cfd/RunManager.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits>
#include <set>
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
           {"remoteHost", remoteHost},
           {"remoteCpus", remoteCpus},
           {"remoteError", remoteError},
           {"message", message}};
    j["recommendedBackend"] = recommended ? Json(backendName(*recommended)) : Json(nullptr);
    return j;
}

bool isValidEngineHost(std::string_view host)
{
    if (host.empty())
        return true;
    if (host.size() > 255)
        return false;
    if (host.rfind("unix://", 0) == 0) // A local socket, e.g. to test the remote path.
        return host.size() > 7 && std::all_of(host.begin() + 7, host.end(), [](char c) {
                   return std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '.' || c == '-' || c == '_';
               });
    if (host.rfind("ssh://", 0) != 0 || host.size() <= 6)
        return false;
    // user@host[:port] only: no spaces, quotes or shell characters.
    return std::all_of(host.begin() + 6, host.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '@' || c == '.' || c == '-' || c == '_' ||
               c == ':' || c == '[' || c == ']';
    });
}

namespace {

fs::path engineConfigPath() { return RunManager::defaultRoot() / "engine.json"; }

} // namespace

std::string engineHost()
{
    if (const std::string over = env("AEROCAD_ENGINE_HOST"); !over.empty())
        return isValidEngineHost(over) ? over : std::string();
    std::ifstream in(engineConfigPath());
    if (!in)
        return {};
    try {
        Json j;
        in >> j;
        const std::string host = j.value("remoteHost", std::string());
        return isValidEngineHost(host) ? host : std::string();
    } catch (const std::exception&) {
        return {};
    }
}

void setEngineHost(const std::string& host)
{
    if (!isValidEngineHost(host))
        throw std::invalid_argument("The remote engine must be ssh://user@host (optionally :port)");
    fs::create_directories(engineConfigPath().parent_path());
    std::ofstream(engineConfigPath()) << Json{{"remoteHost", host}}.dump(2) << "\n";
}

std::vector<std::string> dockerCommand(const fs::path& docker, const std::string& host)
{
    std::vector<std::string> argv{docker.string()};
    if (!host.empty())
        argv.insert(argv.end(), {"-H", host});
    return argv;
}

Environment detectEnvironment() { return detectEnvironment(engineHost()); }

Environment detectEnvironment(const std::string& remoteHost)
{
    Environment e;
    e.docker = findDocker();
    e.remoteHost = remoteHost;
    extendToolPath(e.docker);
    if (!e.docker.empty()) {
        // Over SSH every call opens a connection: allow more time.
        const auto timeout = std::chrono::seconds(e.remoteHost.empty() ? 10 : 25);
        auto cmd = [&](std::vector<std::string> args) {
            auto argv = dockerCommand(e.docker, e.remoteHost);
            argv.insert(argv.end(), args.begin(), args.end());
            return runCommand(argv, timeout);
        };
        const auto info = cmd({"info", "--format", "{{.NCPU}}"});
        // Some docker versions exit 0 and print 0 CPUs when the daemon does not answer. Warnings
        // (stderr is in the output too) are on lines of their own.
        int cpus = 0;
        {
            std::istringstream lines(info.output);
            for (std::string line; std::getline(lines, line);) {
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
                    line.pop_back();
                if (!line.empty() && line.find_first_not_of("0123456789") == std::string::npos)
                    cpus = std::atoi(line.c_str());
            }
        }
        e.dockerRunning = info.exitCode == 0 && cpus > 0;
        if (e.dockerRunning) {
            if (!e.remoteHost.empty())
                e.remoteCpus = cpus;
            e.imagePresent = cmd({"image", "inspect", std::string(kDockerImage), "--format", "{{.Id}}"}).exitCode == 0;
        } else if (!e.remoteHost.empty()) {
            e.remoteError = info.output;
        }
    }
    if (!e.remoteHost.empty()) {
        // A remote engine is used only remotely, never a local OpenFOAM instead.
        if (e.dockerRunning) {
            e.recommended = Backend::Docker;
            e.message = "Remote engine " + e.remoteHost + " (" + std::to_string(e.remoteCpus) + " CPUs) is ready" +
                        (e.imagePresent ? "." : "; the first run downloads the OpenFOAM image there.");
        } else {
            e.message = e.docker.empty() ? "The docker command line tool is needed to reach the remote engine " +
                                               e.remoteHost + " (macOS: brew install docker)."
                                         : "Cannot reach Docker on " + e.remoteHost +
                                               ": check that SSH login with a key works and that this user may run "
                                               "docker there.";
        }
        return e;
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
        e.message = "Docker is installed but not running. Start its engine (Colima: colima start) and try again.";
    } else {
        e.message = "No simulation engine found. Install Docker (on macOS: brew install docker colima, then "
                    "colima start); AeroCAD runs OpenFOAM " +
                    std::string(kOpenFoamVersion) + " in the official image " + std::string(kDockerImage) + ".";
    }
    return e;
}

// --- Engine setup ("Install for me") ----------------------------------------------------

Platform currentPlatform()
{
#if defined(__APPLE__)
    return Platform::MacOS;
#elif defined(__linux__)
    return Platform::Linux;
#else
    return Platform::Other;
#endif
}

fs::path findBrew()
{
    return findExecutable("brew", {"/opt/homebrew/bin/brew", "/usr/local/bin/brew"});
}

SetupTools findSetupTools()
{
    SetupTools t;
    t.brew = findBrew();
    t.colima = findExecutable("colima", {"/opt/homebrew/bin/colima", "/usr/local/bin/colima"});
    std::error_code ec;
    t.dockerDesktop = fs::exists("/Applications/Docker.app", ec);
    return t;
}

namespace {

/// Opens a Terminal window running `script` (macOS).
std::vector<std::string> inTerminal(const std::string& script)
{
    return {"osascript", "-e", "tell application \"Terminal\" to activate", "-e",
            "tell application \"Terminal\" to do script \"" + script + "\""};
}

} // namespace

SetupStep planEngineSetup(const Environment& env, Platform platform, const SetupTools& tools)
{
    static const std::string linuxInstall = "https://docs.docker.com/engine/install/";
    if (env.dockerRunning && env.imagePresent)
        return {"none", {}, "Docker is running and the OpenFOAM image is present."};
    if (!env.dockerRunning && !env.openfoamBashrc.empty())
        return {"none", {}, "A local OpenFOAM installation is used."};
    if (env.dockerRunning)
        return {"pull", {}, "Downloading the OpenFOAM image " + std::string(kDockerImage) + " (about 0.4-0.5 GB)" +
                                (env.remoteHost.empty() ? std::string(".") : " on " + env.remoteHost + ".")};
    if (!env.remoteHost.empty()) {
        // Only the docker command line tool is needed here; the engine runs remotely.
        if (env.docker.empty() && platform == Platform::MacOS && !tools.brew.empty())
            return {"terminal", inTerminal(tools.brew.string() + " install docker"),
                    "Installing the docker command line tool with Homebrew in a Terminal window."};
        return {"manual", {}, env.message};
    }
    if (platform == Platform::MacOS) {
        // The Docker engine needs a Linux VM on macOS. Colima runs one without a GUI; give it
        // all cores and half of the memory, since the default (2 CPUs, 2 GB) is too small for
        // the medium and fine meshes. Changing these on an existing Colima VM restarts it.
        const std::string start = "colima start --cpu $(sysctl -n hw.ncpu) --memory $(( $(sysctl -n hw.memsize) / "
                                  "2147483648 ))";
        if (!tools.colima.empty() && !env.docker.empty())
            return {"terminal", inTerminal(tools.colima.parent_path().string() + "/" + start),
                    "Starting Colima (the Docker engine) in a Terminal window; then the OpenFOAM image is "
                    "downloaded."};
        if (!tools.brew.empty()) {
            const std::string bin = tools.brew.parent_path().string() + "/";
            // A new VM uses Apple's Virtualization framework (vz, with virtiofs mounts): Colima's
            // own default (qemu) needs QEMU, which Homebrew's colima does not install.
            return {"terminal",
                    inTerminal(tools.brew.string() + " install docker colima && " + bin + start + " --vm-type vz"),
                    "Installing the docker command line tool and Colima (a Docker engine without Docker Desktop) "
                    "with Homebrew in a Terminal window, then starting Colima; then the OpenFOAM image is "
                    "downloaded."};
        }
        if (!env.docker.empty() && tools.dockerDesktop)
            return {"start", {"open", "-a", "Docker"},
                    "Starting Docker Desktop (installed already); then the OpenFOAM image is downloaded."};
        return {"browser", {"open", "https://brew.sh"},
                "Opening the Homebrew page: install Homebrew, then press Install for me again to install the "
                "docker command line tool and Colima."};
    }
    if (!env.docker.empty())
        return {"manual", {}, "Docker is installed but not running: start it (sudo systemctl start docker)."};
    return {"browser", {"xdg-open", linuxInstall},
            "Opening the Docker Engine install page: install it, start it, then press Install for me again."};
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
    std::string dockerHost; ///< Remote engine the run was started on, empty = this machine.
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

/// Runs a case on a remote Docker engine (no shared directory): copies the case into a new
/// container, starts it, copies the progress files and results back every 10 s while it
/// runs, then the final results, and removes the container. Started again after an app
/// restart, it only follows the container that is already there. Arguments: docker CLI,
/// host, container name, image; working directory = the local case.
constexpr const char* kRemoteScript = R"SH(
set -o pipefail
DK="$1"; H="$2"; N="$3"; IMG="$4"
d() { "$DK" -H "$H" "$@"; }
fetch() {
    d exec "$N" bash -c 'cd /case && tar -cf - aerocad.stage aerocad.status aerocad.engine log.* postProcessing 2>/dev/null' |
        tar -xf - 2>/dev/null
}
if ! d container inspect "$N" >/dev/null 2>&1; then
    echo "Sending the case to $H"
    d create --name "$N" -e OMPI_ALLOW_RUN_AS_ROOT=1 -e OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1 "$IMG" -c         'cd /case && ./Allrun; s=$?; tar -cf /tmp/aerocad-results.tar aerocad.* log.* postProcessing 2>/dev/null; exit $s'         >/dev/null || exit 1
    d cp . "$N:/case" || { d rm -f "$N" >/dev/null 2>&1; exit 1; }
    d start "$N" >/dev/null || exit 1
fi
lost=0
while :; do
    if state=$(d container inspect -f '{{.State.Running}}' "$N" 2>&1); then
        [ "$state" = true ] || break
        [ "$lost" = 1 ] && echo "Connection to $H is back"
        lost=0
        fetch
    elif [[ "$state" == *"No such container"* ]]; then
        echo "The container $N is gone from $H"
        exit 1
    else
        # SSH or Docker did not answer: the run goes on there, keep trying.
        [ "$lost" = 0 ] && echo "No answer from $H, retrying: $state"
        lost=1
    fi
    sleep 10
done
code=$(d container inspect -f '{{.State.ExitCode}}' "$N" 2>/dev/null || echo 1)
for _ in 1 2 3; do
    if d cp "$N:/tmp/aerocad-results.tar" - | tar -xOf - | tar -xf -; then
        d rm "$N" >/dev/null 2>&1
        exit "$code"
    fi
    sleep 10
done
# Keep the only complete copy of the results; the next start of the app tries again.
echo "Could not copy the results from $H: the container $N is kept there; restart the app to try again"
exit 75
)SH";

std::vector<std::string> remoteArgv(const fs::path& docker, const std::string& host, const std::string& name)
{
    return {"/bin/bash", "-c", kRemoteScript, "aerocad-remote", docker.string(), host, name, std::string(kDockerImage)};
}

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
    run->dockerHost = j.value("dockerHost", std::string());
    Run& r = *run;
    m_runs[r.id] = std::move(run);
    // A remote run continues on its host while the app is closed: follow it again.
    if (!r.dockerHost.empty() && !r.docker.empty()) {
        std::error_code ec;
        const std::string status = readFirstLine(r.caseDir / "aerocad.status");
        if (status.empty() && !fs::exists(r.caseDir / "aerocad.cancelled", ec)) {
            try {
                r.process = Process::start(remoteArgv(r.docker, r.dockerHost, containerName(r.id)), r.caseDir,
                                           r.dir / "log.aerocad");
            } catch (const std::exception&) {
            }
        }
    }
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

    const bool remote = chosen == Backend::Docker && !e.remoteHost.empty();
    Settings effective = settings;
    if (remote && settings.processes <= 0 && e.remoteCpus > 0)
        effective.processes = std::clamp(e.remoteCpus, 1, 16); // The remote machine is not shared with the UI.
    const std::string id = create(geometry, effective, source, chosen);
    Run& run = *m_runs.at(id);
    std::vector<std::string> argv;
    if (remote) {
        run.docker = e.docker;
        run.dockerHost = e.remoteHost;
        argv = remoteArgv(e.docker, e.remoteHost, containerName(id));
    } else if (chosen == Backend::Docker) {
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
    if (remote)
        j["dockerHost"] = run.dockerHost;
    j["command"] = remote ? Json::array({"remote", run.dockerHost, containerName(id)}) : Json(argv);
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
    } else {
        // Imported cases and runs from an earlier session need the same completion
        // marker. Coefficient files alone may belong to a still-running or interrupted run.
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
                auto argv = dockerCommand(docker, run->dockerHost);
                // A remote container is not started with --rm: remove it, which also stops it.
                if (run->dockerHost.empty())
                    argv.insert(argv.end(), {"kill", containerName(id)});
                else
                    argv.insert(argv.end(), {"rm", "-f", containerName(id)});
                run->stopper = Process::start(argv, run->dir, run->dir / "log.aerocad");
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

/// Mean over the last 20 % of the iterations (at least the last one). Every retained
/// sample must be finite, including those before the averaging window.
std::optional<double> tailMean(const std::vector<double>* v)
{
    if (v == nullptr || v->empty() || !std::all_of(v->begin(), v->end(), [](double x) { return std::isfinite(x); }))
        return std::nullopt;
    const std::size_t start = v->size() - std::max<std::size_t>(1, v->size() / 5);
    double sum = 0;
    for (std::size_t i = start; i < v->size(); ++i) {
        sum += (*v)[i];
        if (!std::isfinite(sum))
            return std::nullopt;
    }
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
                           // The resolved setup (CAD-84): domain with the ground, surroundings, engine.
                           {"domain", {{"min", {info->domain.min.x, info->domain.min.y, info->domain.min.z}},
                                       {"max", {info->domain.max.x, info->domain.max.y, info->domain.max.z}}}},
                           {"contextHash", info->contextHash ? Json(std::to_string(*info->contextHash)) : Json(nullptr)},
                           // The engine that actually ran the case (Allrun records WM_PROJECT_VERSION in
                           // aerocad.engine); null when not recorded. engineTarget: what it was written for.
                           {"engine", [&] {
                                const std::string v = readFirstLine(run.caseDir / "aerocad.engine");
                                return v.empty() ? Json(nullptr) : Json(v);
                            }()},
                           {"engineTarget", info->openfoam.empty() ? Json(nullptr) : Json(info->openfoam)},
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

    // Histories are useful while solving, but their current tail is not a final result.
    // Keep this gate independent of the numerical quality checks: even apparently good
    // partial output must never become a final summary or an A/B verdict.
    constexpr const char* resultKeys[] = {"cd", "cl", "cs", "dragN", "liftN", "sideN", "pressureDragN", "viscousDragN"};
    std::vector<std::string> reasons;
    if (st->state != RunState::Finished)
        reasons.emplace_back("The run has not finished successfully (" + std::string(runStateName(st->state)) + ").");
    if (std::any_of(std::begin(resultKeys), std::end(resultKeys), [&](const char* key) {
            return !j.at("summary").at(key).is_number();
        }))
        reasons.emplace_back("Required coefficient or force results are missing or non-finite.");
    if (coeffs.incomplete || forces.incomplete || coeffs.invalidTime || forces.invalidTime)
        reasons.emplace_back("Coefficient or force output contains an incomplete row or invalid iteration.");
    if (!coeffs.empty() && !forces.empty() &&
        (!std::isfinite(coeffs.time.back()) || !std::isfinite(forces.time.back()) ||
         coeffs.time != forces.time))
        reasons.emplace_back("Force and coefficient outputs do not cover the same valid iterations.");
    const bool finalAvailable = reasons.empty();
    j["finalResults"] = {{"available", finalAvailable}, {"reasons", reasons}};
    j["provisional"] = !finalAvailable;
    if (!finalAvailable)
        for (const char* key : resultKeys)
            j["summary"][key] = nullptr;

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

bool RunManager::startImagePull(const fs::path& docker, const std::string& host)
{
    if (m_imagePull && m_imagePull->running())
        return false;
    fs::create_directories(m_root);
    auto argv = dockerCommand(docker, host);
    argv.insert(argv.end(), {"pull", std::string(kDockerImage)});
    m_imagePull = Process::start(argv, m_root, m_root / "image-pull.log");
    return true;
}

Json RunManager::imagePullStatus()
{
    if (!m_imagePull)
        return nullptr;
    const bool running = m_imagePull->running();
    // `docker pull` without a terminal prints one line per layer event:
    // "<layer>: Pulling fs layer", "<layer>: Pull complete", "<layer>: Already exists".
    std::ifstream in(m_root / "image-pull.log");
    std::set<std::string> layers, done;
    std::string line, last;
    while (std::getline(in, line)) {
        if (!line.empty())
            last = line;
        const auto colon = line.find(": ");
        if (colon == std::string::npos)
            continue;
        const std::string layer = line.substr(0, colon), what = line.substr(colon + 2);
        if (what == "Pulling fs layer" || what == "Waiting")
            layers.insert(layer);
        else if (what == "Pull complete" || what == "Already exists") {
            layers.insert(layer);
            done.insert(layer);
        }
    }
    Json j;
    if (running) {
        j["state"] = "running";
        j["progress"] = layers.empty() ? 0.0 : static_cast<double>(done.size()) / static_cast<double>(layers.size());
        j["message"] = "Downloading the OpenFOAM image: " + std::to_string(done.size()) + " of " +
                       std::to_string(layers.size()) + " parts.";
    } else if (m_imagePull->exitCode() == 0) {
        j["state"] = "finished";
        j["progress"] = 1.0;
        j["message"] = "The OpenFOAM image is ready.";
    } else {
        j["state"] = "failed";
        j["progress"] = 0.0;
        j["message"] = "Downloading the OpenFOAM image failed: " + last;
    }
    return j;
}

namespace {

int meshRank(const std::string& level)
{
    const auto parsed = parseMeshLevel(level);
    return parsed ? static_cast<int>(*parsed) : -1;
}

} // namespace

std::map<std::string, std::vector<RunStatus>> RunManager::sameCaseOnOtherMeshes(const std::string& id)
{
    std::map<std::string, std::vector<RunStatus>> out;
    const auto self = caseInfo(id);
    if (!self)
        return out;
    const Json settings = toJson(self->settings);
    for (const RunStatus& other : list()) {
        if (other.id == id || (other.state != RunState::Finished && other.state != RunState::Running))
            continue;
        const auto info = caseInfo(other.id);
        if (!info || info->geometryHash != self->geometryHash || info->settings.mesh == self->settings.mesh)
            continue;
        if (differingConditions(settings, toJson(info->settings)) != std::vector<std::string>{"mesh"})
            continue;
        out[std::string(meshLevelName(info->settings.mesh))].push_back(other);
    }
    return out;
}

Json RunManager::meshSensitivity(const std::string& a, const std::string& b)
{
    const auto infoA = caseInfo(a);
    if (!infoA)
        return Json{{"status", "not assessed"}, {"levels", Json::array()}, {"message", "No case information."}};
    const std::string own(meshLevelName(infoA->settings.mesh));
    auto othersA = sameCaseOnOtherMeshes(a);
    auto othersB = sameCaseOnOtherMeshes(b);

    // A and B per mesh level: the compared pair, and the newest finished pair per other level.
    struct Pair {
        std::string mesh, a, b;
    };
    std::vector<Pair> pairs{{own, a, b}};
    bool pending = false;
    auto newestFinished = [](const std::vector<RunStatus>& runs) -> std::optional<std::string> {
        std::optional<std::string> best;
        for (const RunStatus& r : runs) // Run ids start with the creation time.
            if (r.state == RunState::Finished && (!best || r.id > *best))
                best = r.id;
        return best;
    };
    for (const auto& [mesh, runsA] : othersA) {
        const auto itB = othersB.find(mesh);
        const auto fa = newestFinished(runsA);
        const auto fb = itB != othersB.end() ? newestFinished(itB->second) : std::nullopt;
        if (fa && fb) {
            pairs.push_back({mesh, *fa, *fb});
            continue;
        }
        auto running = [](const std::vector<RunStatus>& runs) {
            return std::any_of(runs.begin(), runs.end(), [](const RunStatus& r) { return r.state == RunState::Running; });
        };
        pending = pending || running(runsA) || (itB != othersB.end() && running(itB->second));
    }
    for (const auto& [mesh, runsB] : othersB)
        if (othersA.count(mesh) == 0)
            pending = pending || std::any_of(runsB.begin(), runsB.end(),
                                             [](const RunStatus& r) { return r.state == RunState::Running; });
    std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return meshRank(x.mesh) < meshRank(y.mesh); });

    Json levels = Json::array();
    std::map<std::string, std::vector<double>> differences; // Force -> difference per level (coarse first).
    std::vector<bool> levelOk; // Both runs of the level have quality status ok.
    for (const Pair& p : pairs) {
        const Json ra = results(p.a, 2);
        const Json rb = results(p.b, 2);
        const Json& sa = ra.at("summary");
        const Json& sb = rb.at("summary");
        const bool ok = ra.at("quality").at("status") == "ok" && rb.at("quality").at("status") == "ok";
        levelOk.push_back(ok);
        Json level{{"mesh", p.mesh}, {"a", p.a}, {"b", p.b}, {"qualityOk", ok}};
        for (const char* key : {"dragN", "liftN"}) {
            if (!sa.at(key).is_number() || !sb.at(key).is_number())
                continue;
            const double d = sb.at(key).get<double>() - sa.at(key).get<double>();
            level[key] = {{"a", sa.at(key)}, {"b", sb.at(key)}, {"difference", d}};
            differences[key].push_back(d);
        }
        levels.push_back(std::move(level));
    }

    Json j{{"levels", levels}};
    const bool complete = differences["dragN"].size() == pairs.size() && differences["liftN"].size() == pairs.size();
    if (pairs.size() < 2 || !complete) {
        j["status"] = pending ? "pending" : "not assessed";
        const int rank = meshRank(own);
        const std::string next = rank < 2 ? std::string(meshLevelName(static_cast<MeshLevel>(rank + 1)))
                                          : std::string(meshLevelName(static_cast<MeshLevel>(rank - 1)));
        j["suggestedMesh"] = next;
        j["message"] = pending ? "Waiting for the runs on the other mesh level."
                               : "Run A and B on the " + next + " mesh too to check that the difference holds.";
        return j;
    }

    // The assessment uses the two finest levels: their runs must be trustworthy first.
    const std::size_t n = pairs.size();
    for (const std::size_t i : {n - 2, n - 1}) {
        if (!levelOk[i]) {
            j["status"] = "check quality";
            j["meshes"] = {pairs[n - 2].mesh, pairs[n - 1].mesh};
            j["message"] = "A run on the " + pairs[i].mesh +
                           " mesh has quality warnings: check it before reading the mesh check.";
            return j;
        }
    }

    // Two finest levels: the difference on the finer one, and how much refinement moved it.
    std::string deciding;
    double largest = -1.0;
    bool inconclusive = false;
    for (const char* key : {"dragN", "liftN"}) {
        const auto& d = differences[key];
        const double fine = d[d.size() - 1], coarse = d[d.size() - 2];
        const double change = std::abs(fine - coarse);
        const bool flips = fine * coarse < 0.0;
        const bool unsure = flips || (change > 0.0 && change >= 0.5 * std::abs(fine)); // No change: holds.
        j[key] = {{"difference", fine},
                  {"previousDifference", coarse},
                  {"change", change},
                  {"signChanged", flips},
                  {"status", unsure ? "inconclusive" : "holds"}};
        if (std::abs(fine) > largest) {
            largest = std::abs(fine);
            deciding = key;
            inconclusive = unsure;
        }
    }
    // The verdict follows the force with the larger A/B difference (drag for most parts,
    // lift for wings and diffusers); both are reported.
    j["decidingForce"] = deciding;
    j["status"] = inconclusive ? "inconclusive" : "holds";
    j["meshes"] = {pairs[pairs.size() - 2].mesh, pairs.back().mesh};
    const char* other = deciding == "dragN" ? "liftN" : "dragN";
    auto name = [](const std::string& key) { return key == "dragN" ? std::string("drag") : std::string("lift"); };
    std::string message =
        inconclusive ? "The " + name(deciding) +
                           " difference changes with the mesh about as much as its own size: difference inconclusive."
                     : "The " + name(deciding) + " difference holds on the finer mesh.";
    if (j[other].at("status") == "inconclusive")
        message += " The smaller " + name(other) + " difference does not" +
                   (j[other].at("signChanged").get<bool>() ? " (it changes sign)." : ".");
    j["message"] = message;
    return j;
}

Json RunManager::compare(const std::string& a, const std::string& b)
{
    const Json ra = results(a);
    const Json rb = results(b);
    Json j{{"a", ra.at("run").at("id")}, {"b", rb.at("run").at("id")}};
    j["states"] = {{"a", ra.at("run").at("state")}, {"b", rb.at("run").at("state")}};
    std::vector<std::string> reasons;
    for (const auto& reason : ra.at("finalResults").at("reasons"))
        reasons.emplace_back("Run A: " + reason.get<std::string>());
    for (const auto& reason : rb.at("finalResults").at("reasons"))
        reasons.emplace_back("Run B: " + reason.get<std::string>());
    const bool finalAvailable = reasons.empty();
    j["reasons"] = reasons;

    const Json ca = ra.value("conditions", Json::object());
    const Json cb = rb.value("conditions", Json::object());
    auto diff = differingConditions(ca.value("settings", Json::object()), cb.value("settings", Json::object()));
    std::vector<std::string> notes;
    // The resolved setup, not only the requested settings (CAD-84): surroundings, domain
    // (with the ground) and engine. Missing evidence is unknown, never equal.
    std::vector<std::string> unknown;
    const Json ctxA = ca.value("contextHash", Json()), ctxB = cb.value("contextHash", Json());
    if (ctxA.is_null() || ctxB.is_null())
        unknown.emplace_back("context");
    else if (ctxA != ctxB)
        diff.emplace_back("context");
    // The domain is sized from the target part and the surroundings, so a target-part variant
    // of another size gets its own domain. That is reported as a differing condition; when it
    // is the only one, the forces are still compared and the verdict says so.
    bool sameDomain = false;
    if (!ca.contains("domain") || !cb.contains("domain")) {
        unknown.emplace_back("domain");
    } else {
        double worst = 0.0;
        for (const char* side : {"min", "max"})
            for (std::size_t k = 0; k < 3; ++k)
                worst = std::max(worst, std::abs(ca["domain"][side][k].get<double>() - cb["domain"][side][k].get<double>()));
        sameDomain = worst <= 1e-6; // 1 µm: the same domain up to rounding
        if (!sameDomain) {
            diff.emplace_back("domain");
            const double cell = std::max(ca.value("baseCellSize", 0.0), cb.value("baseCellSize", 0.0));
            notes.emplace_back("The flow domains differ by up to " + std::to_string(std::lround(worst * 1000.0)) +
                               " mm (" + (worst <= cell ? "within" : "more than") +
                               " one background cell), because the geometry differs in size or position; forces "
                               "are compared on different domains.");
        }
    }
    j["sameDomain"] = sameDomain;
    const Json engineA = ca.value("engine", Json()), engineB = cb.value("engine", Json());
    if (engineA.is_null() || engineB.is_null())
        unknown.emplace_back("engine");
    else if (engineA != engineB)
        diff.emplace_back("engine");
    for (const auto& u : unknown)
        notes.emplace_back("The " + std::string(u == "context" ? "surroundings (context geometry)" : u) + " of " +
                           ((u == "context" ? ctxA.is_null() && ctxB.is_null()
                             : u == "engine" ? engineA.is_null() && engineB.is_null()
                                             : !ca.contains("domain") && !cb.contains("domain"))
                                ? std::string("both runs")
                                : std::string("one run")) +
                           " was not recorded (older case), so it could not be compared.");
    j["sameConditions"] = diff.empty() && unknown.empty();
    j["differingConditions"] = diff;
    j["unknownConditions"] = unknown;
    // Only a domain difference still allows comparing the forces (a target-part variant).
    const bool blocking = !unknown.empty() || std::any_of(diff.begin(), diff.end(), [](const std::string& c) {
                              return c != "domain";
                          });
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
    const Json sensitivity = finalAvailable && !blocking
                                 ? meshSensitivity(a, b)
                                 : Json{{"status", "not assessed"}, {"levels", Json::array()}};
    j["meshSensitivity"] = sensitivity;
    j["verdict"] = !finalAvailable || blocking ? "not comparable"
                   : !(ra.at("quality").at("status") == "ok" && rb.at("quality").at("status") == "ok")
                       ? "check quality"
                   : sensitivity.at("status") == "check quality" ? "check quality"
                   : sensitivity.at("status") == "inconclusive" ? "difference inconclusive"
                   : !sameDomain                                ? "different domains"
                                                                 : "comparable";

    // Shared colour scales for the field views.
    Json scales = Json::object();
    for (const auto& [kind, scalar] :
         std::vector<std::pair<const char*, const char*>>{{"surface", "Cp"}, {"sectionHorizontal", "speed"}}) {
        double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
        if (finalAvailable) {
            for (const auto& id : {a, b}) {
                const PolyData d = field(id, kind);
                if (auto it = d.scalars.find(scalar); it != d.scalars.end()) {
                    const auto [l, h] = range(it->second);
                    lo = std::min(lo, l);
                    hi = std::max(hi, h);
                }
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
