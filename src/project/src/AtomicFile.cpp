// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/project/ProjectFile.hpp"

#include "aerocad/core/Path.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace aerocad::project {

namespace fs = std::filesystem;

namespace {

/// Unique name in the target's directory, so the final rename stays on one file system.
fs::path temporaryPath(const fs::path& target)
{
    static std::atomic<unsigned> counter{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path tmp = target;
    tmp += ".tmp-" + std::to_string(ticks) + "-" + std::to_string(counter++);
    return tmp;
}

#ifdef _WIN32

std::string lastError()
{
    return std::system_category().message(static_cast<int>(GetLastError()));
}

void writeAndFlush(const fs::path& path, std::string_view data)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw ProjectError("Cannot create " + core::pathToUtf8(path) + ": " + lastError());
    std::size_t done = 0;
    bool ok = true;
    while (ok && done < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 30));
        DWORD written = 0;
        ok = WriteFile(h, data.data() + done, chunk, &written, nullptr) != 0;
        done += written;
    }
    ok = ok && FlushFileBuffers(h) != 0;
    const std::string error = ok ? std::string() : lastError();
    CloseHandle(h);
    if (!ok)
        throw ProjectError("Cannot write " + core::pathToUtf8(path) + ": " + error);
}

void replaceFile(const fs::path& from, const fs::path& to)
{
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
        throw ProjectError("Cannot replace " + core::pathToUtf8(to) + ": " + lastError());
}

#else

std::string errnoText()
{
    return std::generic_category().message(errno);
}

void writeAndFlush(const fs::path& path, std::string_view data)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        throw ProjectError("Cannot create " + core::pathToUtf8(path) + ": " + errnoText());
    std::size_t done = 0;
    bool ok = true;
    while (ok && done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        ok = n > 0;
        if (ok)
            done += static_cast<std::size_t>(n);
    }
    ok = ok && ::fsync(fd) == 0;
    const std::string error = ok ? std::string() : errnoText();
    ::close(fd);
    if (!ok)
        throw ProjectError("Cannot write " + core::pathToUtf8(path) + ": " + error);
}

void replaceFile(const fs::path& from, const fs::path& to)
{
    if (::rename(from.c_str(), to.c_str()) != 0)
        throw ProjectError("Cannot replace " + core::pathToUtf8(to) + ": " + errnoText());
    // Make the rename itself durable (best effort).
    fs::path dir = to.parent_path();
    if (dir.empty())
        dir = ".";
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (dfd >= 0) {
        (void)::fsync(dfd);
        ::close(dfd);
    }
}

#endif

} // namespace

void writeFileAtomically(const fs::path& path, std::string_view data, const std::function<void()>& beforeRename)
{
    const fs::path tmp = temporaryPath(path);
    try {
        writeAndFlush(tmp, data);
        if (beforeRename)
            beforeRename();
        replaceFile(tmp, path);
    } catch (...) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
}

std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw ProjectError("Cannot open " + core::pathToUtf8(path));
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad())
        throw ProjectError("Cannot read " + core::pathToUtf8(path));
    return data;
}

} // namespace aerocad::project
