// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end test of the local IPC: an IpcServer (as in the application) runs commands
// in a Session on its own thread; an IpcClient (as in aerocad-mcp) calls it.
#include "aerocad/mcp/IpcClient.hpp"
#include "aerocad/mcp/IpcServer.hpp"
#include "aerocad/mcp/IpcProtocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>
#include <QLocalSocket>
#include <QSemaphore>
#include <QRandomGenerator>
#include <QThread>

#include <atomic>
#include <memory>

using namespace aerocad;
using mcp::Json;

namespace {

QCoreApplication& app()
{
    static int argc = 1;
    static char name[] = "test_mcp_ipc";
    static char* argv[] = {name, nullptr};
    static QCoreApplication instance(argc, argv);
    return instance;
}

QString uniqueEndpoint()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("aerocad-mcp-test-%1").arg(QRandomGenerator::global()->generate());
#else
    return QDir::tempPath() +
           QStringLiteral("/acad-%1/mcp.sock").arg(QRandomGenerator::global()->generate());
#endif
}

/// IpcServer + Session living in a worker thread with its own event loop.
class ServerThread {
public:
    explicit ServerThread(const QString& endpoint)
    {
        m_context.moveToThread(&m_thread);
        m_thread.start();
        QMetaObject::invokeMethod(
            &m_context,
            [this, endpoint] {
                m_server = std::make_unique<mcp::IpcServer>(
                    [this](const mcp::IpcServer::Client& client, const std::string& command, const Json& params) {
                        m_lastClient = client.name;
                        ++m_calls;
                        if (command == "test.revoke") {
                            m_entered.release();
                            m_resume.acquire();
                            m_server->revokeAll(QStringLiteral("project replaced"));
                            return commands::CommandResult::success(Json::object()).toJson();
                        }
                        return m_session.execute(command, params).toJson();
                    });
                m_listening = m_server->listen(endpoint);
                m_error = m_server->errorString();
            },
            Qt::BlockingQueuedConnection);
    }
    ~ServerThread()
    {
        m_resume.release(); // do not strand the worker if a test assertion fails
        QMetaObject::invokeMethod(&m_context, [this] { m_server.reset(); }, Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }
    ServerThread(const ServerThread&) = delete;
    ServerThread& operator=(const ServerThread&) = delete;

    void revoke()
    {
        QMetaObject::invokeMethod(
            &m_context, [this] { m_server->revokeAll(QStringLiteral("test")); }, Qt::BlockingQueuedConnection);
    }

    // Simulates a lost revoke notification, e.g. the application crashed. The same client
    // process must still refuse a subsequent application's different access session.
    void restart(const QString& endpoint)
    {
        QMetaObject::invokeMethod(&m_context, [this, endpoint] {
            for (auto* socket : m_server->findChildren<QLocalSocket*>())
                socket->abort();
            m_listening = m_server->listen(endpoint);
            m_error = m_server->errorString();
        }, Qt::BlockingQueuedConnection);
    }
    int clientCount()
    {
        int count = 0;
        QMetaObject::invokeMethod(&m_context, [this, &count] { count = static_cast<int>(m_server->clients().size()); },
                                  Qt::BlockingQueuedConnection);
        return count;
    }
    bool waitForRevokeCommand() { return m_entered.tryAcquire(1, 5000); }
    void resumeRevokeCommand() { m_resume.release(); }

    bool listening() const { return m_listening; }
    int calls() const { return m_calls; }
    QString error() const { return m_error; }
    QString lastClient()
    {
        QString name;
        QMetaObject::invokeMethod(&m_context, [this, &name] { name = m_lastClient; }, Qt::BlockingQueuedConnection);
        return name;
    }

private:
    struct Context : QObject {};
    QThread m_thread;
    Context m_context;
    commands::Session m_session;
    std::unique_ptr<mcp::IpcServer> m_server;
    bool m_listening = false;
    QString m_error;
    std::atomic<int> m_calls{0};
    QString m_lastClient;
    QSemaphore m_entered;
    QSemaphore m_resume;
};

void send(QLocalSocket& socket, const Json& message)
{
    socket.write(QByteArray::fromStdString(message.dump() + "\n"));
    socket.flush();
}

Json receive(QLocalSocket& socket)
{
    while (!socket.canReadLine()) {
        if (!socket.waitForReadyRead(5000))
            return Json();
    }
    return Json::parse(socket.readLine().toStdString());
}

std::string hello(QLocalSocket& socket, const QString& endpoint, const char* name)
{
    socket.connectToServer(endpoint);
    if (!socket.waitForConnected(5000))
        return {};
    send(socket, {{"id", 1}, {"hello", {{"client", name}, {"protocol", mcp::kIpcProtocolVersion}}}});
    const auto reply = receive(socket);
    if (!reply.is_object() || !reply.value("ok", false))
        return {};
    return reply.at("result").at("session").get<std::string>();
}

} // namespace

TEST_CASE("IpcClient reports a clear error when the app is not running", "[mcp][ipc]")
{
    app();
    mcp::IpcClient client(uniqueEndpoint(), QStringLiteral("test"));
    const auto r = client.execute("document.parts", Json::object());
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("not reachable") != std::string::npos);
}

TEST_CASE("Commands round-trip over the local socket", "[mcp][ipc]")
{
    app();
    const QString endpoint = uniqueEndpoint();
    ServerThread server(endpoint);
    INFO(server.error().toStdString());
    REQUIRE(server.listening());

    // A second server on the same endpoint must not steal it.
    {
        mcp::IpcServer second([](const mcp::IpcServer::Client&, const std::string&, const Json&) { return Json(); });
        CHECK_FALSE(second.listen(endpoint));
        CHECK_FALSE(second.errorString().isEmpty());
    }

    mcp::IpcClient client(endpoint, QStringLiteral("Claude test"));
    auto r = client.execute("geometry.measure", {{"kind", "distance"}, {"points", {{0, 0, 0}, {6, 8, 0}}}});
    REQUIRE(r.ok);
    CHECK(r.data.at("distance").get<double>() == 10.0);
    CHECK(server.lastClient() == QStringLiteral("Claude test"));

    r = client.execute("part.remove", {{"id", 42}});
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("No part") != std::string::npos);
    CHECK(server.calls() == 2);

    mcp::IpcClient secondClient(endpoint, QStringLiteral("Second agent"));
    REQUIRE(secondClient.execute("document.parts", Json::object()).ok);
    REQUIRE(server.clientCount() == 2);

    // After the user disconnects both agents, neither can execute another command.
    server.revoke();
    CHECK(server.clientCount() == 0);
    r = client.execute("document.parts", Json::object());
    CHECK_FALSE(r.ok);
    CHECK(client.isRevoked());
    CHECK(r.error.find("disconnected") != std::string::npos);
    r = secondClient.execute("document.parts", Json::object());
    CHECK_FALSE(r.ok);
    CHECK(secondClient.isRevoked());
    CHECK(server.calls() == 3);

    mcp::IpcClient freshClient(endpoint, QStringLiteral("New agent"));
    CHECK(freshClient.execute("document.parts", Json::object()).ok);
    CHECK(server.calls() == 4);
}

TEST_CASE("A client cannot reconnect to a new grant after losing the revoke event", "[mcp][ipc]")
{
    app();
    const QString endpoint = uniqueEndpoint();
    ServerThread server(endpoint);
    INFO(server.error().toStdString());
    REQUIRE(server.listening());
    mcp::IpcClient client(endpoint, QStringLiteral("Old agent"));
    REQUIRE(client.execute("document.parts", Json::object()).ok);
    server.restart(endpoint);
    INFO(server.error().toStdString());
    REQUIRE(server.listening());
    CHECK_FALSE(client.execute("document.parts", Json::object()).ok);
    // The first read may detect the socket closure before Qt updates its state. Retrying
    // must still never send a command under the new grant.
    CHECK_FALSE(client.execute("document.parts", Json::object()).ok);
    CHECK(client.isRevoked());
    CHECK(server.calls() == 1);
    mcp::IpcClient freshClient(endpoint, QStringLiteral("New agent"));
    CHECK(freshClient.execute("document.parts", Json::object()).ok);
    CHECK(server.calls() == 2);
}

TEST_CASE("Revocation drops pipelined commands and another client's queued request", "[mcp][ipc]")
{
    app();
    const QString endpoint = uniqueEndpoint();
    ServerThread server(endpoint);
    INFO(server.error().toStdString());
    REQUIRE(server.listening());
    QLocalSocket first;
    QLocalSocket second;
    const std::string session = hello(first, endpoint, "first");
    REQUIRE_FALSE(session.empty());
    REQUIRE(hello(second, endpoint, "second") == session);
    REQUIRE(server.clientCount() == 2);

    const Json revoke{{"id", 2}, {"session", session}, {"command", "test.revoke"}};
    const Json queued{{"id", 3}, {"session", session}, {"command", "document.parts"}};
    first.write(QByteArray::fromStdString(revoke.dump() + "\n" + queued.dump() + "\n"));
    first.flush();
    REQUIRE(server.waitForRevokeCommand());
    // The UI thread is inside the first command; this second client's request is already
    // on the wire when access is revoked. Neither it nor the first client's next line runs.
    send(second, queued);
    server.resumeRevokeCommand();
    CHECK(receive(first).value("event", "") == "revoked");
    CHECK(receive(second).value("event", "") == "revoked");
    CHECK(server.clientCount() == 0);
    CHECK(server.calls() == 1);

    QLocalSocket fresh;
    const std::string next = hello(fresh, endpoint, "fresh");
    REQUIRE_FALSE(next.empty());
    REQUIRE(next != session);
    send(fresh, {{"id", 4}, {"session", session}, {"command", "document.parts"}});
    CHECK_FALSE(receive(fresh).value("ok", true));
    CHECK(server.calls() == 1);
    send(fresh, {{"id", 5}, {"session", next}, {"command", "document.parts"}});
    CHECK(receive(fresh).value("ok", false));
    CHECK(server.calls() == 2);
}

TEST_CASE("Commands require a compatible handshake and its project session", "[mcp][ipc]")
{
    app();
    const QString endpoint = uniqueEndpoint();
    ServerThread server(endpoint);
    INFO(server.error().toStdString());
    REQUIRE(server.listening());
    QLocalSocket socket;
    socket.connectToServer(endpoint);
    REQUIRE(socket.waitForConnected(5000));
    send(socket, {{"id", 1}, {"command", "document.parts"}});
    CHECK_FALSE(receive(socket).value("ok", true));
    send(socket, {{"id", 2}, {"hello", {{"client", "old protocol"}, {"protocol", 1}}}});
    CHECK_FALSE(receive(socket).value("ok", true));
    send(socket, {{"id", 3}, {"command", "document.parts"}, {"session", "stale"}});
    CHECK_FALSE(receive(socket).value("ok", true));
    CHECK(server.calls() == 0);
    send(socket, {{"id", 4}, {"hello", {{"client", "current"}, {"protocol", mcp::kIpcProtocolVersion}}}});
    const auto reply = receive(socket);
    REQUIRE(reply.value("ok", false));
    const auto session = reply.at("result").at("session");
    send(socket, {{"id", 5}, {"command", "document.parts"}});
    CHECK_FALSE(receive(socket).value("ok", true));
    CHECK(server.calls() == 0);
    send(socket, {{"id", 6}, {"command", "document.parts"}, {"session", session}});
    CHECK(receive(socket).value("ok", false));
    CHECK(server.calls() == 1);
}
