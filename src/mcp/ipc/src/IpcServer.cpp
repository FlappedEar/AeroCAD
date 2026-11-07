// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/mcp/IpcServer.hpp"

#include "aerocad/mcp/IpcProtocol.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>

namespace aerocad::mcp {

namespace {

constexpr qsizetype kMaxLineBytes = 16 * 1024 * 1024;

/// On Unix a missing socket directory is created so that only the user can enter it (the
/// default ~/.aerocad). Existing directories (e.g. an AEROCAD_MCP_SOCKET override) are left
/// alone; Qt restricts the socket file itself to the user in any case (UserAccessOption).
bool prepareSocketDirectory(const QString& endpoint, QString& error)
{
#if defined(Q_OS_WIN)
    Q_UNUSED(endpoint);
    Q_UNUSED(error);
    return true;
#else
    if (!endpoint.contains(QLatin1Char('/')))
        return true;
    const QString dir = QFileInfo(endpoint).absolutePath();
    if (QFileInfo::exists(dir))
        return true; // never change permissions of a directory we did not create
    if (!QDir().mkpath(dir)) {
        error = QStringLiteral("Cannot create %1").arg(dir);
        return false;
    }
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return true;
#endif
}

} // namespace

IpcServer::IpcServer(Handler handler, QObject* parent) : QObject(parent), m_handler(std::move(handler)) {}

IpcServer::~IpcServer()
{
    close();
}

bool IpcServer::isListening() const
{
    return m_server != nullptr && m_server->isListening();
}

QList<IpcServer::Client> IpcServer::clients() const
{
    QList<Client> out;
    for (const auto& c : m_connections)
        out.push_back(c.info);
    return out;
}

bool IpcServer::listen(const QString& endpoint)
{
    close();
    m_error.clear();
    m_endpoint = endpoint;
    if (!prepareSocketDirectory(endpoint, m_error))
        return false;

    {
        // Another running instance owns the endpoint: do not steal it.
        QLocalSocket probe;
        probe.connectToServer(endpoint);
        if (probe.waitForConnected(200)) {
            probe.disconnectFromServer();
            m_error = QStringLiteral("Another AeroCAD window already accepts AI agents (MCP)");
            return false;
        }
    }
    QLocalServer::removeServer(endpoint); // stale socket of a crashed instance

    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(endpoint)) {
        m_error = m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, &IpcServer::onNewConnection);
    return true;
}

void IpcServer::close(const QString& reason)
{
    if (!reason.isEmpty())
        revokeAll(reason);
    const auto sockets = m_connections.keys();
    m_connections.clear();
    for (QLocalSocket* s : sockets) {
        s->disconnect(this);
        s->abort();
        s->deleteLater();
    }
    if (m_server != nullptr) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
    if (!sockets.isEmpty())
        emit clientsChanged();
}

void IpcServer::revokeAll(const QString& reason)
{
    const auto sockets = m_connections.keys();
    for (QLocalSocket* s : sockets) {
        send(s, {{"event", "revoked"}, {"reason", reason.toStdString()}});
        s->flush();
        s->disconnectFromServer();
    }
}

void IpcServer::onNewConnection()
{
    while (QLocalSocket* socket = m_server->nextPendingConnection()) {
        Connection c;
        c.info.id = m_nextClientId++;
        c.info.name = QStringLiteral("MCP client");
        m_connections.insert(socket, c);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            if (m_connections.remove(socket) > 0)
                emit clientsChanged();
            socket->deleteLater();
        });
        emit clientsChanged();
    }
}

void IpcServer::onReadyRead(QLocalSocket* socket)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end())
        return;
    it->buffer += socket->readAll();
    qsizetype newline = -1;
    while ((newline = it->buffer.indexOf('\n')) >= 0) {
        const QByteArray line = it->buffer.left(newline);
        it->buffer.remove(0, newline + 1);
        handleMessage(socket, line);
        it = m_connections.find(socket); // the handler may have disconnected the client
        if (it == m_connections.end())
            return;
    }
    if (it->buffer.size() > kMaxLineBytes) {
        socket->abort();
    }
}

void IpcServer::handleMessage(QLocalSocket* socket, const QByteArray& line)
{
    if (line.trimmed().isEmpty())
        return;
    const nlohmann::json message =
        nlohmann::json::parse(line.constData(), line.constData() + line.size(), nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
        send(socket, {{"id", nullptr}, {"ok", false}, {"error", "Invalid message"}});
        return;
    }
    const nlohmann::json id = message.value("id", nlohmann::json());

    if (message.contains("hello")) {
        const auto& hello = message["hello"];
        if (hello.is_object() && hello.contains("client") && hello["client"].is_string()) {
            m_connections[socket].info.name = QString::fromStdString(hello["client"].get<std::string>());
            emit clientsChanged();
        }
        send(socket, {{"id", id}, {"ok", true}, {"result", {{"app", "AeroCAD"}, {"protocol", kIpcProtocolVersion}}}});
        return;
    }
    if (!message.contains("command") || !message["command"].is_string()) {
        send(socket, {{"id", id}, {"ok", false}, {"error", "Missing command"}});
        return;
    }
    const nlohmann::json params = message.value("params", nlohmann::json::object());
    const Client client = m_connections.value(socket).info;
    nlohmann::json response = m_handler(client, message["command"].get<std::string>(), params);
    response["id"] = id;
    if (m_connections.contains(socket))
        send(socket, response);
}

void IpcServer::send(QLocalSocket* socket, const nlohmann::json& message)
{
    QByteArray bytes = QByteArray::fromStdString(message.dump());
    bytes.append('\n');
    socket->write(bytes);
}

} // namespace aerocad::mcp
