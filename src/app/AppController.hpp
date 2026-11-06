// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "PartListModel.hpp"

#include "aerocad/commands/Session.hpp"
#include "aerocad/core/MeshSelection.hpp"

#include <QColor>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <QVector3D>
#include <QVector>

#include <QtQml/qqmlregistration.h>
#include <optional>

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
    Q_PROPERTY(QStringList exportNameFilters READ exportNameFilters CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    /// Changes whenever an Inspect analysis (section, heat map) changes; bind to it to
    /// re-evaluate inspectInfo() / sectionCuts().
    Q_PROPERTY(int inspectRevision READ inspectRevision NOTIFY inspectChanged)

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
    /// Save dialog filters, one per format, each "<label> (*.ext)".
    [[nodiscard]] QStringList exportNameFilters() const;
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
    /// Convenience wrapper around document.export for the save dialog. `ids` empty means
    /// all visible parts. Returns {ok, error?, warnings: [string]}; the status bar shows
    /// the outcome.
    Q_INVOKABLE QVariantMap exportFile(const QUrl& url, const QVariantList& ids = {});
    /// Runs a JSON command script: an array of {"name": ..., "params": {...}} objects (the
    /// same calls an MCP client makes). Stops at the first failing command. Returns false
    /// and sets the status message on errors.
    Q_INVOKABLE bool runScript(const QString& path);
    /// Part details for the properties panel ({} for unknown ids).
    Q_INVOKABLE QVariantMap partInfo(quint64 id) const;

    /// Nearest hit of a ray (model coordinates) with the visible parts' meshes:
    /// {partId, point (QVector3D), triangle}, or {} when nothing is hit.
    Q_INVOKABLE QVariantMap pickSurface(const QVector3D& origin, const QVector3D& direction) const;

    /// Shows a message in the status bar.
    Q_INVOKABLE void showStatus(const QString& message) { setStatusMessage(message); }

    /// Brush stroke with live preview. brushBegin starts a stroke on a part, brushAdd adds
    /// a dab at a point in model coordinates (mm), brushEnd runs selection.brush with all
    /// points as one undoable command. mode: "replace", "add" or "subtract".
    Q_INVOKABLE bool brushBegin(quint64 id, double radius, bool connected, const QString& mode);
    Q_INVOKABLE void brushAdd(const QVector3D& point);
    Q_INVOKABLE QVariantMap brushEnd();
    Q_INVOKABLE void brushCancel();
    [[nodiscard]] Q_INVOKABLE bool brushActive() const { return m_brush.has_value(); }

    /// Selection to draw for a part: the brush preview during a stroke, else the part's
    /// selection. nullptr when there is none.
    [[nodiscard]] const core::TriangleSet* displayedSelection(quint64 id) const;

    [[nodiscard]] int inspectRevision() const;
    /// Active analyses for QML: {section: {enabled, origin, normal, cap}, heatMap: {enabled,
    /// kind, part, reference, rangeMin, rangeMax, stats}}.
    Q_INVOKABLE QVariantMap inspectInfo() const;
    /// True when the section analysis cuts this part.
    Q_INVOKABLE bool sectionCuts(quint64 id) const;
    /// True when the heat map is shown on this part (and still matches its mesh).
    Q_INVOKABLE bool showsHeatMap(quint64 id) const;
    /// Heat map colour for t in [0, 1], for the legend.
    Q_INVOKABLE QColor heatColor(double t) const;

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
    /// The displayed selection of a part changed without a document change (brush preview).
    void selectionPreviewChanged(quint64 id);
    /// Emitted after runScript() so the view can frame the result.
    void scriptFinished();
    void inspectChanged();

private:
    void setStatusMessage(const QString& message);
    /// Runs the heat map again after its parts changed (or clears it when they are gone).
    void refreshHeatMap();

    bool m_heatMapRefreshQueued = false;

    struct BrushStroke {
        quint64 partId = 0;
        double radius = 1.0;
        bool connected = true;
        QString mode;
        std::shared_ptr<const core::TriangleMesh> mesh;
        std::shared_ptr<const core::MeshAdjacency> adjacency;
        core::TriangleSet base;    ///< Part selection when the stroke started.
        core::TriangleSet stroke;  ///< Union of all dabs.
        core::TriangleSet preview; ///< base combined with stroke.
        QVector<QVector3D> points;
    };

    commands::Session m_session;
    std::optional<BrushStroke> m_brush;
    PartListModel m_parts;
    QString m_statusMessage;
};

} // namespace aerocad::app
