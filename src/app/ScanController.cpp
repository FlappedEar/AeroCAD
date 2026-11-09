// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScanController.hpp"

#include <algorithm>
#include <cstdint>

#include "AppController.hpp"

#include "aerocad/core/MeshSimplify.hpp"
#include "aerocad/core/Path.hpp"
#include "aerocad/core/Units.hpp"
#include "aerocad/io/Import.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QLocale>
#include <QSettings>
#include <QThread>

#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace aerocad::app {

namespace {
ScanController* g_instance = nullptr;
}

struct ScanController::Parsed {
    std::optional<io::ImportResult> result;
    QString error;
    bool cancelled = false;
};

ScanController::ScanController(AppController* app, QObject* parent) : QObject(parent), m_app(app)
{
    g_instance = this;
    QSettings settings;
    const QString units = settings.value(QStringLiteral("scan/importUnits"), QStringLiteral("mm")).toString();
    if (core::parseLengthUnit(units.toStdString()))
        m_importUnits = units;
    m_triangleBudget = std::max(0, settings.value(QStringLiteral("display/triangleBudget"), 1000000).toInt());

    m_progressTimer.setInterval(50);
    connect(&m_progressTimer, &QTimer::timeout, this, [this] {
        if (m_jobProgress)
            setProgress(m_jobProgress->load(), m_progressText);
    });
    // New / Open (from the menu, a script or MCP) ends the imports of the old project.
    connect(m_app, &AppController::projectChanged, this, &ScanController::dropStaleImports);
    // Parts whose mesh changed or that were removed drop their cached level of detail.
    connect(m_app, &AppController::partChanged, this, [this](quint64 id) {
        auto it = m_lods.find(id);
        if (it == m_lods.end())
            return;
        const model::Part* p = m_app->part(id);
        if (p == nullptr || p->mesh != it->source)
            m_lods.erase(it);
    });
}

ScanController::~ScanController()
{
    if (m_jobCancel)
        m_jobCancel->store(true);
    if (m_job != nullptr) {
        m_job->wait();
        delete m_job;
    }
    if (g_instance == this)
        g_instance = nullptr;
}

ScanController* ScanController::instance()
{
    return g_instance;
}

void ScanController::setImportUnits(const QString& units)
{
    if (units == m_importUnits || !core::parseLengthUnit(units.toStdString()))
        return;
    m_importUnits = units;
    QSettings().setValue(QStringLiteral("scan/importUnits"), units);
    emit importUnitsChanged();
}

void ScanController::setTriangleBudget(int budget)
{
    budget = std::max(0, budget);
    if (budget == m_triangleBudget)
        return;
    m_triangleBudget = budget;
    QSettings().setValue(QStringLiteral("display/triangleBudget"), budget);
    m_lods.clear();
    emit triangleBudgetChanged();
    // Every part re-evaluates its display mesh.
    if (m_app) {
        for (const auto& p : m_app->session().document().parts())
            emit displayMeshReady(p.id);
    }
}

QStringList ScanController::unitSymbols() const
{
    QStringList out;
    for (const auto u : core::allLengthUnits())
        out << QString::fromUtf8(core::unitSymbol(u).data(), static_cast<qsizetype>(core::unitSymbol(u).size()));
    return out;
}

bool ScanController::importFile(const QUrl& url)
{
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    if (path.isEmpty())
        return false;
    // Project files are documents, not meshes: open them directly.
    if (QFileInfo(path).suffix().compare(QStringLiteral("aerocad"), Qt::CaseInsensitive) == 0)
        return m_app->openFile(url);
    // The job belongs to the project open now and keeps the units chosen now.
    m_queue << ImportJob{QDir::toNativeSeparators(path), m_importUnits, projectGeneration()};
    if (m_job == nullptr && !m_parsed)
        startNextImport();
    return true;
}

void ScanController::cancelImport()
{
    m_queue.clear();
    if (m_jobCancel)
        m_jobCancel->store(true);
}

std::uint64_t ScanController::projectGeneration() const
{
    return m_app ? m_app->session().project().generation : 0;
}

void ScanController::dropStaleImports()
{
    const std::uint64_t generation = projectGeneration();
    m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
                                 [generation](const ImportJob& j) { return j.generation != generation; }),
                  m_queue.end());
    if (m_job != nullptr && m_current.generation != generation && m_jobCancel)
        m_jobCancel->store(true);
}

void ScanController::setProgress(double value, const QString& text)
{
    if (value == m_progressValue && text == m_progressText)
        return;
    m_progressValue = value;
    m_progressText = text;
    emit progressChanged();
}

void ScanController::startNextImport()
{
    dropStaleImports();
    if (m_queue.isEmpty() || m_job != nullptr)
        return;
    m_current = m_queue.takeFirst();
    m_jobProgress = std::make_shared<std::atomic<double>>(0.0);
    m_jobCancel = std::make_shared<std::atomic<bool>>(false);
    auto parsed = std::make_shared<Parsed>();
    const std::filesystem::path path = core::pathFromUtf8(m_current.path.toStdString());
    auto progress = m_jobProgress;
    auto cancel = m_jobCancel;
    m_job = QThread::create([path, parsed, progress, cancel] {
        try {
            io::ImportOptions options;
            options.progress = [progress, cancel](double fraction) {
                progress->store(fraction);
                return !cancel->load();
            };
            parsed->result = io::importFile(path, options);
        } catch (const io::ImportCancelled&) {
            parsed->cancelled = true;
        } catch (const std::exception& e) {
            parsed->error = QString::fromStdString(e.what());
        }
    });
    m_parsed = parsed;
    connect(m_job, &QThread::finished, this, &ScanController::finishImport);
    setProgress(0.0, tr("Importing %1…").arg(QFileInfo(m_current.path).fileName()));
    m_progressTimer.start();
    m_job->start();
    emit busyChanged();
}

void ScanController::finishImport()
{
    m_progressTimer.stop();
    m_job->deleteLater();
    m_job = nullptr;
    std::shared_ptr<Parsed> parsed = std::exchange(m_parsed, nullptr);
    m_jobProgress.reset();
    m_jobCancel.reset();

    const QString name = QFileInfo(m_current.path).fileName();
    if (m_current.generation != projectGeneration()) {
        // The project it was started for was replaced (New / Open): nothing is added.
        m_app->showStatus(tr("Import of %1 discarded: the project was replaced").arg(name));
        m_lastImportOk = false;
    } else if (parsed->cancelled) {
        m_app->showStatus(tr("Import of %1 cancelled").arg(name));
        m_lastImportOk = false;
    } else if (!parsed->result) {
        m_app->showStatus(parsed->error);
        m_lastImportOk = false;
        emit importFailed(parsed->error);
    } else {
        setProgress(1.0, tr("Adding %1…").arg(name));
        const std::string key = m_current.path.toStdString();
        m_app->session().importStage().put(key, std::move(*parsed->result));
        const QVariantMap r = m_app->execute(QStringLiteral("document.import"),
                                             {{QStringLiteral("path"), m_current.path},
                                              {QStringLiteral("units"), m_current.units}});
        // Never leave a staged result behind (e.g. when the command rejected its params).
        (void)m_app->session().importStage().take(key);
        if (r.value(QStringLiteral("ok")).toBool()) {
            const QVariantMap data = r.value(QStringLiteral("result")).toMap();
            const QString units = data.value(QStringLiteral("unitSource")).toString() == QStringLiteral("file")
                                      ? tr("file units → mm")
                                      : tr("%1 → mm").arg(data.value(QStringLiteral("units")).toString());
            // A STEP file with several bodies gives several parts (CAD-227): count them all.
            const QVariantList parts = data.value(QStringLiteral("parts")).toList();
            qlonglong triangles = data.value(QStringLiteral("triangles")).toLongLong();
            if (!parts.isEmpty()) {
                triangles = 0;
                for (const QVariant& p : parts)
                    triangles += p.toMap().value(QStringLiteral("triangles")).toLongLong();
            }
            const QString format = data.value(QStringLiteral("format")).toString();
            const QString what = parts.isEmpty() ? format : tr("%1, %2 bodies").arg(format).arg(parts.size());
            m_app->showStatus(tr("Imported %1 (%2, %3 triangles, %4)")
                                  .arg(name, what, QLocale().toString(triangles), units));
            emit m_app->partImported(data.value(QStringLiteral("id")).toULongLong());
        } else {
            m_lastImportOk = false;
        }
    }
    m_current = {};
    emit busyChanged();
    startNextImport();
}

bool ScanController::waitForImports()
{
    m_lastImportOk = true;
    while (m_job != nullptr || !m_queue.isEmpty()) {
        if (m_job != nullptr)
            m_job->wait();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (m_job == nullptr && !m_queue.isEmpty())
            startNextImport();
    }
    return m_lastImportOk;
}

void ScanController::waitForDisplayMeshes()
{
    if (!m_app)
        return;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 120000) {
        bool pending = false;
        for (const auto& p : m_app->session().document().parts()) {
            if (p.mesh && !displayMesh(p))
                pending = true;
        }
        if (!pending)
            return;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
}

QVector3D ScanController::snapToMesh(quint64 partId, const QVector3D& point) const
{
    const model::Part* p = m_app ? m_app->part(partId) : nullptr;
    if (p == nullptr || !p->mesh || p->mesh->vertexCount() == 0)
        return point;
    const core::Vec3 q{point.x(), point.y(), point.z()};
    double best = std::numeric_limits<double>::max();
    core::Vec3 nearest = q;
    for (const auto& v : p->mesh->vertices()) {
        const core::Vec3 d = v - q;
        const double dd = d.dot(d);
        if (dd < best) {
            best = dd;
            nearest = v;
        }
    }
    return {static_cast<float>(nearest.x), static_cast<float>(nearest.y), static_cast<float>(nearest.z)};
}

qulonglong ScanController::displayedTriangles(quint64 partId) const
{
    const model::Part* p = m_app ? m_app->part(partId) : nullptr;
    if (p == nullptr || !p->mesh)
        return 0;
    if (m_triangleBudget == 0 || p->shape || p->mesh->triangleCount() <= static_cast<std::size_t>(m_triangleBudget))
        return p->mesh->triangleCount();
    const auto it = m_lods.constFind(partId);
    if (it == m_lods.cend() || it->source != p->mesh || !it->mesh)
        return 0;
    return it->mesh->triangleCount();
}

std::shared_ptr<const core::TriangleMesh> ScanController::displayMesh(const model::Part& part)
{
    if (!part.mesh)
        return nullptr;
    // Tessellated B-rep parts are small and need their exact normals; never decimate them.
    if (m_triangleBudget == 0 || part.shape ||
        part.mesh->triangleCount() <= static_cast<std::size_t>(m_triangleBudget))
        return part.mesh;

    auto it = m_lods.find(part.id);
    if (it != m_lods.end() && it->source == part.mesh && it->budget == m_triangleBudget)
        return it->mesh; // nullptr while computing

    Lod lod;
    lod.source = part.mesh;
    lod.budget = m_triangleBudget;
    for (const CachedLod& c : m_lodCache) {
        if (c.budget == m_triangleBudget && c.source.lock() == part.mesh) {
            lod.mesh = c.mesh;
            m_lods.insert(part.id, lod);
            return lod.mesh;
        }
    }
    m_lods.insert(part.id, lod);

    const quint64 id = part.id;
    auto source = part.mesh;
    const int budget = m_triangleBudget;
    QPointer<ScanController> self(this);
    QThread* worker = QThread::create([self, id, source, budget] {
        auto mesh = std::make_shared<const core::TriangleMesh>(
            core::decimateToTarget(*source, static_cast<std::size_t>(budget)));
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, id, source, budget, mesh] {
                if (!self)
                    return;
                auto entry = self->m_lods.find(id);
                if (entry == self->m_lods.end() || entry->source != source || entry->budget != budget)
                    return; // stale (mesh changed or budget changed meanwhile)
                entry->mesh = mesh;
                self->rememberLod(source, budget, mesh);
                emit self->displayMeshReady(id);
            },
            Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);
    return nullptr;
}

void ScanController::rememberLod(const std::shared_ptr<const core::TriangleMesh>& source, int budget,
                                 const std::shared_ptr<const core::TriangleMesh>& mesh)
{
    std::erase_if(m_lodCache, [](const CachedLod& c) { return c.source.expired(); });
    m_lodCache.insert(m_lodCache.begin(), CachedLod{source, budget, mesh});
    constexpr std::size_t kCacheSize = 4;
    if (m_lodCache.size() > kCacheSize)
        m_lodCache.resize(kCacheSize);
}

} // namespace aerocad::app
