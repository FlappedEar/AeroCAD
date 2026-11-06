// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace aerocad::cfd {

/// A child process in its own process group with stdout and stderr redirected to a file.
/// Non-blocking: poll() reaps it. POSIX only for now (macOS, Linux).
class Process {
public:
    Process() = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;
    /// Does not stop the process: a simulation keeps running when the app closes.
    ~Process();

    /// Starts argv[0] (searched in PATH when it has no slash). Throws std::runtime_error.
    static Process start(const std::vector<std::string>& argv, const std::filesystem::path& workingDir,
                         const std::filesystem::path& logFile);

    [[nodiscard]] bool started() const { return m_pid > 0; }
    /// True while the process runs. Reaps it when it has exited.
    bool running();
    /// Exit code once finished (128 + signal when killed).
    [[nodiscard]] std::optional<int> exitCode() const { return m_exitCode; }
    /// Sends SIGTERM to the whole process group (SIGKILL when `force`).
    void terminate(bool force = false);

private:
    long m_pid = -1;
    std::optional<int> m_exitCode;
};

struct ProcessOutput {
    int exitCode = -1; ///< -1 when it could not be started or timed out.
    std::string output; ///< stdout and stderr.
    bool timedOut = false;
};

/// Runs a short command synchronously and captures its output (used for environment
/// checks such as `docker info`). Kills it after `timeout`.
ProcessOutput runCommand(const std::vector<std::string>& argv,
                         std::chrono::milliseconds timeout = std::chrono::seconds(15));

/// Finds an executable: `name` in PATH, then the extra candidate paths (in order).
/// Returns an empty path when not found.
std::filesystem::path findExecutable(const std::string& name, const std::vector<std::filesystem::path>& candidates = {});

} // namespace aerocad::cfd
