// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "PartListModel.hpp"

#include "aerocad/commands/Session.hpp"

#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <QVector3D>
#include <QtQml/qqmlregistration.h>

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

    /// Runs a command. Returns {ok: bool, result: object} or {ok: false, error: string}.
    Q_INVOKABLE QVariantMap execute(const QString& name, const QVariantMap& params = {});
    /// Runs a command as a live preview (feature dialogs): the previous preview is undone
    /// first, the undo/redo history is left as it was, and a failure is not shown in the
    /// status bar. Any other document change ends the preview.
    Q_INVOKABLE QVariantMap preview(const QString& name, const QVariantMap& params = {});
    /// Undoes the preview, if any. Returns true if one was active.
    Q_INVOKABLE bool cancelPreview();
    Q_INVOKABLE bool previewActive() const { return m_session.previewActive(); }
    /// Convenience wrapper around document.import for file dialogs and drag & drop.
    Q_INVOKABLE bool importFile(const QUrl& url);
    /// Runs a JSON command script: an array of {"name": ..., "params": {...}} objects (the
    /// same calls an MCP client makes). Stops at the first failing command. Returns false
    /// and sets the status message on errors.
    Q_INVOKABLE bool runScript(const QString& path);
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

private:
    void setStatusMessage(const QString& message);

    commands::Session m_session;
    PartListModel m_parts;
    QString m_statusMessage;
};

} // namespace aerocad::app
