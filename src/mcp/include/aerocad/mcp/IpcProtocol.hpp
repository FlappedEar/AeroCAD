// SPDX-License-Identifier: GPL-3.0-or-later
// Local IPC between the MCP server process (aerocad-mcp) and the running application.
//
// Transport: a local socket (Unix domain socket on macOS/Linux, named pipe on Windows)
// that only the current OS user can open. Messages are JSON objects, one per line (UTF-8,
// '\n' terminated):
//
//   client -> app  {"id": 1, "hello": {"client": "Claude 1.0", "pid": 123}}
//   app -> client  {"id": 1, "ok": true, "result": {"app": "AeroCAD", "version": "0.1.0", "protocol": 1}}
//   client -> app  {"id": 2, "command": "part.scale", "params": {"id": 1, "factor": 2}}
//   app -> client  {"id": 2, "ok": true, "result": {...}}   or   {"id": 2, "ok": false, "error": "..."}
//   app -> client  {"event": "revoked", "reason": "..."}    (the user disconnected the agent)
#pragma once

#include <string>

namespace aerocad::mcp {

inline constexpr int kIpcProtocolVersion = 1;

/// Local socket the application listens on. AEROCAD_MCP_SOCKET overrides it. Default:
/// "$HOME/.aerocad/mcp.sock" on macOS/Linux (in a directory only the user can access),
/// the named pipe "aerocad-mcp-<user>" on Windows.
[[nodiscard]] std::string defaultIpcEndpoint();

} // namespace aerocad::mcp
