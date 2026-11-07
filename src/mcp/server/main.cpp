// SPDX-License-Identifier: GPL-3.0-or-later
// aerocad-mcp: the MCP server bundled with AeroCAD. An MCP client (Claude Desktop, Claude
// Code, ...) starts it as a child process and talks MCP over stdio (newline-delimited
// JSON-RPC). Tool calls are forwarded to the running AeroCAD application over local IPC;
// with --headless they run in an in-process document instead (no GUI, for scripting and
// tests). stdout carries only protocol messages; diagnostics go to stderr.
#include "aerocad/BuildInfo.hpp"
#include "aerocad/mcp/IpcClient.hpp"
#include "aerocad/mcp/IpcProtocol.hpp"
#include "aerocad/mcp/McpServer.hpp"

#include <QCommandLineParser>
#include <QCoreApplication>

#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("aerocad-mcp"));
    QCoreApplication::setApplicationVersion(QString::fromUtf8(aerocad::buildStamp()));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("AeroCAD MCP server. Start it from an MCP client (stdio transport); it forwards tool calls "
                       "to the running AeroCAD application."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption headless(QStringLiteral("headless"),
                                QStringLiteral("Run commands in an in-process document instead of the AeroCAD app."));
    parser.addOption(headless);
    QCommandLineOption socket(QStringLiteral("socket"),
                              QStringLiteral("Local socket of the AeroCAD app (default: %1).")
                                  .arg(QString::fromStdString(aerocad::mcp::defaultIpcEndpoint())),
                              QStringLiteral("path"));
    parser.addOption(socket);
    parser.process(app);

    std::ios::sync_with_stdio(false);

    // The server's own command registry provides the tool list, so tools/list works even
    // before the application is started. It is the same code as the application's.
    aerocad::commands::Session localSession;
    std::unique_ptr<aerocad::mcp::Backend> backend;
    aerocad::mcp::IpcClient* ipc = nullptr;
    if (parser.isSet(headless)) {
        backend = std::make_unique<aerocad::mcp::SessionBackend>(localSession);
        std::fprintf(stderr, "aerocad-mcp: headless mode (in-process document)\n");
    } else {
        const QString endpoint =
            parser.isSet(socket) ? parser.value(socket) : QString::fromStdString(aerocad::mcp::defaultIpcEndpoint());
        auto client = std::make_unique<aerocad::mcp::IpcClient>(
            endpoint, QStringLiteral("MCP client (aerocad-mcp %1)").arg(QString::fromUtf8(aerocad::buildStamp())));
        ipc = client.get();
        backend = std::move(client);
        std::fprintf(stderr, "aerocad-mcp: forwarding tool calls to AeroCAD at %s\n", qPrintable(endpoint));
    }

    aerocad::mcp::McpServer server(localSession.registry(), *backend,
                                   {"aerocad", "AeroCAD", std::string(aerocad::buildStamp())});
    std::string line;
    while (std::getline(std::cin, line)) {
        std::optional<std::string> response;
        try {
            response = server.handleLine(line);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "aerocad-mcp: %s\n", e.what());
            continue;
        }
        if (ipc != nullptr && !server.clientName().empty())
            ipc->setClientName(QStringLiteral("%1 (aerocad-mcp %2)")
                                   .arg(QString::fromStdString(server.clientName()), QString::fromUtf8(aerocad::buildStamp())));
        if (response) {
            std::cout << *response << '\n';
            std::cout.flush();
        }
    }
    return 0;
}
