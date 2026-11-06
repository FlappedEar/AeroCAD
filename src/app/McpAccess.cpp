// SPDX-License-Identifier: GPL-3.0-or-later
#include "McpAccess.hpp"

#include "AppController.hpp"

#include "aerocad/mcp/IpcProtocol.hpp"
#include "aerocad/mcp/McpServer.hpp"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QSettings>

namespace aerocad::app {

namespace {

constexpr int kMaxLogEntries = 500;
constexpr qsizetype kMaxParamsChars = 300;
const QString kSettingsKey = QStringLiteral("mcp/enabled");

QString jsonString(const nlohmann::json& value)
{
    return QString::fromStdString(value.dump());
}

QString elided(QString text, qsizetype max)
{
    if (text.size() > max)
        text = text.left(max - 1) + QChar(0x2026);
    return text;
}

/// Short human-readable outcome of a command for the log.
QString summarize(const commands::CommandResult& r)
{
    if (!r.ok)
        return QString::fromStdString(r.error);
    if (r.data.contains("name") && r.data["name"].is_string())
        return QString::fromStdString(r.data["name"].get<std::string>());
    if (r.data.contains("distance") && r.data["distance"].is_number())
        return QStringLiteral("%1 mm").arg(r.data["distance"].get<double>(), 0, 'f', 3);
    if (r.data.contains("parts") && r.data["parts"].is_array())
        return QObject::tr("%n part(s)", nullptr, static_cast<int>(r.data["parts"].size()));
    return QObject::tr("OK");
}

} // namespace

McpAccess::McpAccess(AppController& controller, QObject* parent)
    : QObject(parent), m_controller(controller),
      m_server([this](const mcp::IpcServer::Client& client, const std::string& command, const nlohmann::json& params) {
          return handle(client, command, params);
      })
{
    connect(&m_server, &mcp::IpcServer::clientsChanged, this, &McpAccess::clientsChanged);
}

QString McpAccess::endpoint() const
{
    return QDir::toNativeSeparators(QString::fromStdString(mcp::defaultIpcEndpoint()));
}

QStringList McpAccess::clients() const
{
    QStringList out;
    for (const auto& c : m_server.clients())
        out << c.name;
    return out;
}

void McpAccess::setEnabled(bool enabled)
{
    QSettings().setValue(kSettingsKey, enabled);
    apply(enabled);
}

void McpAccess::restoreFromSettings()
{
    apply(QSettings().value(kSettingsKey, false).toBool());
}

void McpAccess::apply(bool enabled)
{
    m_enabled = enabled;
    m_error.clear();
    if (enabled) {
        if (!m_server.isListening() && !m_server.listen(QString::fromStdString(mcp::defaultIpcEndpoint())))
            m_error = m_server.errorString();
    } else {
        m_server.close(tr("AI agent access was turned off in AeroCAD"));
    }
    emit stateChanged();
}

void McpAccess::disconnectClients()
{
    m_server.revokeAll(tr("disconnected by the user"));
}

void McpAccess::clearLog()
{
    m_log.clear();
    emit logChanged();
}

void McpAccess::copyToClipboard(const QString& text) const
{
    if (QClipboard* clipboard = QGuiApplication::clipboard())
        clipboard->setText(text);
}

nlohmann::json McpAccess::handle(const mcp::IpcServer::Client& client, const std::string& command,
                                 const nlohmann::json& params)
{
    QElapsedTimer timer;
    timer.start();
    const commands::CommandResult result = m_controller.executeCommand(command, params);
    const qint64 ms = timer.elapsed();

    if (result.ok && command == "document.import" && result.data.contains("id"))
        emit m_controller.partImported(result.data["id"].get<quint64>());

    m_log.prepend(QVariantMap{
        {QStringLiteral("time"), QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))},
        {QStringLiteral("client"), client.name},
        {QStringLiteral("command"), QString::fromStdString(command)},
        {QStringLiteral("tool"), QString::fromStdString(mcp::toolNameForCommand(command))},
        {QStringLiteral("params"), elided(jsonString(params), kMaxParamsChars)},
        {QStringLiteral("ok"), result.ok},
        {QStringLiteral("message"), elided(summarize(result), kMaxParamsChars)},
        {QStringLiteral("durationMs"), ms},
    });
    while (m_log.size() > kMaxLogEntries)
        m_log.removeLast();
    ++m_operationCount;
    emit logChanged();
    return result.toJson();
}

QString McpAccess::serverPath() const
{
    const QString dir = QCoreApplication::applicationDirPath();
#if defined(Q_OS_WIN)
    const QString exe = QStringLiteral("aerocad-mcp.exe");
#else
    const QString exe = QStringLiteral("aerocad-mcp");
#endif
    // Next to the executable (Linux, Windows, inside the macOS bundle) or next to the
    // bundle in a build tree.
    const QStringList candidates{dir + QLatin1Char('/') + exe, dir + QStringLiteral("/../../../") + exe};
    for (const QString& c : candidates) {
        if (QFileInfo::exists(c))
            return QDir::toNativeSeparators(QFileInfo(c).canonicalFilePath());
    }
    return QDir::toNativeSeparators(candidates.first());
}

QString McpAccess::claudeDesktopConfig() const
{
    const nlohmann::json config{{"mcpServers", {{"aerocad", {{"command", serverPath().toStdString()}}}}}};
    return QString::fromStdString(config.dump(2));
}

QString McpAccess::claudeCodeCommand() const
{
    return QStringLiteral("claude mcp add --scope user aerocad -- \"%1\"").arg(serverPath());
}

} // namespace aerocad::app
