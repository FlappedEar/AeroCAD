// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVector3D>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>
#include <vector>

class QThread;

namespace aerocad::model {
struct Part;
}

namespace aerocad::app {

class AppController;

/// Scan workflow support for the UI:
/// - background import with progress and cancel (files are parsed on a worker thread,
///   staged, then added through the document.import command on the UI thread);
/// - display level of detail: meshes above `triangleBudget` are drawn decimated (computed
///   in the background) while the document keeps the full mesh for selection and fitting;
/// - snapping picked points to the full-resolution mesh.
class ScanController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by main()")
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged)
    Q_PROPERTY(QString importUnits READ importUnits WRITE setImportUnits NOTIFY importUnitsChanged)
    Q_PROPERTY(int triangleBudget READ triangleBudget WRITE setTriangleBudget NOTIFY triangleBudgetChanged)
    Q_PROPERTY(QStringList unitSymbols READ unitSymbols CONSTANT)

public:
    explicit ScanController(AppController* app, QObject* parent = nullptr);
    ~ScanController() override;
    ScanController(const ScanController&) = delete;
    ScanController& operator=(const ScanController&) = delete;

    /// The application's instance (used by MeshGeometry); nullptr before construction.
    [[nodiscard]] static ScanController* instance();

    [[nodiscard]] bool busy() const { return m_job != nullptr; }
    [[nodiscard]] double progress() const { return m_progressValue; }
    [[nodiscard]] QString progressText() const { return m_progressText; }
    [[nodiscard]] QString importUnits() const { return m_importUnits; }
    void setImportUnits(const QString& units);
    /// Maximum number of triangles drawn per part; 0 draws every triangle.
    [[nodiscard]] int triangleBudget() const { return m_triangleBudget; }
    void setTriangleBudget(int budget);
    [[nodiscard]] QStringList unitSymbols() const;

    /// Queues a file for background import (units: importUnits). Returns false for
    /// unsupported URLs.
    Q_INVOKABLE bool importFile(const QUrl& url);
    /// Cancels the running import and drops queued ones.
    Q_INVOKABLE void cancelImport();
    /// Blocks until queued imports are finished (scripts, smoke test). Returns false if one failed.
    bool waitForImports();
    /// Blocks until every part's display mesh is available (smoke test, screenshots).
    void waitForDisplayMeshes();

    /// Nearest full-resolution mesh vertex to `point` (model coordinates) on a part, or
    /// `point` itself when the part has no mesh.
    Q_INVOKABLE QVector3D snapToMesh(quint64 partId, const QVector3D& point) const;
    /// Number of triangles currently drawn for a part (0 while its level of detail is computed).
    Q_INVOKABLE qulonglong displayedTriangles(quint64 partId) const;

    /// Mesh to draw for a part: the part's mesh when it fits the budget, else a decimated
    /// copy. Returns nullptr while that copy is computed; displayMeshReady(id) follows.
    [[nodiscard]] std::shared_ptr<const core::TriangleMesh> displayMesh(const model::Part& part);

signals:
    void busyChanged();
    void progressChanged();
    void importUnitsChanged();
    void triangleBudgetChanged();
    void displayMeshReady(quint64 partId);
    /// A queued import failed (message also shown in the status bar).
    void importFailed(const QString& message);

private:
    struct Lod {
        std::shared_ptr<const core::TriangleMesh> source;
        std::shared_ptr<const core::TriangleMesh> mesh; ///< nullptr while computing
        int budget = 0;
    };

    /// A queued file with the settings captured when it was queued.
    struct ImportJob {
        QString path;
        QString units;
        std::uint64_t generation = 0; ///< ProjectState::generation it belongs to
    };

    void startNextImport();
    void finishImport();
    [[nodiscard]] std::uint64_t projectGeneration() const;
    /// Cancels the running job and drops queued files that belong to a replaced project.
    void dropStaleImports();
    void setProgress(double value, const QString& text);

    QPointer<AppController> m_app;
    QList<ImportJob> m_queue;
    ImportJob m_current;
    QThread* m_job = nullptr;
    std::shared_ptr<std::atomic<double>> m_jobProgress;
    std::shared_ptr<std::atomic<bool>> m_jobCancel;
    QTimer m_progressTimer;
    double m_progressValue = 0.0;
    QString m_progressText;
    QString m_importUnits = QStringLiteral("mm");
    int m_triangleBudget = 1000000;
    bool m_lastImportOk = true;
    QHash<quint64, Lod> m_lods;
    /// Recently computed reduced meshes by source mesh, so Undo / Redo back to a mesh shown
    /// before draws it at once instead of computing it again (the part would vanish meanwhile).
    struct CachedLod {
        std::weak_ptr<const core::TriangleMesh> source;
        int budget = 0;
        std::shared_ptr<const core::TriangleMesh> mesh;
    };
    std::vector<CachedLod> m_lodCache;
    void rememberLod(const std::shared_ptr<const core::TriangleMesh>& source, int budget,
                     const std::shared_ptr<const core::TriangleMesh>& mesh);
    /// Parse result handed from the worker thread to the UI thread.
    struct Parsed;
    std::shared_ptr<Parsed> m_parsed;
};

} // namespace aerocad::app
