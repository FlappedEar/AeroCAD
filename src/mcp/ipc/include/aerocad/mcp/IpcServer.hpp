// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <nlohmann/json.hpp>

#include <QHash>
#include <QObject>
#include <QString>

#include <functional>

class QLocalServer;
class QLocalSocket;

namespace aerocad::mcp {

/// Application side of the MCP IPC (see IpcProtocol.hpp). Listens on a local socket
/// that only the current user can open and runs every received command through the
/// handler, on the thread this object lives in (the UI thread in the application).
class IpcServer : public QObject {
    Q_OBJECT

public:
    struct Client {
        quint64 id = 0;
        QString name; ///< From the hello message, e.g. "Claude 1.0 (aerocad-mcp 0.1.0)".
    };
    /// Runs a command and returns {"ok": ..., "result" | "error": ...}.
    using Handler =
        std::function<nlohmann::json(const Client&, const std::string& command, const nlohmann::json& params)>;

    explicit IpcServer(Handler handler, QObject* parent = nullptr);
    ~IpcServer() override;

    /// Starts listening. Fails (returns false, see errorString()) when another instance is
    /// already serving the endpoint. A stale socket file left by a crashed instance is removed.
    bool listen(const QString& endpoint);
    /// Stops listening and disconnects all clients with the given reason.
    void close(const QString& reason = {});
    /// Immediately drops all queued commands, rotates the access session, sends
    /// {"event": "revoked"} to all clients and disconnects them. Does not stop listening;
    /// use close() to end consent until the user explicitly grants access again.
    void revokeAll(const QString& reason);

    [[nodiscard]] bool isListening() const;
    [[nodiscard]] QString endpoint() const { return m_endpoint; }
    [[nodiscard]] QString errorString() const { return m_error; }
    [[nodiscard]] QList<Client> clients() const;

signals:
    void clientsChanged();

private:
    struct Connection {
        Client info;
        QByteArray buffer;
        bool ready = false;
    };

    void onNewConnection();
    void onReadyRead(QLocalSocket* socket);
    void handleMessage(QLocalSocket* socket, const QByteArray& line);
    void send(QLocalSocket* socket, const nlohmann::json& message);

    Handler m_handler;
    QLocalServer* m_server = nullptr;
    QHash<QLocalSocket*, Connection> m_connections;
    QString m_endpoint;
    QString m_error;
    QString m_accessSession; ///< Changes when access ends, including an explicit Disconnect.
    quint64 m_nextClientId = 1;
};

} // namespace aerocad::mcp
