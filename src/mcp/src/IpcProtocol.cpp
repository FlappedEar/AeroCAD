// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/mcp/IpcProtocol.hpp"

#include <cstdlib>

namespace aerocad::mcp {

namespace {

std::string env(const char* name)
{
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
        return {};
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

} // namespace

std::string defaultIpcEndpoint()
{
    if (auto overridden = env("AEROCAD_MCP_SOCKET"); !overridden.empty())
        return overridden;
#if defined(_WIN32)
    std::string user = env("USERNAME");
    return "aerocad-mcp-" + (user.empty() ? std::string("user") : user);
#else
    std::string home = env("HOME");
    if (home.empty())
        home = "/tmp";
    return home + "/.aerocad/mcp.sock";
#endif
}

} // namespace aerocad::mcp
