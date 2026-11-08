// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "McpAccess.hpp"
#include "PartListModel.hpp"

#include "aerocad/commands/Session.hpp"
#include "aerocad/core/MeshSelection.hpp"

#include <QColor>
#include <QLockFile>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QVector3D>
#include <QVector>

#include <QtQml/qqmlregistration.h>
#include <optional>

#include <memory>

namespace aerocad::app {

/// Bridge between QML and the command layer. Every UI action goes through execute(),
/// i.e. through the same commands the MCP server will expose to an AI agent.
class AppController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by main()")
    Q_PROPERTY(aerocad::app::PartListModel* parts READ parts CONSTANT)
    Q_PROPERTY(aerocad::app::McpAccess* mcp READ mcp CONSTANT)
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
    /// AI agent (MCP) access to this project.
    [[nodiscard]] McpAccess* mcp() { return &m_mcp; }
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
    /// Same as execute() for C++ callers such as the MCP access (AI agent): JSON in and out,
    /// with the same UI notifications (history, status, framing of imported parts).
    commands::CommandResult executeCommand(const std::string& name, const commands::Json& params);
    /// Runs a command as a live preview (feature dialogs): the previous preview is undone
    /// first, the undo/redo history is left as it was, and a failure is not shown in the
    /// status bar. Any other document change ends the preview.
    Q_INVOKABLE QVariantMap preview(const QString& name, const QVariantMap& params = {});
    /// Undoes the preview, if any. Returns true if one was active.
    Q_INVOKABLE bool cancelPreview();
    /// Token of the last undo step (UndoStack::topId), 0 when there is none. A dialog keeps
    /// the tokens of the steps it made so it never undoes or joins anyone else's.
    Q_INVOKABLE double historyTopId() const;
    /// Whether the step with token `id` is still applied.
    Q_INVOKABLE bool historyApplied(double id) const;
    /// Whether the step with token `id` left the history while applied (UndoStack::isExpired).
    Q_INVOKABLE bool historyExpired(double id) const;
    /// Undoes the last step only if its token is `id`. Returns true if it did.
    Q_INVOKABLE bool undoIfTop(double id);
    /// Joins the steps `ids` (oldest first) into one called `label`, only if they are exactly
    /// the last applied steps (see UndoStack::mergeIds).
    Q_INVOKABLE bool mergeHistoryIds(const QVariantList& ids, const QString& label);
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
    /// True if the open project's recovery copy is on disk.
    Q_INVOKABLE bool hasAutosave() const;
    Q_INVOKABLE void clearRecentProjects();
    Q_INVOKABLE void setStatus(const QString& message) { setStatusMessage(message); }

    /// Colour map stops (low to high) used for a result scalar, for legends.
    Q_INVOKABLE QVariantList colormap(const QString& scalar) const;
    /// Where `value` sits on a result colour scale [lo, hi], 0 (bottom) to 1 (top); pressure
    /// scales keep zero in the middle (FieldGeometry::scalePosition).
    Q_INVOKABLE double fieldScalePosition(const QString& scalar, double value, double lo, double hi) const;
    /// Part details for the properties panel ({} for unknown ids).
    Q_INVOKABLE QVariantMap partInfo(quint64 id) const;

    /// Nearest hit of a ray (model coordinates) with the visible parts' meshes:
    /// {partId, point (QVector3D), triangle}, or {} when nothing is hit. With `skipId`
    /// that part is left out; with `onlyId` only that part is tried.
    Q_INVOKABLE QVariantMap pickSurface(const QVector3D& origin, const QVector3D& direction, quint64 skipId = 0,
                                        quint64 onlyId = 0) const;
    /// For a click that misses (into a hole of a scan, such as a grille opening): the
    /// visible surface point seen closest to the ray, within `maxAngleDeg` of it, as
    /// pickSurface's map plus snapped: true; {} when nothing is that close.
    Q_INVOKABLE QVariantMap pickNearSurface(const QVector3D& origin, const QVector3D& direction,
                                            double maxAngleDeg) const;
    /// The visible sketch line closest to a ray (model coordinates), within `maxDistance`
    /// mm of it: {partId, point (on the line, QVector3D), distance (along the ray)}, or {}.
    /// Measure picks sketch curves with it: they have no surface to ray-cast.
    Q_INVOKABLE QVariantMap pickSketchLine(const QVector3D& origin, const QVector3D& direction,
                                           double maxDistance) const;
    /// `point` moved along `normal` onto the first skin of part `id` (and the scan it was
    /// fitted from) seen from outside, for drawing sketch previews where the draped sketch
    /// is shown; `point` itself on a miss.
    Q_INVOKABLE QVector3D dropOntoPart(quint64 id, const QVector3D& point, const QVector3D& normal) const;
    /// Like pickSurface, on one part only (and the scan it was fitted from, which reaches
    /// further); {} on a miss.
    Q_INVOKABLE QVariantMap pickPartSurface(quint64 id, const QVector3D& origin, const QVector3D& direction) const;

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
    /// Warning shown in the Wind tunnel panel when the part's size is implausible for a run
    /// (most likely the wrong unit, CAD-109); empty when it is fine.
    Q_INVOKABLE QString partSizeWarning(quint64 id) const;
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
    /// Before any command runs (from the UI or the AI agent), so a live preview of the view
    /// (Move / Rotate / Scale) can be applied or dropped first when the command changes,
    /// saves, exports or analyses the document.
    void commandAboutToRun(const QString& command, bool modifiesDocument);
    void statusMessageChanged();
    /// Emitted after a successful import so the view can frame the new model.
    void partImported(quint64 id);
    /// The displayed selection of a part changed without a document change (brush preview).
    void selectionPreviewChanged(quint64 id);
    /// Emitted after runScript() so the view can frame the result.
    void scriptFinished();
    void inspectChanged();
    void projectChanged();
    void recentProjectsChanged();
    void autosaveSecondsChanged();
    /// After a project was opened or recovered; `view` is the stored camera state.
    void projectOpened(const QVariantMap& view);

private:
    /// Shared fast queries on one mesh (scans of millions of triangles), cached.
    [[nodiscard]] const core::MeshProjector& projectorFor(const std::shared_ptr<const core::TriangleMesh>& mesh,
                                                          const core::Vec3& outward) const;
    mutable std::shared_ptr<const core::TriangleMesh> m_projectorMesh;
    mutable std::unique_ptr<core::MeshProjector> m_projector;

    void setStatusMessage(const QString& message);
    /// Runs the heat map again after its parts changed (or clears it when they are gone).
    void refreshHeatMap();

    bool m_heatMapRefreshQueued = false;
    bool openProject(const QString& path, bool recover);
    bool saveTo(const QString& path, const QVariantMap& view);
    void addRecent(const QString& path);
    /// Deletes an autosave copy unless another running instance holds its lock.
    void removeAutosave(const QString& path);
    /// Autosave file of the open project (per project path, or per process if untitled).
    [[nodiscard]] QString autosavePath() const;
    [[nodiscard]] static QString autosaveDir();

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
    McpAccess m_mcp{*this};
    QStringList m_recent;
    QTimer m_autosaveTimer;
    /// Held while this process owns an autosave file, so other instances do not offer it
    /// for recovery.
    std::unique_ptr<QLockFile> m_autosaveLock;
};

} // namespace aerocad::app
