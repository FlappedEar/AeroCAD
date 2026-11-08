// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/cfd/CaseWriter.hpp"
#include "aerocad/cfd/Process.hpp"
#include "aerocad/cfd/Results.hpp"
#include "aerocad/cfd/Settings.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aerocad::cfd {

/// Where OpenFOAM runs.
enum class Backend {
    Docker, ///< The official OpenFOAM image in Docker (macOS, Linux). Default.
    Local,  ///< An OpenFOAM installation on this machine (Linux).
};

[[nodiscard]] std::string_view backendName(Backend backend);
[[nodiscard]] std::optional<Backend> parseBackend(std::string_view name);

/// Result of probing the machine for a usable simulation engine.
struct Environment {
    std::filesystem::path docker;     ///< docker CLI, empty when not found.
    bool dockerRunning = false;       ///< `docker info` succeeded.
    bool imagePresent = false;        ///< kDockerImage already pulled.
    std::filesystem::path openfoamBashrc; ///< Local OpenFOAM etc/bashrc, empty when not found.
    std::string remoteHost;           ///< Remote engine (ssh://user@host), empty = this machine.
    int remoteCpus = 0;               ///< CPUs of the remote Docker host (0 = unknown).
    std::string remoteError;          ///< Output of the failed `docker info` on the remote engine.
    std::optional<Backend> recommended;
    std::string message;              ///< Human-readable summary / what to install.

    [[nodiscard]] Json toJson() const;
};

/// Probes Docker and a local OpenFOAM, or the remote engine when one is set (engineHost()).
/// Blocking (runs `docker info`, at most a few seconds; longer over SSH).
[[nodiscard]] Environment detectEnvironment();
/// The same for a given remote engine ("" = this machine), e.g. to test it before saving.
[[nodiscard]] Environment detectEnvironment(const std::string& remoteHost);

/// Remote engine: Docker on another machine, reached over SSH ("ssh://user@host" or
/// "ssh://user@host:port"; the docker CLI's -H syntax). Cases are copied into the
/// container there, results are copied back while it runs, so this machine only waits.
/// Empty = this machine. $AEROCAD_ENGINE_HOST overrides the saved setting
/// (<default runs root>/engine.json).
[[nodiscard]] std::string engineHost();
/// Saves the remote engine ("" = this machine). Throws std::invalid_argument for a host
/// that is not ssh://[user@]host[:port].
void setEngineHost(const std::string& host);
[[nodiscard]] bool isValidEngineHost(std::string_view host);
/// The docker command line with -H for a remote engine.
[[nodiscard]] std::vector<std::string> dockerCommand(const std::filesystem::path& docker, const std::string& host);

/// The next step of "Install for me", from what is there now:
/// - "pull": Docker runs, the OpenFOAM image is missing: download it (in the background);
/// - "terminal" (macOS): open a Terminal window that starts Colima, installing the docker
///   command line tool and Colima with Homebrew first when they are missing (no Docker
///   Desktop);
/// - "start" (macOS): no Homebrew but Docker Desktop is installed: start it;
/// - "browser": macOS without Homebrew (Homebrew page) or Linux without Docker (install page);
/// - "manual": Linux, Docker installed but not running;
/// - "none": ready (Docker with the image, or a local OpenFOAM).
struct SetupStep {
    std::string action;
    std::vector<std::string> command; ///< Program and arguments to run; empty for "pull" / "none".
    std::string message;              ///< What happens, for the user.
};
enum class Platform { MacOS, Linux, Other };
[[nodiscard]] Platform currentPlatform();
/// What is installed for the setup steps (macOS).
struct SetupTools {
    std::filesystem::path brew;   ///< Homebrew's brew, empty when not installed.
    std::filesystem::path colima; ///< Colima, empty when not installed.
    bool dockerDesktop = false;   ///< /Applications/Docker.app exists.
};
[[nodiscard]] SetupStep planEngineSetup(const Environment& env, Platform platform, const SetupTools& tools);
/// Homebrew's brew, empty when not installed (also looked for where Finder-started apps
/// do not see it: /opt/homebrew/bin, /usr/local/bin).
[[nodiscard]] std::filesystem::path findBrew();
[[nodiscard]] SetupTools findSetupTools();

enum class RunState { Running, Finished, Failed, Cancelled, Interrupted };
[[nodiscard]] std::string_view runStateName(RunState state);

/// Who and what a run was started for.
struct RunSource {
    std::uint64_t partId = 0;
    std::string partName;
    std::uint64_t partRevision = 0;
    std::uint64_t partFingerprint = 0; ///< meshFingerprint of the part; 0 = unknown (imported run).
    std::vector<std::uint64_t> contextPartIds;
    std::string label; ///< Optional user label, e.g. "A" / "B" / "with lip".
};

struct RunStatus {
    std::string id;
    RunState state = RunState::Running;
    std::string stage;    ///< Current Allrun step (features, background, mesh, ..., done).
    double progress = 0.0; ///< 0..1, estimated from the step and the solver iteration.
    int iteration = 0;
    int maxIterations = 0;
    std::string message;  ///< Failure reason or hint.
    std::string created;  ///< ISO 8601 UTC.
    /// While a run started by this app runs: seconds since its engine started, and, while
    /// solving, at most how many seconds are left (the solver may converge sooner).
    std::optional<int> elapsedSeconds, remainingSeconds;
    RunSource source;
    Backend backend = Backend::Docker;
    std::filesystem::path caseDir;
    /// Latest values (empty until the solver writes the first iteration). Forces in N
    /// along the flow (drag), up (lift) and to the side.
    std::optional<double> cd, cl, cs, dragN, liftN, sideN;

    [[nodiscard]] Json toJson() const;
};

/// Owns the simulation runs: creates cases, starts and cancels engine processes and
/// derives every run's state from the files the case writes (so the state also survives
/// an application restart). Not thread-safe; call from one thread. Never blocks for long
/// except detectEnvironment().
///
/// Layout: <root>/<run id>/ is the OpenFOAM case, plus run.json (source, backend,
/// creation time) and log.aerocad (engine output).
class RunManager {
public:
    /// `root`: directory holding the runs. Default: $AEROCAD_CFD_DIR or ~/.aerocad/cfd.
    explicit RunManager(std::filesystem::path root = defaultRoot());
    ~RunManager();
    RunManager(const RunManager&) = delete;
    RunManager& operator=(const RunManager&) = delete;

    static std::filesystem::path defaultRoot();
    [[nodiscard]] const std::filesystem::path& root() const { return m_root; }

    /// Generates the case and starts the engine. Returns the new run id. Throws
    /// std::runtime_error (no engine, bad geometry, I/O). A run on this computer waits
    /// (state running, stage "queued") while starting it would use more solver processes than
    /// the processor limit next to the runs already solving here; it starts by itself when
    /// they finish (list() / status() start it). Oversubscribed MPI runs are many times slower
    /// than the same runs one after the other.
    std::string start(const CaseGeometry& geometry, const Settings& settings, const RunSource& source,
                      std::optional<Backend> backend = std::nullopt);

    /// Solver processes that may run at once on this computer (default: the logical cores).
    void setProcessorLimit(int processes) { m_processorLimit = processes; }
    [[nodiscard]] int processorLimit() const { return m_processorLimit; }

    /// Only writes the case (no engine), e.g. to run it elsewhere. Returns the run id.
    std::string prepare(const CaseGeometry& geometry, const Settings& settings, const RunSource& source);

    /// Registers a case that was computed outside the app (an AeroCAD case directory that
    /// has case.json). Returns the run id.
    std::string importCase(const std::filesystem::path& caseDir, const std::string& label = {});

    /// Re-reads the run directories (new runs from other processes, deleted runs). Only
    /// reads: a remote run found here is not followed (no process is started, no log is
    /// written); the session that started it follows it, or the next session at start-up.
    void rescan();

    [[nodiscard]] std::vector<RunStatus> list();
    [[nodiscard]] std::optional<RunStatus> status(const std::string& id);
    /// Stops a running run. Returns false when it is not running, or when it runs on a
    /// local engine started by an earlier session (it cannot be identified safely).
    bool cancel(const std::string& id);
    /// Deletes a finished run and its files. Returns false when unknown or still running.
    bool remove(const std::string& id);

    [[nodiscard]] std::optional<CaseInfo> caseInfo(const std::string& id);

    /// Histories, summary in N, quality report. Summary values are null until successful
    /// completion with complete finite coefficient/force output; finalResults reports
    /// availability and reasons. Histories remain available, marked provisional when
    /// final results are unavailable. Throws std::runtime_error for unknown ids.
    [[nodiscard]] Json results(const std::string& id, int maxPoints = 400);

    /// Field data for the result view, converted to model coordinates (mm):
    /// kind "surface" (pressure on the part), "sectionHorizontal", "sectionVertical",
    /// a plane of a section stack ("sectionX_3", see sectionStacks()), "streamlines". Empty PolyData when the run has not written it (yet).
    [[nodiscard]] PolyData field(const std::string& id, std::string_view kind);
    /// Pressure difference B - A on A's part surface (CAD-141): dCp, dpressure, dp at A's
    /// points from the nearest point of B's surface within 1 % of A's part size; NaN where
    /// B's part has no surface that close (its shape differs there).
    [[nodiscard]] PolyData surfaceDifference(const std::string& a, const std::string& b);

    /// Downloads the OpenFOAM image in the background (`docker pull`, log in
    /// <root>/image-pull.log). Returns false when a download is already running.
    bool startImagePull(const std::filesystem::path& docker, const std::string& host = {});
    /// State of the last download started by this manager: {state: running | finished |
    /// failed, progress 0..1 (layers done / layers), message}; null when none was started.
    [[nodiscard]] Json imagePullStatus();

    /// A/B comparison: force differences in N, condition check, quality, shared value
    /// ranges for colour scales, and how the difference changes with the mesh (when the
    /// same A and B were also run on another mesh level, see meshSensitivity()).
    /// Both runs must have final results; otherwise no deltas/scales are returned and
    /// the verdict is "not comparable", with states and blocking reasons.
    [[nodiscard]] Json compare(const std::string& a, const std::string& b);

    /// Sensitivity of the A/B force difference to mesh refinement. Looks for finished runs
    /// of the same case as A and as B (same geometry and conditions) on other mesh levels.
    /// With two levels, the difference on the finer one is "inconclusive" when it changed
    /// by at least half of itself between the levels, or changed sign; otherwise it
    /// "holds". Status "not assessed" when only one level exists, "pending" while a run
    /// on another level is still running.
    [[nodiscard]] Json meshSensitivity(const std::string& a, const std::string& b);

    /// Finished or running runs of the same case as `id` (same geometry hash and
    /// conditions) on another mesh level, by level name.
    [[nodiscard]] std::map<std::string, std::vector<RunStatus>> sameCaseOnOtherMeshes(const std::string& id);

private:
    struct Run;
    Run* find(const std::string& id);
    /// `follow`: resume following an unfinished remote run (starts the remote fetch process).
    void loadRun(const std::filesystem::path& dir, bool follow = false);
    void scan(bool follow);
    void refresh(Run& run);
    /// Solver processes of the local runs this manager is running now.
    int localProcessesInUse();
    /// Starts queued local runs, oldest first, while they fit under the processor limit.
    void startQueued();
    std::string newRunId(const std::string& partName);
    std::string create(const CaseGeometry& geometry, const Settings& settings, const RunSource& source,
                       std::optional<Backend> backend);

    std::filesystem::path m_root;
    std::map<std::string, std::unique_ptr<Run>> m_runs;
    std::optional<Process> m_imagePull;
    int m_processorLimit = 0; ///< Set in the constructor.
    std::uint64_t m_queueCounter = 0; ///< Last submission number given to a queued run.
};

} // namespace aerocad::cfd
