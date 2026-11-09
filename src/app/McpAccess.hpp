// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/mcp/IpcServer.hpp"

#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <cstdint>

namespace aerocad::app {

class AppController;

/// AI agent access to the open project (MCP): the user turns it on or off, sees every
/// operation an agent performs and can disconnect agents. When enabled, the application
/// listens on a local socket (current user only) for the bundled aerocad-mcp server, which
/// an MCP client such as Claude starts. Agent commands run on the UI thread through
/// AppController, exactly like UI actions.
class McpAccess : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by AppController")
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY stateChanged)
    Q_PROPERTY(bool listening READ listening NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY stateChanged)
    Q_PROPERTY(QString endpoint READ endpoint CONSTANT)
    Q_PROPERTY(QString projectTarget READ projectTarget NOTIFY stateChanged)
    Q_PROPERTY(QStringList clients READ clients NOTIFY clientsChanged)
    /// Newest first: {time, client, project, effects, command, tool, params, ok, message, durationMs}.
    Q_PROPERTY(QVariantList log READ log NOTIFY logChanged)
    Q_PROPERTY(int operationCount READ operationCount NOTIFY logChanged)
    Q_PROPERTY(QString serverPath READ serverPath CONSTANT)
    Q_PROPERTY(QString claudeDesktopConfig READ claudeDesktopConfig CONSTANT)
    Q_PROPERTY(QString claudeCodeCommand READ claudeCodeCommand CONSTANT)

public:
    explicit McpAccess(AppController& controller, QObject* parent = nullptr);

    [[nodiscard]] bool enabled() const { return m_enabled; }
    /// Grants access only to this open project generation, until replacement or app exit.
    void setEnabled(bool enabled);
    /// Clears the legacy global preference and starts with no grant (called at startup).
    void restoreFromSettings();

    [[nodiscard]] bool listening() const { return m_server.isListening(); }
    [[nodiscard]] QString errorString() const { return m_error; }
    [[nodiscard]] QString endpoint() const;
    [[nodiscard]] QString projectTarget() const;
    [[nodiscard]] QStringList clients() const;
    [[nodiscard]] QVariantList log() const { return m_log; }
    [[nodiscard]] int operationCount() const { return m_operationCount; }
    [[nodiscard]] QString serverPath() const;
    [[nodiscard]] QString claudeDesktopConfig() const;
    [[nodiscard]] QString claudeCodeCommand() const;

    /// Disconnects all agents and ends consent until the user enables access again.
    Q_INVOKABLE void disconnectClients();
    Q_INVOKABLE void clearLog();
    Q_INVOKABLE void copyToClipboard(const QString& text) const;

signals:
    void stateChanged();
    void clientsChanged();
    void logChanged();

private:
    nlohmann::json handle(const mcp::IpcServer::Client& client, const std::string& command,
                          const nlohmann::json& params);
    void apply(bool enabled, const QString& reason = {});
    void projectChanged();
    void recordAccessEvent(const QString& message, const QString& target, bool ok = true);

    AppController& m_controller;
    mcp::IpcServer m_server;
    bool m_enabled = false;
    std::uint64_t m_grantedGeneration = 0;
    QString m_grantedProject;
    QString m_error;
    QVariantList m_log;
    int m_operationCount = 0;
};

} // namespace aerocad::app
