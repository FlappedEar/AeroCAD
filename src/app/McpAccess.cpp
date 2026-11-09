// SPDX-License-Identifier: GPL-3.0-or-later
#include "McpAccess.hpp"

#include "AppController.hpp"
#include "JsonBridge.hpp"

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

/// Declared capabilities, not a claim that a failed command performed every effect.
QString describeEffects(const commands::CommandInfo* info)
{
    if (info == nullptr)
        return QObject::tr("Unknown command");
    const auto& e = info->effects;
    QStringList effects;
    if (info->modifiesDocument)
        effects << (e.undoable ? QObject::tr("Changes project (undoable)")
                              : QObject::tr("Replaces project (cannot undo)"));
    if (e.readsFiles)
        effects << QObject::tr("May read files");
    if (e.writesFiles)
        effects << QObject::tr("May write or replace files");
    if (e.deletesFiles)
        effects << QObject::tr("May delete files");
    if (e.runsProcesses)
        effects << QObject::tr("May start or stop programs");
    if (e.network)
        effects << QObject::tr("May use the network");
    if (e.installsSoftware)
        effects << QObject::tr("May install software");
    if (e.writesFiles || e.deletesFiles || e.runsProcesses || e.installsSoftware)
        effects << QObject::tr("External effects cannot be undone");
    return effects.isEmpty() ? QObject::tr("Reads project state") : effects.join(QStringLiteral("; "));
}

} // namespace

McpAccess::McpAccess(AppController& controller, QObject* parent)
    : QObject(parent), m_controller(controller),
      m_server([this](const mcp::IpcServer::Client& client, const std::string& command, const nlohmann::json& params) {
          return handle(client, command, params);
      })
{
    connect(&m_server, &mcp::IpcServer::clientsChanged, this, &McpAccess::clientsChanged);
    connect(&controller, &AppController::projectChanged, this, &McpAccess::projectChanged);
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

QString McpAccess::projectTarget() const
{
    const auto& project = m_controller.session().project();
    return project.path.empty() ? tr("Untitled (session %1)").arg(project.generation + 1)
                                : QDir::toNativeSeparators(QString::fromStdString(project.path));
}

void McpAccess::setEnabled(bool enabled)
{
    projectChanged();
    apply(enabled);
}

void McpAccess::restoreFromSettings()
{
    QSettings().remove(kSettingsKey); // migrate the old blanket grant; consent is never persisted
    apply(false);
}

void McpAccess::projectChanged()
{
    if (m_enabled && m_grantedGeneration != m_controller.session().project().generation)
        apply(false, tr("Project replaced. Allow access again for the new project."));
    if (m_enabled)
        m_grantedProject = projectTarget(); // Save As renames this project; it does not replace it.
    emit stateChanged();
}

void McpAccess::apply(bool enabled, const QString& reason)
{
    const bool changed = m_enabled != enabled;
    m_enabled = enabled;
    m_error.clear();
    if (enabled) {
        m_grantedGeneration = m_controller.session().project().generation;
        m_grantedProject = projectTarget();
        if (!m_server.isListening() && !m_server.listen(QString::fromStdString(mcp::defaultIpcEndpoint())))
            m_error = m_server.errorString();
    } else {
        m_server.close(reason.isEmpty() ? tr("AI agent access was turned off in AeroCAD") : reason);
    }
    if (changed) {
        const QString message = enabled
            ? (m_error.isEmpty() ? tr("Access enabled for this project")
                                 : tr("Access requested; listener failed: %1").arg(m_error))
            : (reason.isEmpty() ? tr("Access turned off") : reason);
        recordAccessEvent(message, m_grantedProject, m_error.isEmpty());
    }
    emit stateChanged();
}

void McpAccess::disconnectClients()
{
    apply(false, tr("Agents disconnected. Allow access again to reconnect to this project."));
}

void McpAccess::recordAccessEvent(const QString& message, const QString& target, bool ok)
{
    m_log.prepend(QVariantMap{
        {QStringLiteral("time"), QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))},
        {QStringLiteral("client"), tr("AeroCAD")}, {QStringLiteral("project"), target},
        {QStringLiteral("effects"), tr("Access control")}, {QStringLiteral("command"), QStringLiteral("access")},
        {QStringLiteral("tool"), tr("Access")}, {QStringLiteral("params"), QString()},
        {QStringLiteral("ok"), ok}, {QStringLiteral("message"), message}, {QStringLiteral("durationMs"), 0},
    });
    while (m_log.size() > kMaxLogEntries)
        m_log.removeLast();
    emit logChanged();
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
    // Check at dispatch as well as on projectChanged: queued transport requests must never
    // act on a replacement project, even if another caller has not emitted its UI signals yet.
    const QString target = m_grantedProject;
    const QString effects = describeEffects(m_controller.session().registry().info(command));
    projectChanged();
    const bool viewCommand = command.rfind("view.", 0) == 0 && m_viewHandler;
    const commands::CommandResult result = !m_enabled ? commands::CommandResult::failure(
                                                            "AI agent access is not granted for this project")
                                           : viewCommand ? m_viewHandler(command, params)
                                                         : m_controller.executeCommand(command, params);
    const qint64 ms = timer.elapsed();

    if (result.ok && command == "document.import" && result.data.contains("id"))
        emit m_controller.partImported(result.data["id"].get<quint64>());

    m_log.prepend(QVariantMap{
        {QStringLiteral("time"), QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))},
        {QStringLiteral("client"), client.name},
        {QStringLiteral("project"), target},
        {QStringLiteral("effects"), effects},
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

QVariantMap McpAccess::runViewCommand(const QString& name, const QVariantMap& params)
{
    const commands::CommandResult r = m_viewHandler
        ? m_viewHandler(name.toStdString(), toJson(QVariant(params)))
        : commands::CommandResult::failure("No view handler");
    return toVariant(r.toJson()).toMap();
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
