// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "PartListModel.hpp"

#include "aerocad/commands/Session.hpp"

#include <QLockFile>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QUrl>
#include <QVariantMap>
#include <QVector3D>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace aerocad::app {

/// Bridge between QML and the command layer. Every UI action goes through execute(),
/// i.e. through the same commands the MCP server will expose to an AI agent.
class AppController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by main()")
    Q_PROPERTY(aerocad::app::PartListModel* parts READ parts CONSTANT)
    Q_PROPERTY(QVector3D sceneCenter READ sceneCenter NOTIFY boundsChanged)
    Q_PROPERTY(double sceneRadius READ sceneRadius NOTIFY boundsChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY historyChanged)
    Q_PROPERTY(QString redoText READ redoText NOTIFY historyChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(QStringList importNameFilters READ importNameFilters CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString projectName READ projectName NOTIFY projectChanged)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY projectChanged)
    Q_PROPERTY(bool projectDirty READ projectDirty NOTIFY projectChanged)
    Q_PROPERTY(QStringList recentProjects READ recentProjects NOTIFY recentProjectsChanged)
    Q_PROPERTY(QStringList openNameFilters READ openNameFilters CONSTANT)
    /// Variants of the design: [{id, name, active}], see variant.list.
    Q_PROPERTY(QVariantList variants READ variants NOTIFY projectChanged)
    Q_PROPERTY(QString activeVariantName READ activeVariantName NOTIFY projectChanged)
    /// Autosave interval in seconds (QSettings project/autosaveSeconds, default 120; 0 = off).
    Q_PROPERTY(int autosaveSeconds READ autosaveSeconds WRITE setAutosaveSeconds NOTIFY autosaveSecondsChanged)

public:
    explicit AppController(QObject* parent = nullptr);

    [[nodiscard]] PartListModel* parts() { return &m_parts; }
    /// Centre of all visible parts in scene coordinates (Y-up), see Viewport.qml.
    [[nodiscard]] QVector3D sceneCenter() const;
    /// Radius of the bounding sphere of all visible parts (mm); 0 when empty.
    [[nodiscard]] double sceneRadius() const;
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    [[nodiscard]] QString undoText() const;
    [[nodiscard]] QString redoText() const;
    [[nodiscard]] QString statusMessage() const { return m_statusMessage; }
    [[nodiscard]] QStringList importNameFilters() const;
    [[nodiscard]] QString version() const;
    [[nodiscard]] QString projectName() const;
    [[nodiscard]] QString projectPath() const;
    [[nodiscard]] bool projectDirty() const;
    [[nodiscard]] QStringList recentProjects() const { return m_recent; }
    [[nodiscard]] QStringList openNameFilters() const;
    [[nodiscard]] QVariantList variants() const;
    [[nodiscard]] QString activeVariantName() const;
    [[nodiscard]] int autosaveSeconds() const;
    void setAutosaveSeconds(int seconds);

    /// Runs a command. Returns {ok: bool, result: object} or {ok: false, error: string}.
    Q_INVOKABLE QVariantMap execute(const QString& name, const QVariantMap& params = {});
    /// Convenience wrapper around document.import for file dialogs and drag & drop.
    Q_INVOKABLE bool importFile(const QUrl& url);
    /// Runs a JSON command script: an array of {"name": ..., "params": {...}} objects (the
    /// same calls an MCP client makes). Stops at the first failing command. Returns false
    /// and sets the status message on errors.
    Q_INVOKABLE bool runScript(const QString& path);
    /// Opens a .aerocad project, or imports any other supported 3D file.
    Q_INVOKABLE bool openFile(const QUrl& url);
    /// Opens a local file path (Open Recent).
    Q_INVOKABLE bool openPath(const QString& path) { return openFile(QUrl::fromLocalFile(path)); }
    /// project.new; removes the autosave copy of the closed project.
    Q_INVOKABLE void newProject();
    /// Saves to the current file. Returns false when the project has no file yet (the UI
    /// then asks for one and calls saveProjectAs) or saving failed. `view` = camera state.
    Q_INVOKABLE bool saveProject(const QVariantMap& view = {});
    Q_INVOKABLE bool saveProjectAs(const QUrl& url, const QVariantMap& view = {});
    /// Autosave copies left by a previous session that did not exit cleanly, newest first.
    Q_INVOKABLE QStringList autosaveFiles() const;
    /// Opens an autosave copy as an unsaved project with its original path.
    Q_INVOKABLE bool recoverAutosave(const QString& path);
    /// Deletes all autosave copies (the user declined recovery) or, with `currentOnly`, the
    /// copy of the open project (clean exit without saving).
    Q_INVOKABLE void discardAutosaves(bool currentOnly = false);
    /// Deletes one autosave copy (the user declined to recover it).
    Q_INVOKABLE void discardAutosaveFile(const QString& path) { removeAutosave(path); }
    /// Writes the autosave copy now if there are unsaved changes. Returns true if written.
    Q_INVOKABLE bool autosaveNow();
    Q_INVOKABLE void clearRecentProjects();
    Q_INVOKABLE void setStatus(const QString& message) { setStatusMessage(message); }

    /// Part details for the properties panel ({} for unknown ids).
    Q_INVOKABLE QVariantMap partInfo(quint64 id) const;

    /// Access for C++ views (MeshGeometry).
    [[nodiscard]] const model::Part* part(quint64 id) const;
    [[nodiscard]] commands::Session& session() { return m_session; }

signals:
    void partChanged(quint64 id);
    void boundsChanged();
    void historyChanged();
    void statusMessageChanged();
    /// Emitted after a successful import so the view can frame the new model.
    void partImported(quint64 id);
    /// Emitted after runScript() so the view can frame the result.
    void scriptFinished();
    void projectChanged();
    void recentProjectsChanged();
    void autosaveSecondsChanged();
    /// After a project was opened or recovered; `view` is the stored camera state.
    void projectOpened(const QVariantMap& view);

private:
    void setStatusMessage(const QString& message);
    bool openProject(const QString& path, bool recover);
    bool saveTo(const QString& path, const QVariantMap& view);
    void addRecent(const QString& path);
    /// Deletes an autosave copy unless another running instance holds its lock.
    void removeAutosave(const QString& path);
    /// Autosave file of the open project (per project path, or per process if untitled).
    [[nodiscard]] QString autosavePath() const;
    [[nodiscard]] static QString autosaveDir();

    commands::Session m_session;
    PartListModel m_parts;
    QString m_statusMessage;
    QStringList m_recent;
    QTimer m_autosaveTimer;
    /// Held while this process owns an autosave file, so other instances do not offer it
    /// for recovery.
    std::unique_ptr<QLockFile> m_autosaveLock;
};

} // namespace aerocad::app
