// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cfd/Process.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace aerocad::cfd {

namespace fs = std::filesystem;

Process::Process(Process&& other) noexcept
    : m_pid(std::exchange(other.m_pid, -1)), m_exitCode(std::exchange(other.m_exitCode, std::nullopt))
{
}

Process& Process::operator=(Process&& other) noexcept
{
    if (this != &other) {
        m_pid = std::exchange(other.m_pid, -1);
        m_exitCode = std::exchange(other.m_exitCode, std::nullopt);
    }
    return *this;
}

Process::~Process() = default;

#ifndef _WIN32

namespace {

std::vector<char*> cArgv(const std::vector<std::string>& argv)
{
    std::vector<char*> out;
    out.reserve(argv.size() + 1);
    for (const auto& a : argv)
        out.push_back(const_cast<char*>(a.c_str()));
    out.push_back(nullptr);
    return out;
}

int decodeStatus(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

} // namespace

Process Process::start(const std::vector<std::string>& argv, const fs::path& workingDir, const fs::path& logFile)
{
    if (argv.empty())
        throw std::runtime_error("No command to start");
    const int logFd = ::open(logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (logFd < 0)
        throw std::runtime_error("Cannot open " + logFile.string() + ": " + std::strerror(errno));
    // Report exec failures through a pipe that closes on a successful exec.
    int errPipe[2];
    if (::pipe(errPipe) != 0) {
        ::close(logFd);
        throw std::runtime_error("pipe() failed");
    }
    ::fcntl(errPipe[1], F_SETFD, FD_CLOEXEC);
    auto args = cArgv(argv);
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(logFd);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        throw std::runtime_error("fork() failed");
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        ::close(errPipe[0]);
        const int devNull = ::open("/dev/null", O_RDONLY);
        if (devNull >= 0)
            ::dup2(devNull, STDIN_FILENO);
        ::dup2(logFd, STDOUT_FILENO);
        ::dup2(logFd, STDERR_FILENO);
        if (::chdir(workingDir.c_str()) == 0)
            ::execvp(args[0], args.data());
        const int err = errno;
        (void)!::write(errPipe[1], &err, sizeof err);
        ::_exit(127);
    }
    ::setpgid(pid, pid); // Also from the parent, so terminate() works right away.
    ::close(logFd);
    ::close(errPipe[1]);
    int childErr = 0;
    const auto n = ::read(errPipe[0], &childErr, sizeof childErr);
    ::close(errPipe[0]);
    if (n == static_cast<ssize_t>(sizeof childErr)) {
        ::waitpid(pid, nullptr, 0);
        throw std::runtime_error("Cannot start " + argv[0] + ": " + std::strerror(childErr));
    }
    Process p;
    p.m_pid = pid;
    return p;
}

bool Process::running()
{
    if (m_pid <= 0 || m_exitCode)
        return false;
    int status = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(m_pid), &status, WNOHANG);
    if (r == 0)
        return true;
    m_exitCode = r == static_cast<pid_t>(m_pid) ? decodeStatus(status) : -1;
    return false;
}

void Process::terminate(bool force)
{
    if (m_pid > 0 && !m_exitCode)
        ::kill(-static_cast<pid_t>(m_pid), force ? SIGKILL : SIGTERM);
}

ProcessOutput runCommand(const std::vector<std::string>& argv, std::chrono::milliseconds timeout)
{
    ProcessOutput result;
    if (argv.empty())
        return result;
    int out[2];
    if (::pipe(out) != 0)
        return result;
    auto args = cArgv(argv);
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(out[0]);
        ::close(out[1]);
        return result;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        ::close(out[0]);
        const int devNull = ::open("/dev/null", O_RDONLY);
        if (devNull >= 0)
            ::dup2(devNull, STDIN_FILENO);
        ::dup2(out[1], STDOUT_FILENO);
        ::dup2(out[1], STDERR_FILENO);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    ::close(out[1]);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buffer[4096];
    bool open = true;
    while (open) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            result.timedOut = true;
            ::kill(-pid, SIGKILL);
            break;
        }
        pollfd fd{out[0], POLLIN, 0};
        const int ready = ::poll(&fd, 1, static_cast<int>(left.count()));
        if (ready < 0 && errno != EINTR)
            break;
        if (ready > 0) {
            const auto n = ::read(out[0], buffer, sizeof buffer);
            if (n <= 0)
                open = false;
            else if (result.output.size() < (1U << 20))
                result.output.append(buffer, static_cast<std::size_t>(n));
        }
    }
    ::close(out[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!result.timedOut)
        result.exitCode = decodeStatus(status);
    return result;
}

#else // _WIN32: not built in CI yet; the simulation engine needs a POSIX host for now.

Process Process::start(const std::vector<std::string>&, const fs::path&, const fs::path&)
{
    throw std::runtime_error("Running simulations is not supported on Windows yet");
}

bool Process::running() { return false; }
void Process::terminate(bool) {}

ProcessOutput runCommand(const std::vector<std::string>&, std::chrono::milliseconds) { return {}; }

#endif

fs::path findExecutable(const std::string& name, const std::vector<fs::path>& candidates)
{
    std::error_code ec;
    auto usable = [&](const fs::path& p) {
        return fs::is_regular_file(p, ec)
#ifndef _WIN32
               && ::access(p.c_str(), X_OK) == 0
#endif
            ;
    };
    if (name.find('/') != std::string::npos)
        return usable(name) ? fs::path(name) : fs::path();
    if (const char* path = std::getenv("PATH")) {
        std::string_view rest(path);
        while (!rest.empty()) {
            const auto colon = rest.find(':');
            const std::string_view dir = rest.substr(0, colon);
            if (!dir.empty()) {
                const fs::path candidate = fs::path(std::string(dir)) / name;
                if (usable(candidate))
                    return candidate;
            }
            if (colon == std::string_view::npos)
                break;
            rest.remove_prefix(colon + 1);
        }
    }
    for (const auto& c : candidates)
        if (usable(c))
            return c;
    return {};
}

} // namespace aerocad::cfd
