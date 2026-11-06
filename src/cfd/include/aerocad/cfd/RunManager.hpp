// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/cfd/CaseWriter.hpp"
#include "aerocad/cfd/Process.hpp"
#include "aerocad/cfd/Results.hpp"
#include "aerocad/cfd/Settings.hpp"

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
    std::optional<Backend> recommended;
    std::string message;              ///< Human-readable summary / what to install.

    [[nodiscard]] Json toJson() const;
};

/// Probes Docker and a local OpenFOAM. Blocking (runs `docker info`, at most a few
/// seconds).
[[nodiscard]] Environment detectEnvironment();

enum class RunState { Running, Finished, Failed, Cancelled, Interrupted };
[[nodiscard]] std::string_view runStateName(RunState state);

/// Who and what a run was started for.
struct RunSource {
    std::uint64_t partId = 0;
    std::string partName;
    std::uint64_t partRevision = 0;
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
    /// std::runtime_error (no engine, bad geometry, I/O).
    std::string start(const CaseGeometry& geometry, const Settings& settings, const RunSource& source,
                      std::optional<Backend> backend = std::nullopt);

    /// Only writes the case (no engine), e.g. to run it elsewhere. Returns the run id.
    std::string prepare(const CaseGeometry& geometry, const Settings& settings, const RunSource& source);

    /// Registers a case that was computed outside the app (an AeroCAD case directory that
    /// has case.json). Returns the run id.
    std::string importCase(const std::filesystem::path& caseDir, const std::string& label = {});

    /// Re-reads the run directories (new runs from other processes, deleted runs).
    void rescan();

    [[nodiscard]] std::vector<RunStatus> list();
    [[nodiscard]] std::optional<RunStatus> status(const std::string& id);
    /// Stops a running run. Returns false when it is not running.
    bool cancel(const std::string& id);
    /// Deletes a finished run and its files. Returns false when unknown or still running.
    bool remove(const std::string& id);

    [[nodiscard]] std::optional<CaseInfo> caseInfo(const std::string& id);

    /// Histories, summary in N, quality report. Throws std::runtime_error for unknown ids.
    [[nodiscard]] Json results(const std::string& id, int maxPoints = 400);

    /// Field data for the result view, converted to model coordinates (mm):
    /// kind "surface" (pressure on the part), "sectionHorizontal", "sectionVertical",
    /// "streamlines". Empty PolyData when the run has not written it (yet).
    [[nodiscard]] PolyData field(const std::string& id, std::string_view kind);

    /// A/B comparison: force differences in N, condition check, quality, shared value
    /// ranges for colour scales.
    [[nodiscard]] Json compare(const std::string& a, const std::string& b);

private:
    struct Run;
    Run* find(const std::string& id);
    void loadRun(const std::filesystem::path& dir);
    void refresh(Run& run);
    std::string newRunId(const std::string& partName);
    std::string create(const CaseGeometry& geometry, const Settings& settings, const RunSource& source,
                       std::optional<Backend> backend);

    std::filesystem::path m_root;
    std::map<std::string, std::unique_ptr<Run>> m_runs;
};

} // namespace aerocad::cfd
