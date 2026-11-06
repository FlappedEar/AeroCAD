// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "PartListModel.hpp"

#include "aerocad/commands/Session.hpp"
#include "aerocad/core/MeshSelection.hpp"

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
    /// Convenience wrapper around document.import for file dialogs and drag & drop.
    Q_INVOKABLE bool importFile(const QUrl& url);
    /// Part details for the properties panel ({} for unknown ids).
    Q_INVOKABLE QVariantMap partInfo(quint64 id) const;

    /// Shows a message in the status bar.
    Q_INVOKABLE void showStatus(const QString& message) { setStatusMessage(message); }

    /// Brush stroke with live preview. brushBegin starts a stroke on a part, brushAdd adds
    /// a dab at a point in model coordinates (mm), brushEnd runs selection.brush with all
    /// points as one undoable command. mode: "add" or "subtract".
    Q_INVOKABLE bool brushBegin(quint64 id, double radius, bool connected, const QString& mode);
    Q_INVOKABLE void brushAdd(const QVector3D& point);
    Q_INVOKABLE QVariantMap brushEnd();
    Q_INVOKABLE void brushCancel();
    [[nodiscard]] Q_INVOKABLE bool brushActive() const { return m_brush.has_value(); }

    /// Selection to draw for a part: the brush preview during a stroke, else the part's
    /// selection. nullptr when there is none.
    [[nodiscard]] const core::TriangleSet* displayedSelection(quint64 id) const;

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

private:
    void setStatusMessage(const QString& message);

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
