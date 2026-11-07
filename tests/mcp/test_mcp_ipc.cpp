// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end test of the local IPC: an IpcServer (as in the application) runs commands
// in a Session on its own thread; an IpcClient (as in aerocad-mcp) calls it.
#include "aerocad/mcp/IpcClient.hpp"
#include "aerocad/mcp/IpcServer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>
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
                        return m_session.execute(command, params).toJson();
                    });
                m_listening = m_server->listen(endpoint);
            },
            Qt::BlockingQueuedConnection);
    }
    ~ServerThread()
    {
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

    bool listening() const { return m_listening; }
    int calls() const { return m_calls; }
    QString lastClient() const { return m_lastClient; }

private:
    struct Context : QObject {};
    QThread m_thread;
    Context m_context;
    commands::Session m_session;
    std::unique_ptr<mcp::IpcServer> m_server;
    bool m_listening = false;
    std::atomic<int> m_calls{0};
    QString m_lastClient;
};

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

    // After the user disconnects the agent, the client refuses further calls.
    server.revoke();
    r = client.execute("document.parts", Json::object());
    CHECK_FALSE(r.ok);
    CHECK(client.isRevoked());
    CHECK(r.error.find("disconnected") != std::string::npos);
    CHECK(server.calls() == 2);
}
