// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/mcp/McpServer.hpp"

#include <QByteArray>
#include <QString>

#include <memory>

class QLocalSocket;

namespace aerocad::mcp {

/// MCP server side of the IPC: forwards commands to the running application. Blocking
/// (the MCP stdio loop handles one request at a time). Connects lazily, so the MCP server
/// can start before the application. Each connection binds to the application's current
/// explicit grant. A later call can reconnect after fresh consent, without replaying a call
/// interrupted by revocation or restarting the MCP server.
class IpcClient : public Backend {
public:
    IpcClient(QString endpoint, QString clientName);
    ~IpcClient() override;

    commands::CommandResult execute(const std::string& command, const Json& params) override;

    /// Updates the name sent to the application (the MCP client's name is only known after
    /// initialize). Takes effect on the next connection.
    void setClientName(const QString& name) { m_clientName = name; }

    /// True after observed revocation, until a fresh connection receives a new grant.
    [[nodiscard]] bool isRevoked() const { return m_revoked; }

    /// Milliseconds to wait for a command result (imports of large files can take a while).
    void setTimeout(int ms) { m_timeoutMs = ms; }

private:
    bool ensureConnected(QString& error);
    std::optional<Json> readMessage(int timeoutMs, QString& error);
    bool handleEvent(const Json& message);
    /// Reads what the app sent while no call was running (e.g. a revoke event).
    void drainPending();
    void disconnect();

    QString m_endpoint;
    QString m_clientName;
    QString m_accessSession; ///< Bound by hello for this connection; sent on every command.
    std::unique_ptr<QLocalSocket> m_socket;
    QByteArray m_buffer;
    qint64 m_nextId = 1;
    bool m_revoked = false;
    QString m_revokeReason;
    int m_timeoutMs = 300000;
};

} // namespace aerocad::mcp
