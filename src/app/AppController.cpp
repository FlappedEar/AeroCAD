// SPDX-License-Identifier: GPL-3.0-or-later
#include "AppController.hpp"

#include "FieldGeometry.hpp"
#include "InspectGeometry.hpp"
#include "JsonBridge.hpp"
#include "PickFilter.hpp"

#include "aerocad/BuildInfo.hpp"
#include "aerocad/cfd/Settings.hpp"
#include "aerocad/core/MeshCheck.hpp"
#include "aerocad/core/Path.hpp"

#include <cmath>
#include "aerocad/io/Import.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QGuiApplication>
#include <QLocale>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <thread>

namespace aerocad::app {

namespace {

PickFilter pickFilter(const QVariantMap& options)
{
    PickFilter filter;
    filter.kind = options.value(QStringLiteral("kind")).toString().toStdString();
    for (const auto& id : options.value(QStringLiteral("hiddenPartIds")).toList())
        filter.hiddenPartIds.push_back(id.toULongLong());
    const auto feature = [&](const QString& name) -> std::optional<std::uint64_t> {
        const QVariant value = options.value(name);
        bool ok = false;
        const qlonglong id = value.toLongLong(&ok);
        return ok && id >= 0 ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(id)) : std::nullopt;
    };
    filter.shownSketch = feature(QStringLiteral("shownSketch"));
    filter.unpickablePlane = feature(QStringLiteral("unpickablePlane"));
    return filter;
}

constexpr int kMaxRecent = 10;
const QString kProjectExtension = QStringLiteral("aerocad");

QString toLocalPath(const QUrl& url)
{
    return QDir::toNativeSeparators(url.isLocalFile() ? url.toLocalFile() : url.toString());
}

} // namespace

QString AppController::localPath(const QUrl& url) const
{
    return toLocalPath(url);
}

AppController::AppController(QObject* parent) : QObject(parent), m_parts(m_session.document())
{
    m_session.project().generator = std::string("AeroCAD ") + aerocad::buildStamp();
    QSettings settings;
    m_recent = settings.value(QStringLiteral("project/recent")).toStringList();
    m_autosaveTimer.setInterval(std::max(10, autosaveSeconds()) * 1000);
    connect(&m_autosaveTimer, &QTimer::timeout, this, [this] { autosaveNow(); });
    if (autosaveSeconds() > 0)
        m_autosaveTimer.start();

    m_session.document().addObserver([this](model::Document::Change change, model::PartId id) {
        using Change = model::Document::Change;
        if (change == Change::AboutToAdd || change == Change::AboutToRemove)
            return;
        m_bounds.reset();
        // Drop the pick hierarchies of replaced meshes; start one for a new big mesh, so
        // hovering it is fast from the start.
        std::vector<const core::TriangleMesh*> meshes;
        for (const auto& p : m_session.document().parts())
            if (p.mesh)
                meshes.push_back(p.mesh.get());
        m_pickCache.retain(meshes);
        std::erase_if(m_meshFacts, [&meshes](const auto& entry) {
            return std::find(meshes.begin(), meshes.end(), entry.first) == meshes.end();
        });
        if (const model::Part* changed = part(id); changed != nullptr && changed->mesh)
            m_pickCache.prepare(changed->mesh);
        emit partChanged(id);
        emit boundsChanged();
        // A heat map belongs to a mesh: recompute it when its parts change. Queued, because
        // this runs inside the command that changed the part.
        const commands::HeatMap& h = m_session.inspect().heatMap();
        if (h.enabled && (id == h.part || id == h.reference) && !m_heatMapRefreshQueued) {
            m_heatMapRefreshQueued = true;
            QMetaObject::invokeMethod(this, &AppController::refreshHeatMap, Qt::QueuedConnection);
        }
    });
    m_session.inspect().setListener([this]() { emit inspectChanged(); });
}

void AppController::refreshHeatMap()
{
    m_heatMapRefreshQueued = false;
    commands::InspectState& inspect = m_session.inspect();
    const commands::HeatMap h = inspect.heatMap();
    if (!h.enabled)
        return;
    const model::Part* measured = part(h.part);
    const bool referenceGone = h.kind == commands::HeatMap::Kind::Deviation && part(h.reference) == nullptr;
    if (measured == nullptr || referenceGone) {
        inspect.setHeatMap({});
        return;
    }
    // Visibility or name changes keep the meshes: nothing to recompute.
    const model::Part* reference = part(h.reference);
    if (measured->mesh == h.mesh &&
        (h.kind != commands::HeatMap::Kind::Deviation || (reference != nullptr && reference->mesh == h.referenceMesh)))
        return;
    const char* name = h.kind == commands::HeatMap::Kind::Deviation ? "inspect.deviation" : "inspect.thickness";
    const commands::CommandResult r = m_session.execute(name, h.params);
    if (!r.ok) {
        inspect.setHeatMap({});
        setStatusMessage(tr("Heat map cleared: %1").arg(QString::fromStdString(r.error)));
    }
}

int AppController::inspectRevision() const
{
    return static_cast<int>(m_session.inspect().revision() & 0x7fffffff);
}

QVariantMap AppController::inspectInfo() const
{
    const commands::InspectState& st = m_session.inspect();
    const auto& s = st.section();
    auto vec = [](const core::Vec3& v) { return QVariantList{v.x, v.y, v.z}; };
    QVariantMap section{{QStringLiteral("enabled"), s.enabled},
                        {QStringLiteral("origin"), vec(s.plane.origin)},
                        {QStringLiteral("normal"), vec(s.plane.normal)},
                        {QStringLiteral("cap"), s.cap}};
    const auto& h = st.heatMap();
    QVariantMap heat{{QStringLiteral("enabled"), h.enabled}};
    if (h.enabled) {
        heat.insert({{QStringLiteral("kind"), h.kind == commands::HeatMap::Kind::Deviation ? QStringLiteral("deviation")
                                                                                             : QStringLiteral("thickness")},
                     {QStringLiteral("part"), QVariant::fromValue<quint64>(h.part)},
                     {QStringLiteral("reference"), QVariant::fromValue<quint64>(h.reference)},
                     {QStringLiteral("rangeMin"), h.rangeMin},
                     {QStringLiteral("rangeMax"), h.rangeMax},
                     {QStringLiteral("min"), h.stats.min},
                     {QStringLiteral("max"), h.stats.max},
                     {QStringLiteral("mean"), h.stats.mean},
                     {QStringLiteral("rms"), h.stats.rms},
                     {QStringLiteral("stddev"), h.stats.stddev}});
        if (h.kind == commands::HeatMap::Kind::Deviation && h.params.contains("tolerance") && h.values &&
            h.stats.count > 0) {
            const double tol = h.params["tolerance"].get<double>();
            std::size_t within = 0;
            for (const double v : *h.values)
                within += std::isfinite(v) && std::abs(v) <= tol ? 1 : 0;
            heat.insert(QStringLiteral("tolerance"), tol);
            heat.insert(QStringLiteral("withinTolerance"),
                        100.0 * static_cast<double>(within) / static_cast<double>(h.stats.count));
        }
    }
    return {{QStringLiteral("section"), section}, {QStringLiteral("heatMap"), heat}};
}

bool AppController::sectionCuts(quint64 id) const
{
    const model::Part* p = part(id);
    return p != nullptr && p->kind != model::PartKind::Sketch && m_session.inspect().section().cuts(id);
}

QString AppController::partSizeWarning(quint64 id) const
{
    const model::Part* p = part(id);
    if (p == nullptr || !p->mesh || p->mesh->isEmpty())
        return {};
    const core::Vec3 size = p->mesh->bounds().size();
    // Model millimetres to metres, as the case writer converts them.
    return QString::fromStdString(cfd::partSizeWarning(std::max({size.x, size.y, size.z}) / 1000.0));
}

QString AppController::partOpenWarning(quint64 id) const
{
    const model::Part* p = part(id);
    if (p == nullptr || !p->mesh || p->mesh->isEmpty() || p->kind == model::PartKind::Sketch ||
        p->kind == model::PartKind::Plane)
        return {};
    // A scan can have millions of triangles: check each mesh once, a big one in the
    // background (meshChecksChanged follows) so an edit never waits for it.
    MeshFacts& facts = meshFacts(p->mesh);
    if (!facts.openEdges && !facts.counting) {
        constexpr std::size_t kSyncTriangles = 100000;
        if (p->mesh->triangleCount() <= kSyncTriangles) {
            facts.openEdges = core::checkMesh(*p->mesh).boundaryEdges;
        } else {
            facts.counting = true;
            QPointer<AppController> self(const_cast<AppController*>(this));
            std::shared_ptr<const core::TriangleMesh> mesh = p->mesh;
            std::thread([self, mesh] {
                const std::size_t open = core::checkMesh(*mesh).boundaryEdges;
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [self, mesh, open] {
                        if (!self)
                            return;
                        for (auto& [key, f] : self->m_meshFacts) {
                            if (key == mesh.get() && f.mesh.lock() == mesh) {
                                f.openEdges = open;
                                f.counting = false;
                            }
                        }
                        ++self->m_meshCheckRevision;
                        emit self->meshChecksChanged();
                    },
                    Qt::QueuedConnection);
            }).detach();
        }
    }
    return facts.openEdges ? QString::fromStdString(cfd::openPartWarning(*facts.openEdges)) : QString();
}

QVariantMap AppController::meshStats(quint64 id) const
{
    const model::Part* p = part(id);
    if (p == nullptr || !p->mesh)
        return {};
    // A check of a multi-million triangle scan takes most of a second; the panel asks
    // after every change of the part (also a rename or a selection), so check each mesh once.
    MeshFacts& facts = meshFacts(p->mesh);
    if (!facts.stats && !facts.checking) {
        constexpr std::size_t kSyncTriangles = 100000;
        if (p->mesh->triangleCount() <= kSyncTriangles) {
            facts.stats = core::meshStats(*p->mesh);
        } else {
            facts.checking = true;
            QPointer<AppController> self(const_cast<AppController*>(this));
            std::shared_ptr<const core::TriangleMesh> mesh = p->mesh;
            std::thread([self, mesh] {
                const core::MeshStats stats = core::meshStats(*mesh);
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [self, mesh, stats] {
                        if (!self)
                            return;
                        for (auto& [key, f] : self->m_meshFacts) {
                            if (key == mesh.get() && f.mesh.lock() == mesh) {
                                f.stats = stats;
                                f.checking = false;
                            }
                        }
                        ++self->m_meshCheckRevision;
                        emit self->meshChecksChanged();
                    },
                    Qt::QueuedConnection);
            }).detach();
        }
    }
    if (!facts.stats)
        return {{QStringLiteral("pending"), true}};
    const core::MeshStats& s = *facts.stats;
    auto n = [](std::size_t v) { return QVariant::fromValue(static_cast<double>(v)); };
    QVariantMap out{{QStringLiteral("id"), QVariant::fromValue(static_cast<double>(p->id))},
                    {QStringLiteral("vertices"), n(s.vertices)},
                    {QStringLiteral("triangles"), n(s.triangles)},
                    {QStringLiteral("unusedVertices"), n(s.unusedVertices)},
                    {QStringLiteral("degenerateTriangles"), n(s.degenerateTriangles)},
                    {QStringLiteral("duplicateTriangles"), n(s.duplicateTriangles)},
                    {QStringLiteral("boundaryEdges"), n(s.boundaryEdges)},
                    {QStringLiteral("holes"), n(s.holes)},
                    {QStringLiteral("largestBoundaryEdges"), n(s.largestBoundaryEdges)},
                    {QStringLiteral("loopedBoundaryEdges"), n(s.loopedBoundaryEdges)},
                    {QStringLiteral("nonManifoldEdges"), n(s.nonManifoldEdges)},
                    {QStringLiteral("inconsistentEdges"), n(s.inconsistentEdges)},
                    {QStringLiteral("components"), n(s.components)},
                    {QStringLiteral("closed"), s.boundaryEdges == 0 && s.nonManifoldEdges == 0 && s.triangles > 0}};
    const core::BoundingBox box = p->mesh->bounds();
    if (!box.isEmpty()) {
        auto vec = [](const core::Vec3& v) { return QVariantList{v.x, v.y, v.z}; };
        out.insert(QStringLiteral("bounds"), QVariantMap{{QStringLiteral("min"), vec(box.min)},
                                                         {QStringLiteral("max"), vec(box.max)},
                                                         {QStringLiteral("size"), vec(box.size())}});
    }
    return out;
}

AppController::MeshFacts& AppController::meshFacts(const std::shared_ptr<const core::TriangleMesh>& mesh) const
{
    for (auto& [key, facts] : m_meshFacts)
        if (key == mesh.get() && facts.mesh.lock() == mesh)
            return facts;
    std::erase_if(m_meshFacts, [&mesh](const auto& entry) { return entry.first == mesh.get(); });
    MeshFacts facts;
    facts.mesh = mesh;
    m_meshFacts.emplace_back(mesh.get(), std::move(facts));
    return m_meshFacts.back().second;
}

bool AppController::showsHeatMap(quint64 id) const
{
    const auto& h = m_session.inspect().heatMap();
    const model::Part* p = part(id);
    return h.enabled && h.part == id && p != nullptr && p->mesh == h.mesh;
}

QColor AppController::heatColor(double t) const
{
    float c[4];
    InspectGeometry::heatColor(t, c);
    return QColor::fromRgbF(c[0], c[1], c[2], c[3]);
}

const core::BoundingBox& AppController::documentBounds() const
{
    if (!m_bounds || m_boundsRevision != m_session.document().revision()) {
        m_bounds = m_session.document().bounds();
        m_boundsRevision = m_session.document().revision();
    }
    return *m_bounds;
}

QVector3D AppController::sceneCenter() const
{
    const core::Vec3 c = documentBounds().center();
    // Z-up model -> Y-up scene: (x, y, z) -> (x, z, -y).
    return {static_cast<float>(c.x), static_cast<float>(c.z), static_cast<float>(-c.y)};
}

double AppController::sceneRadius() const
{
    return documentBounds().diagonal() * 0.5;
}

bool AppController::canUndo() const
{
    return m_session.undoStack().canUndo();
}

bool AppController::canRedo() const
{
    return m_session.undoStack().canRedo();
}

QString AppController::undoText() const
{
    return QString::fromStdString(m_session.undoStack().undoLabel());
}

QString AppController::redoText() const
{
    return QString::fromStdString(m_session.undoStack().redoLabel());
}

QStringList AppController::importNameFilters() const
{
    QStringList patterns;
    for (const auto& ext : io::supportedImportExtensions())
        patterns << QStringLiteral("*.") + QString::fromStdString(ext);
    return {tr("3D files (%1)").arg(patterns.join(QLatin1Char(' '))), tr("All files (*)")};
}

QStringList AppController::exportNameFilters() const
{
    return {tr("STL, binary (*.stl)"), tr("3MF (*.3mf)"), tr("STEP (*.step *.stp)")};
}

QString AppController::version() const
{
    return QString::fromUtf8(aerocad::buildStamp());
}

QVariantMap AppController::preview(const QString& name, const QVariantMap& params)
{
    const commands::CommandResult result = m_session.preview(name.toStdString(), toJson(params));
    emit historyChanged();
    return toVariant(result.toJson()).toMap();
}

bool AppController::cancelPreview()
{
    if (!m_session.cancelPreview())
        return false;
    emit historyChanged();
    return true;
}

double AppController::historyTopId() const
{
    return static_cast<double>(m_session.undoStack().topId());
}

bool AppController::historyApplied(double id) const
{
    return id > 0 && m_session.undoStack().isApplied(static_cast<std::uint64_t>(id));
}

bool AppController::historyExpired(double id) const
{
    return id > 0 && m_session.undoStack().isExpired(static_cast<std::uint64_t>(id));
}

bool AppController::undoIfTop(double id)
{
    if (id <= 0 || m_session.undoStack().topId() != static_cast<std::uint64_t>(id))
        return false;
    return execute(QStringLiteral("edit.undo")).value(QStringLiteral("ok")).toBool();
}

bool AppController::addToSelectionHeld() const
{
    return (QGuiApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) != 0;
}

bool AppController::mergeHistoryIds(const QVariantList& ids, const QString& label)
{
    std::vector<std::uint64_t> tokens;
    tokens.reserve(static_cast<std::size_t>(ids.size()));
    for (const QVariant& v : ids)
        tokens.push_back(static_cast<std::uint64_t>(v.toDouble()));
    if (!m_session.undoStack().mergeIds(tokens, label.toStdString()))
        return false;
    emit historyChanged();
    return true;
}

QVariantMap AppController::execute(const QString& name, const QVariantMap& params)
{
    return toVariant(executeCommand(name.toStdString(), toJson(params)).toJson()).toMap();
}

commands::CommandResult AppController::executeCommand(const std::string& name, const commands::Json& params)
{
    const auto* info = m_session.registry().info(name);
    emit commandAboutToRun(QString::fromStdString(name), info != nullptr && info->modifiesDocument);
    const bool hadPreview = m_session.previewActive();
    const auto generation = m_session.project().generation;
    const auto activeVariant = m_session.project().variants.active();
    const commands::CommandResult result = m_session.execute(name, params);
    const bool projectReplaced = m_session.project().generation != generation;
    if (projectReplaced || m_session.project().variants.active() != activeVariant) {
        // A command can replace the editing context through MCP, a script, or Undo/Redo,
        // without using the File/Variant menu wrappers. Close stale editors before the
        // history notification can schedule their previews against the new context.
        emit editingContextChanged();
        if (projectReplaced)
            emit projectOpened(toVariant(m_session.project().view).toMap());
    }
    // A command that ended a live preview (e.g. project.save) also changed the document
    // back; the feature dialog rebuilds its preview on historyChanged.
    if ((info != nullptr && info->modifiesDocument) || (hadPreview && !m_session.previewActive()))
        emit historyChanged();
    emit projectChanged();

    if (!result.ok)
        setStatusMessage(QString::fromStdString(result.error));
    return result;
}

bool AppController::importFile(const QUrl& url)
{
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    const QVariantMap r =
        execute(QStringLiteral("document.import"), {{QStringLiteral("path"), QDir::toNativeSeparators(path)}});
    if (!r.value(QStringLiteral("ok")).toBool())
        return false;

    const QVariantMap data = r.value(QStringLiteral("result")).toMap();
    setStatusMessage(tr("Imported %1 (%2, %3 triangles)")
                         .arg(QFileInfo(path).fileName(), data.value(QStringLiteral("format")).toString(),
                              QLocale().toString(data.value(QStringLiteral("triangles")).toLongLong())));
    emit partImported(data.value(QStringLiteral("id")).toULongLong());
    return true;
}

QVariantMap AppController::exportFile(const QUrl& url, const QVariantList& ids, const QString& expectedState)
{
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    QVariantMap params{{QStringLiteral("path"), QDir::toNativeSeparators(path)}};
    if (!ids.isEmpty())
        params.insert(QStringLiteral("ids"), ids);
    if (!expectedState.isEmpty())
        params.insert(QStringLiteral("expectedState"), expectedState);
    const QVariantMap r = execute(QStringLiteral("document.export"), params);
    if (!r.value(QStringLiteral("ok")).toBool())
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), r.value(QStringLiteral("error"))}};

    const QVariantMap data = r.value(QStringLiteral("result")).toMap();
    const QVariantList warnings = data.value(QStringLiteral("warnings")).toList();
    const QString name = QFileInfo(path).fileName();
    const QString format = data.value(QStringLiteral("format")).toString();
    const qsizetype count = data.value(QStringLiteral("ids")).toList().size();
    if (warnings.isEmpty())
        setStatusMessage(tr("Exported %n part(s) to %1 (%2)", nullptr, static_cast<int>(count)).arg(name, format));
    else
        setStatusMessage(tr("Exported %n part(s) to %1 (%2) with %3 mesh warning(s)", nullptr, static_cast<int>(count))
                             .arg(name, format)
                             .arg(warnings.size()));
    return {{QStringLiteral("ok"), true}, {QStringLiteral("warnings"), warnings}};
}

bool AppController::runScript(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatusMessage(tr("Cannot open script %1").arg(path));
        return false;
    }
    const commands::Json script = commands::Json::parse(file.readAll().toStdString(), nullptr, false);
    if (!script.is_array()) {
        setStatusMessage(tr("Script %1 is not a JSON array of commands").arg(path));
        return false;
    }
    bool ok = true;
    bool fitView = false;
    for (const auto& step : script) {
        if (!step.is_object() || !step.contains("name") || !step["name"].is_string()) {
            setStatusMessage(tr("Script step is not an object with a string \"name\": %1")
                                 .arg(QString::fromStdString(step.dump())));
            ok = false;
            break;
        }
        const std::string name = step["name"].get<std::string>();
        const auto generation = m_session.project().generation;
        const auto activeVariant = m_session.project().variants.active();
        const commands::CommandResult r = executeCommand(name, step.value("params", commands::Json::object()));
        if (!r.ok) {
            setStatusMessage(tr("Script step %1 failed: %2")
                                 .arg(QString::fromStdString(name), QString::fromStdString(r.error)));
            ok = false;
            break;
        }
        if (m_session.project().generation != generation || m_session.project().variants.active() != activeVariant) {
            // Open already restored its camera; variant switches keep the existing one.
            fitView = false;
        } else if (const auto* info = m_session.registry().info(name); info != nullptr && info->modifiesDocument) {
            fitView = true;
        }
    }
    emit historyChanged();
    emit projectChanged();
    emit scriptFinished(fitView);
    return ok;
}

QString AppController::projectName() const
{
    const std::string& path = m_session.project().path;
    return path.empty() ? tr("Untitled") : QFileInfo(QString::fromStdString(path)).completeBaseName();
}

QString AppController::projectPath() const
{
    return QString::fromStdString(m_session.project().path);
}

bool AppController::projectDirty() const
{
    return m_session.project().dirty();
}

QStringList AppController::openNameFilters() const
{
    QStringList patterns{QStringLiteral("*.") + kProjectExtension};
    for (const auto& ext : io::supportedImportExtensions())
        patterns << QStringLiteral("*.") + QString::fromStdString(ext);
    return {tr("AeroCAD projects and 3D files (%1)").arg(patterns.join(QLatin1Char(' '))),
            tr("AeroCAD projects (*.%1)").arg(kProjectExtension), tr("All files (*)")};
}

QVariantList AppController::variants() const
{
    QVariantList out;
    const auto& vs = m_session.project().variants;
    for (const auto& v : vs.variants()) {
        out.push_back(QVariantMap{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(v.id)},
                                  {QStringLiteral("name"), QString::fromStdString(v.name)},
                                  {QStringLiteral("active"), v.id == vs.active()}});
    }
    return out;
}

QString AppController::activeVariantName() const
{
    const auto& vs = m_session.project().variants;
    const cad::Variant* v = vs.find(vs.active());
    return v != nullptr ? QString::fromStdString(v->name) : QString();
}

int AppController::autosaveSeconds() const
{
    return QSettings().value(QStringLiteral("project/autosaveSeconds"), 120).toInt();
}

void AppController::setAutosaveSeconds(int seconds)
{
    seconds = std::max(0, seconds);
    if (seconds == autosaveSeconds())
        return;
    QSettings().setValue(QStringLiteral("project/autosaveSeconds"), seconds);
    m_autosaveTimer.stop();
    if (seconds > 0) {
        m_autosaveTimer.setInterval(std::max(10, seconds) * 1000);
        m_autosaveTimer.start();
    }
    emit autosaveSecondsChanged();
}

bool AppController::openFile(const QUrl& url)
{
    const QString path = toLocalPath(url);
    if (QFileInfo(path).suffix().compare(kProjectExtension, Qt::CaseInsensitive) == 0)
        return openProject(path, false);
    return importFile(url);
}

bool AppController::openProject(const QString& path, bool recover)
{
    const QString previousAutosave = autosavePath();
    const QVariantMap r = execute(QStringLiteral("project.open"),
                                  {{QStringLiteral("path"), path}, {QStringLiteral("recover"), recover}});
    if (!r.value(QStringLiteral("ok")).toBool())
        return false;
    const QVariantMap data = r.value(QStringLiteral("result")).toMap();
    if (recover) {
        // Re-write the recovered changes under the project's own autosave name first; the
        // old copy is the only one on disk until then.
        const QString own = QFileInfo(autosavePath()).absoluteFilePath();
        if (autosaveNow() && own != QFileInfo(path).absoluteFilePath())
            removeAutosave(path);
        setStatusMessage(tr("Recovered unsaved changes of %1").arg(projectName()));
    } else {
        removeAutosave(previousAutosave);
        addRecent(path);
        const int errors = static_cast<int>(data.value(QStringLiteral("featureErrors")).toList().size());
        const QString file = QFileInfo(path).fileName();
        setStatusMessage(errors == 0 ? tr("Opened %1").arg(file)
                                     : tr("Opened %1 (%2 features with errors)").arg(file).arg(errors));
    }
    return true;
}

void AppController::newProject()
{
    const QString previousAutosave = autosavePath();
    const QVariantMap r = execute(QStringLiteral("project.new"));
    if (!r.value(QStringLiteral("ok")).toBool())
        return;
    removeAutosave(previousAutosave);
    setStatusMessage(tr("New design"));
}

bool AppController::saveProject(const QVariantMap& view)
{
    if (m_session.project().path.empty())
        return false;
    return saveTo(QString(), view);
}

bool AppController::saveProjectAs(const QUrl& url, const QVariantMap& view)
{
    QString path = toLocalPath(url);
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".") + kProjectExtension;
    return saveTo(path, view);
}

bool AppController::saveTo(const QString& path, const QVariantMap& view)
{
    const QString previousAutosave = autosavePath();
    QVariantMap params{{QStringLiteral("view"), view}};
    if (!path.isEmpty())
        params.insert(QStringLiteral("path"), path);
    const QVariantMap r = execute(QStringLiteral("project.save"), params);
    if (!r.value(QStringLiteral("ok")).toBool())
        return false;
    const QString saved = r.value(QStringLiteral("result")).toMap().value(QStringLiteral("savedTo")).toString();
    removeAutosave(previousAutosave);
    removeAutosave(autosavePath());
    addRecent(saved);
    setStatusMessage(tr("Saved %1").arg(QDir::toNativeSeparators(saved)));
    emit projectChanged();
    return true;
}

QString AppController::autosaveDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/autosave");
}

QString AppController::autosavePath() const
{
    const QString path = QString::fromStdString(m_session.project().path);
    QString name;
    if (path.isEmpty()) {
        name = QStringLiteral("Untitled-%1").arg(QCoreApplication::applicationPid());
    } else {
        const QByteArray hash = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(8);
        name = QFileInfo(path).completeBaseName() + QLatin1Char('-') + QString::fromLatin1(hash);
    }
    return autosaveDir() + QLatin1Char('/') + name + QStringLiteral(".") + kProjectExtension;
}

bool AppController::autosaveNow()
{
    // Saving ends a live feature preview; try again on the next tick instead.
    if (!m_session.project().dirty() || m_session.previewActive())
        return false;
    const QString target = autosavePath();
    if (!QDir().mkpath(autosaveDir()))
        return false;
    const QString lockPath = target + QStringLiteral(".lock");
    if (!m_autosaveLock || m_autosaveLock->fileName() != lockPath) {
        m_autosaveLock = std::make_unique<QLockFile>(lockPath);
        m_autosaveLock->setStaleLockTime(0);
        if (!m_autosaveLock->tryLock(0)) {
            m_autosaveLock.reset();
            return false;
        }
    }
    commands::Json params{{"path", target.toStdString()}, {"copy", true}};
    if (m_viewProvider.isCallable()) {
        const QJSValue view = m_viewProvider.call();
        if (view.isObject() && !view.isError())
            params["view"] = toJson(view.toVariant());
    }
    const commands::CommandResult r = m_session.execute("project.save", params);
    if (!r.ok) {
        setStatusMessage(tr("Autosave failed: %1").arg(QString::fromStdString(r.error)));
        return false;
    }
    setStatusMessage(tr("Recovery copy saved"));
    return true;
}

bool AppController::hasAutosave() const
{
    return QFileInfo::exists(autosavePath());
}

QStringList AppController::autosaveFiles() const
{
    QDir dir(autosaveDir());
    QStringList out;
    const auto entries = dir.entryInfoList({QStringLiteral("*.") + kProjectExtension}, QDir::Files, QDir::Time);
    for (const QFileInfo& info : entries) {
        // Skip copies owned by a running AeroCAD (another window / instance).
        QLockFile lock(info.absoluteFilePath() + QStringLiteral(".lock"));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0))
            continue;
        lock.unlock();
        out << info.absoluteFilePath();
    }
    return out;
}

bool AppController::recoverAutosave(const QString& path)
{
    return openProject(path, true);
}

QVariantMap AppController::autosaveInfo(const QString& path) const
{
    // Copies are named <project>-<8 hex digits of the path hash> or Untitled-<pid> (autosavePath()).
    const QFileInfo info(path);
    QString name = info.completeBaseName();
    static const QRegularExpression untitled(QStringLiteral("^Untitled-\\d+$"));
    static const QRegularExpression hashed(QStringLiteral("-[0-9a-f]{8}$"));
    if (untitled.match(name).hasMatch())
        name.clear();
    else
        name.remove(hashed);
    return {{QStringLiteral("name"), name},
            {QStringLiteral("saved"), QLocale().toString(info.lastModified(), QStringLiteral("d MMM yyyy HH:mm"))}};
}

void AppController::discardAutosaves(bool currentOnly)
{
    if (currentOnly) {
        removeAutosave(autosavePath());
        return;
    }
    for (const QString& file : autosaveFiles())
        removeAutosave(file);
}

void AppController::removeAutosave(const QString& path)
{
    const QString lockPath = path + QStringLiteral(".lock");
    if (m_autosaveLock && m_autosaveLock->fileName() == lockPath) {
        QFile::remove(path);
        m_autosaveLock.reset(); // unlocks and deletes the lock file
        return;
    }
    // Owned by another running instance (same project open twice): leave it alone.
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return;
    QFile::remove(path);
    lock.unlock();
}

void AppController::addRecent(const QString& path)
{
    const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    m_recent.removeAll(native);
    m_recent.prepend(native);
    while (m_recent.size() > kMaxRecent)
        m_recent.removeLast();
    QSettings().setValue(QStringLiteral("project/recent"), m_recent);
    emit recentProjectsChanged();
}

void AppController::clearRecentProjects()
{
    m_recent.clear();
    QSettings().setValue(QStringLiteral("project/recent"), m_recent);
    emit recentProjectsChanged();
}

QVariantMap AppController::partInfo(quint64 id) const
{
    const model::Part* p = part(id);
    if (p == nullptr)
        return {};
    const core::BoundingBox box = p->mesh ? p->mesh->bounds() : core::BoundingBox{};
    const core::Vec3 size = box.size();
    return {
        {QStringLiteral("id"), QVariant::fromValue<quint64>(p->id)},
        {QStringLiteral("name"), QString::fromStdString(p->name)},
        {QStringLiteral("source"), QString::fromStdString(p->source)},
        {QStringLiteral("importUnits"), QString::fromStdString(p->importUnits)},
        {QStringLiteral("visible"), p->visible},
        {QStringLiteral("color"), QString::fromStdString(p->color)},
        {QStringLiteral("vertices"), QVariant::fromValue<qulonglong>(p->mesh ? p->mesh->vertexCount() : 0)},
        {QStringLiteral("triangles"), QVariant::fromValue<qulonglong>(p->mesh ? p->mesh->triangleCount() : 0)},
        {QStringLiteral("hasBrep"), p->shape.has_value()},
        {QStringLiteral("kind"), p->kind == model::PartKind::Body     ? QStringLiteral("body")
                                 : p->kind == model::PartKind::Sketch ? QStringLiteral("sketch")
                                 : p->kind == model::PartKind::Plane  ? QStringLiteral("plane")
                                                                      : QStringLiteral("imported")},
        {QStringLiteral("featureId"), QVariant::fromValue<qulonglong>(p->featureId)},
        {QStringLiteral("faces"), QVariant::fromValue<qulonglong>(p->shape ? p->shape->faceCount() : 0)},
        {QStringLiteral("solids"), QVariant::fromValue<qulonglong>(p->shape ? p->shape->solidCount() : 0)},
        {QStringLiteral("sizeX"), size.x},
        {QStringLiteral("sizeY"), size.y},
        {QStringLiteral("sizeZ"), size.z},
        {QStringLiteral("diagonal"), box.diagonal()},
        {QStringLiteral("center"), box.isEmpty() ? QVariantList{} : QVariantList{box.center().x, box.center().y,
                                                                                    box.center().z}},
        {QStringLiteral("selectedTriangles"), QVariant::fromValue<qulonglong>(p->selection ? p->selection->size() : 0)},
        {QStringLiteral("selectedArea"), p->mesh && p->selection ? core::selectionArea(*p->mesh, *p->selection) : 0.0},
    };
}

bool AppController::brushBegin(quint64 id, double radius, bool connected, const QString& mode)
{
    brushCancel();
    const model::Part* p = part(id);
    if (p == nullptr || !p->mesh || p->mesh->isEmpty() || radius <= 0.0)
        return false;
    BrushStroke b;
    b.partId = id;
    b.radius = radius;
    b.connected = connected;
    b.mode = mode == QStringLiteral("subtract") || mode == QStringLiteral("replace") ? mode : QStringLiteral("add");
    b.mesh = p->mesh;
    if (connected)
        b.adjacency = core::adjacencyFor(p->mesh);
    if (p->selection)
        b.base = *p->selection;
    b.preview = b.base;
    m_brush = std::move(b);
    return true;
}

void AppController::brushAdd(const QVector3D& point)
{
    if (!m_brush)
        return;
    BrushStroke& b = *m_brush;
    b.points.push_back(point);
    const core::TriangleSet dab =
        core::selectBrushDab(*b.mesh, b.adjacency.get(), {point.x(), point.y(), point.z()}, b.radius);
    b.stroke = core::combineSelection(b.stroke, dab, core::SelectionMode::Add);
    core::SelectionMode mode = core::SelectionMode::Add;
    if (b.mode == QStringLiteral("subtract"))
        mode = core::SelectionMode::Subtract;
    else if (b.mode == QStringLiteral("replace"))
        mode = core::SelectionMode::Replace;
    b.preview = core::combineSelection(b.base, b.stroke, mode);
    emit selectionPreviewChanged(b.partId);
}

QVariantMap AppController::brushEnd()
{
    if (!m_brush)
        return {};
    BrushStroke b = std::move(*m_brush);
    m_brush.reset();
    if (b.points.isEmpty()) {
        emit selectionPreviewChanged(b.partId);
        return {};
    }
    QVariantList points;
    for (const auto& p : std::as_const(b.points))
        points.push_back(QVariantList{p.x(), p.y(), p.z()});
    const QVariantMap r =
        execute(QStringLiteral("selection.brush"), {{QStringLiteral("id"), QVariant::fromValue<quint64>(b.partId)},
                                                    {QStringLiteral("points"), points},
                                                    {QStringLiteral("radius"), b.radius},
                                                    {QStringLiteral("connected"), b.connected},
                                                    {QStringLiteral("mode"), b.mode}});
    emit selectionPreviewChanged(b.partId); // back to the stored selection
    return r;
}

void AppController::brushCancel()
{
    if (!m_brush)
        return;
    const quint64 id = m_brush->partId;
    m_brush.reset();
    emit selectionPreviewChanged(id);
}

QVariantMap AppController::pickSurface(const QVector3D& origin, const QVector3D& direction,
                                       const QVariantMap& options) const
{
    const PickFilter filter = pickFilter(options);
    const quint64 skipId = options.value(QStringLiteral("skipId")).toULongLong();
    const quint64 onlyId = options.value(QStringLiteral("onlyId")).toULongLong();
    const core::Vec3 o{origin.x(), origin.y(), origin.z()};
    const core::Vec3 d{direction.x(), direction.y(), direction.z()};
    std::optional<core::RayHit> best;
    model::PartId bestPart = 0;
    for (const auto& p : m_session.document().parts()) {
        if (!filter.accepts(p) || !p.mesh || (skipId != 0 && p.id == skipId) || (onlyId != 0 && p.id != onlyId))
            continue;
        std::optional<core::RayHit> hit;
        const commands::SectionAnalysis& section = m_session.inspect().section();
        if (p.kind != model::PartKind::Sketch && section.cuts(p.id)) {
            // Section view: only the kept half is drawn, so only it can be picked.
            const core::Vec3 n = section.plane.normal.normalized();
            const double s = section.plane.distance(o);
            const double ds = d.dot(n); // change of the plane distance per unit t
            if (s > 0.0) {
                if (ds < 0.0) {
                    const double t0 = s / -ds; // where the ray enters the kept half
                    hit = m_pickCache.raycast(p.mesh, o + d * t0, d);
                    if (hit)
                        hit->distance += t0;
                }
            } else {
                hit = m_pickCache.raycast(p.mesh, o, d);
                if (hit && section.plane.distance(hit->point) > 1e-9) {
                    hit.reset();
                }
            }
        } else {
            hit = m_pickCache.raycast(p.mesh, o, d);
        }
        if (hit && (!best || hit->distance < best->distance)) {
            best = hit;
            bestPart = p.id;
        }
    }
    if (!best)
        return {};
    return {{QStringLiteral("partId"), QVariant::fromValue<quint64>(bestPart)},
            {QStringLiteral("point"), QVector3D(static_cast<float>(best->point.x), static_cast<float>(best->point.y),
                                                static_cast<float>(best->point.z))},
            {QStringLiteral("triangle"), QVariant::fromValue<quint32>(best->triangle)}};
}

QVariantMap AppController::pickNearSurface(const QVector3D& origin, const QVector3D& direction,
                                           double maxAngleDeg, const QVariantMap& options) const
{
    const PickFilter filter = pickFilter(options);
    const core::Vec3 o{origin.x(), origin.y(), origin.z()};
    const core::Vec3 d{direction.x(), direction.y(), direction.z()};
    const double len = d.length();
    if (!(len > 0.0))
        return {};
    std::optional<core::RayHit> best;
    double bestAngle = 0.0;
    model::PartId bestPart = 0;
    for (const auto& p : m_session.document().parts()) {
        if (!filter.accepts(p) || !p.mesh)
            continue;
        // Section view: only the kept half is drawn, so only it can be snapped to (CAD-96).
        const commands::SectionAnalysis& section = m_session.inspect().section();
        std::function<bool(const core::Vec3&)> kept;
        if (p.kind != model::PartKind::Sketch && section.cuts(p.id))
            kept = [&section](const core::Vec3& v) { return section.plane.distance(v) <= 1e-9; };
        const auto hit = core::nearestToRay(*p.mesh, o, d, maxAngleDeg, kept);
        if (!hit)
            continue;
        const core::Vec3 w = hit->point - o;
        const double along = w.dot(d) / len;
        const double angle = std::atan2((w - d * (along / len)).length(), along);
        if (!best || angle < bestAngle) {
            best = hit;
            bestAngle = angle;
            bestPart = p.id;
        }
    }
    if (!best)
        return {};
    return {{QStringLiteral("partId"), QVariant::fromValue<quint64>(bestPart)},
            {QStringLiteral("point"), QVector3D(static_cast<float>(best->point.x), static_cast<float>(best->point.y),
                                                static_cast<float>(best->point.z))},
            {QStringLiteral("triangle"), QVariant::fromValue<quint32>(best->triangle)},
            {QStringLiteral("snapped"), true}};
}

QVariantMap AppController::pickSketchLine(const QVector3D& origin, const QVector3D& direction,
                                          double maxDistance, const QVariantMap& options) const
{
    const PickFilter filter = pickFilter(options);
    const core::Vec3 o{origin.x(), origin.y(), origin.z()};
    core::Vec3 d{direction.x(), direction.y(), direction.z()};
    if (!(d.length() > 0.0) || !(maxDistance > 0.0))
        return {};
    d = d.normalized();
    double bestGap = maxDistance;
    double bestT = 0.0;
    core::Vec3 bestPoint{};
    model::PartId bestPart = 0;
    for (const auto& p : m_session.document().parts()) {
        if (!filter.accepts(p) || p.kind != model::PartKind::Sketch)
            continue;
        const auto& segs = p.edgeSegments;
        for (std::size_t i = 0; i + 1 < segs.size(); i += 2) {
            // Closest points of the ray o + t d and the segment a + s (b - a), s in [0, 1].
            const core::Vec3 a = segs[i];
            const core::Vec3 u = segs[i + 1] - a;
            const core::Vec3 w = a - o;
            const double uu = u.dot(u);
            const double ud = u.dot(d);
            const double denom = uu - ud * ud;
            double sParam = 0.0;
            if (uu > 0.0)
                sParam = denom > 1e-12 * uu ? (ud * w.dot(d) - w.dot(u)) / denom : 0.0;
            sParam = std::clamp(sParam, 0.0, 1.0);
            const core::Vec3 q = a + u * sParam;
            const double t = (q - o).dot(d);
            if (t <= 0.0)
                continue;
            const double gap = (q - (o + d * t)).length();
            // The nearest line to the ray; between equally near ones the one in front.
            if (gap < bestGap - 1e-9 || (bestPart != 0 && gap <= bestGap + 1e-9 && t < bestT)) {
                bestGap = gap;
                bestT = t;
                bestPoint = q;
                bestPart = p.id;
            }
        }
    }
    if (bestPart == 0)
        return {};
    return {{QStringLiteral("partId"), QVariant::fromValue<quint64>(bestPart)},
            {QStringLiteral("point"), QVector3D(static_cast<float>(bestPoint.x), static_cast<float>(bestPoint.y),
                                                static_cast<float>(bestPoint.z))},
            {QStringLiteral("distance"), bestT}};
}

const core::MeshProjector& AppController::projectorFor(const std::shared_ptr<const core::TriangleMesh>& mesh,
                                                       const core::Vec3& outward) const
{
    // Built on the first use for a mesh and direction; clicks and hover then cost microseconds.
    if (!m_projector || m_projectorMesh != mesh || (m_projector->outward() - outward.normalized()).length() > 1e-9) {
        m_projectorMesh = mesh;
        m_projector = std::make_unique<core::MeshProjector>(*mesh, outward);
    }
    return *m_projector;
}

QVector3D AppController::dropOntoPart(quint64 id, const QVector3D& point, const QVector3D& normal) const
{
    const core::Vec3 p{point.x(), point.y(), point.z()};
    const core::Vec3 n{normal.x(), normal.y(), normal.z()};
    const model::Part* surface = part(id);
    if (surface == nullptr || !(n.length() > 0.0))
        return point;
    std::vector<std::shared_ptr<const core::TriangleMesh>> meshes;
    if (const model::Part* scan = part(model::fittedScanId(*surface)); scan != nullptr && scan->mesh && !scan->shape)
        meshes.push_back(scan->mesh);
    if (surface->mesh)
        meshes.push_back(surface->mesh);
    for (const auto& mesh : meshes) {
        if (const auto hit = projectorFor(mesh, n).drop(p))
            return {static_cast<float>(hit->x), static_cast<float>(hit->y), static_cast<float>(hit->z)};
    }
    return point;
}

QVariantMap AppController::pickPartSurface(quint64 id, const QVector3D& origin, const QVector3D& direction) const
{
    const core::Vec3 o{origin.x(), origin.y(), origin.z()};
    const core::Vec3 d{direction.x(), direction.y(), direction.z()};
    const model::Part* p = part(id);
    if (p == nullptr)
        return {};
    auto cast = [&](const std::shared_ptr<const core::TriangleMesh>& mesh) {
        // Any direction serves for ray casts; reuse the one dropOntoPart built.
        return (m_projector && m_projectorMesh == mesh ? *m_projector : projectorFor(mesh, d)).raycast(o, d);
    };
    std::optional<core::RayHit> hit;
    // A fitted surface covers only the fitted area; the scan it came from goes further.
    // Hidden too: a sketch is laid on it either way (Fit hides it).
    if (const model::Part* scan = part(model::fittedScanId(*p)); scan != nullptr && scan->mesh && !scan->shape)
        hit = cast(scan->mesh);
    if (!hit && p->mesh)
        hit = cast(p->mesh);
    if (!hit)
        return {};
    return {{QStringLiteral("partId"), QVariant::fromValue<quint64>(id)},
            {QStringLiteral("point"), QVector3D(static_cast<float>(hit->point.x), static_cast<float>(hit->point.y),
                                                static_cast<float>(hit->point.z))}};
}

const core::TriangleSet* AppController::displayedSelection(quint64 id) const
{
    if (m_brush && m_brush->partId == id)
        return &m_brush->preview;
    const model::Part* p = part(id);
    return p != nullptr && p->selection ? p->selection.get() : nullptr;
}

const model::Part* AppController::part(quint64 id) const
{
    return m_session.document().part(id);
}

void AppController::setStatusMessage(const QString& message)
{
    if (m_statusMessage == message)
        return;
    m_statusMessage = message;
    emit statusMessageChanged();
}

QVariantList AppController::colormap(const QString& scalar, const QString& palette) const
{
    QVariantList out;
    for (int i = 0; i <= 16; ++i)
        out.append(FieldGeometry::mapColor(scalar, i / 16.0, palette));
    return out;
}

double AppController::fieldScalePosition(const QString& scalar, double value, double lo, double hi) const
{
    return FieldGeometry::scalePosition(scalar, value, lo, hi);
}

} // namespace aerocad::app
