// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/mcp/IpcClient.hpp"

#include "aerocad/mcp/IpcProtocol.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalSocket>

namespace aerocad::mcp {

namespace {

constexpr const char* kNotRunning =
    "AeroCAD is not reachable. Ask the user to start AeroCAD, click TOOLS > Agent in its toolbar and turn on "
    "'Allow AI agents to use this project' (or start AeroCAD with --mcp). Then call the tool again.";

} // namespace

IpcClient::IpcClient(QString endpoint, QString clientName)
    : m_endpoint(std::move(endpoint)), m_clientName(std::move(clientName))
{
}

IpcClient::~IpcClient()
{
    disconnect();
}

void IpcClient::disconnect()
{
    if (m_socket) {
        m_socket->abort();
        m_socket.reset();
    }
    m_buffer.clear();
    m_accessSession.clear();
}

bool IpcClient::handleEvent(const Json& message)
{
    if (!message.contains("event"))
        return false;
    if (message["event"] == "revoked") {
        m_revoked = true;
        m_revokeReason = QString::fromStdString(message.value("reason", std::string()));
    }
    return true;
}

std::optional<Json> IpcClient::readMessage(int timeoutMs, QString& error)
{
    QElapsedTimer timer;
    timer.start();
    for (;;) {
        const qsizetype newline = m_buffer.indexOf('\n');
        if (newline >= 0) {
            const QByteArray line = m_buffer.left(newline);
            m_buffer.remove(0, newline + 1);
            Json message = Json::parse(line.constData(), line.constData() + line.size(), nullptr, false);
            if (message.is_discarded() || !message.is_object()) {
                error = QStringLiteral("Invalid reply from AeroCAD");
                return std::nullopt;
            }
            return message;
        }
        if (m_socket->bytesAvailable() > 0) {
            m_buffer += m_socket->readAll();
            continue;
        }
        if (m_socket->state() != QLocalSocket::ConnectedState) {
            error = QStringLiteral("AeroCAD closed the connection");
            return std::nullopt;
        }
        const qint64 left = timeoutMs - timer.elapsed();
        if (left <= 0) {
            error = QStringLiteral("AeroCAD did not answer within %1 s").arg(timeoutMs / 1000);
            return std::nullopt;
        }
        m_socket->waitForReadyRead(static_cast<int>(left));
    }
}

void IpcClient::drainPending()
{
    if (!m_socket)
        return;
    if (m_socket->state() == QLocalSocket::ConnectedState)
        m_socket->waitForReadyRead(0);
    m_buffer += m_socket->readAll();
    qsizetype newline = -1;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        const Json message = Json::parse(line.constData(), line.constData() + line.size(), nullptr, false);
        if (message.is_object())
            handleEvent(message); // anything else is a stale reply
    }
}

bool IpcClient::ensureConnected(QString& error)
{
    if (m_socket && m_socket->state() == QLocalSocket::ConnectedState)
        return true;
    disconnect();
    m_socket = std::make_unique<QLocalSocket>();
    m_socket->connectToServer(m_endpoint);
    if (!m_socket->waitForConnected(2000)) {
        disconnect();
        error = QString::fromUtf8(kNotRunning);
        return false;
    }
    const Json hello{{"id", m_nextId++},
                     {"hello",
                      {{"client", m_clientName.toStdString()},
                       {"pid", QCoreApplication::applicationPid()},
                       {"protocol", kIpcProtocolVersion},
                       {"session", ""}}}};
    m_socket->write(QByteArray::fromStdString(hello.dump() + "\n"));
    m_socket->waitForBytesWritten(2000);
    for (;;) {
        auto reply = readMessage(5000, error);
        if (!reply) {
            disconnect();
            return false;
        }
        if (handleEvent(*reply)) {
            if (m_revoked)
                break;
            continue;
        }
        if (!reply->value("ok", false)) {
            error = QString::fromStdString(reply->value("error", std::string("AeroCAD refused the connection")));
            disconnect();
            return false;
        }
        const Json result = reply->value("result", Json::object());
        if (!result.is_object() || !result.contains("protocol") ||
            result["protocol"] != kIpcProtocolVersion || !result.contains("session") ||
            !result["session"].is_string() || result["session"].get<std::string>().empty()) {
            error = QStringLiteral("Incompatible AeroCAD MCP protocol; restart the bundled MCP server");
            disconnect();
            return false;
        }
        // Only an explicitly granted application listens here. A new connection receives
        // that grant through a fresh hello; old requests are never replayed on it.
        m_accessSession = QString::fromStdString(result["session"].get<std::string>());
        m_revoked = false;
        m_revokeReason.clear();
        return true;
    }
    disconnect();
    return false;
}

commands::CommandResult IpcClient::execute(const std::string& command, const Json& params)
{
    auto revoked = [this] {
        return commands::CommandResult::failure(
            "AI agent access is not granted for this project in AeroCAD" +
            (m_revokeReason.isEmpty() ? std::string() : " (" + m_revokeReason.toStdString() + ")") +
            ". Ask the user to turn on 'Allow AI agents to use this project' in TOOLS > Agent. "
            "Then call the tool again; the MCP server does not need to be restarted.");
    };
    drainPending();
    if (m_revoked)
        disconnect();

    QString error;
    if (!ensureConnected(error))
        return m_revoked ? revoked() : commands::CommandResult::failure(error.toStdString());

    const qint64 id = m_nextId++;
    const Json request{{"id", id}, {"session", m_accessSession.toStdString()},
                       {"command", command}, {"params", params}};
    m_socket->write(QByteArray::fromStdString(request.dump() + "\n"));
    if (!m_socket->waitForBytesWritten(5000) && m_socket->bytesToWrite() > 0) {
        disconnect();
        return commands::CommandResult::failure("Could not send the command to AeroCAD");
    }
    for (;;) {
        auto reply = readMessage(m_timeoutMs, error);
        if (!reply) {
            disconnect();
            return m_revoked ? revoked() : commands::CommandResult::failure(error.toStdString());
        }
        if (handleEvent(*reply)) {
            if (m_revoked) {
                disconnect();
                return revoked();
            }
            continue;
        }
        if (reply->value("id", Json()) != Json(id))
            continue; // stale reply
        if (reply->value("ok", false))
            return commands::CommandResult::success(reply->value("result", Json::object()));
        return commands::CommandResult::failure(reply->value("error", std::string("Command failed")));
    }
}

} // namespace aerocad::mcp
