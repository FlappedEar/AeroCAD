// SPDX-License-Identifier: GPL-3.0-or-later
#include "AppController.hpp"

#include "JsonBridge.hpp"

#include "aerocad/core/Path.hpp"
#include "aerocad/io/Import.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace aerocad::app {

namespace {

constexpr int kMaxRecent = 10;
const QString kProjectExtension = QStringLiteral("aerocad");

QString toLocalPath(const QUrl& url)
{
    return QDir::toNativeSeparators(url.isLocalFile() ? url.toLocalFile() : url.toString());
}

} // namespace

AppController::AppController(QObject* parent) : QObject(parent), m_parts(m_session.document())
{
    m_session.project().generator = "AeroCAD " AEROCAD_VERSION;
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
        emit partChanged(id);
        emit boundsChanged();
    });
}

QVector3D AppController::sceneCenter() const
{
    const core::Vec3 c = m_session.document().bounds().center();
    // Z-up model -> Y-up scene: (x, y, z) -> (x, z, -y).
    return {static_cast<float>(c.x), static_cast<float>(c.z), static_cast<float>(-c.y)};
}

double AppController::sceneRadius() const
{
    return m_session.document().bounds().diagonal() * 0.5;
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
    return QStringLiteral(AEROCAD_VERSION);
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

QVariantMap AppController::execute(const QString& name, const QVariantMap& params)
{
    const bool hadPreview = m_session.previewActive();
    const commands::CommandResult result = m_session.execute(name.toStdString(), toJson(params));
    const auto* info = m_session.registry().info(name.toStdString());
    // A command that ended a live preview (e.g. project.save) also changed the document
    // back; the feature dialog rebuilds its preview on historyChanged.
    if ((info != nullptr && info->modifiesDocument) || (hadPreview && !m_session.previewActive()))
        emit historyChanged();
    emit projectChanged();

    if (!result.ok)
        setStatusMessage(QString::fromStdString(result.error));
    return toVariant(result.toJson()).toMap();
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

QVariantMap AppController::exportFile(const QUrl& url, const QVariantList& ids)
{
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    QVariantMap params{{QStringLiteral("path"), QDir::toNativeSeparators(path)}};
    if (!ids.isEmpty())
        params.insert(QStringLiteral("ids"), ids);
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
    for (const auto& step : script) {
        if (!step.is_object() || !step.contains("name") || !step["name"].is_string()) {
            setStatusMessage(tr("Script step is not an object with a string \"name\": %1")
                                 .arg(QString::fromStdString(step.dump())));
            ok = false;
            break;
        }
        const std::string name = step["name"].get<std::string>();
        const commands::CommandResult r = m_session.execute(name, step.value("params", commands::Json::object()));
        if (!r.ok) {
            setStatusMessage(tr("Script step %1 failed: %2")
                                 .arg(QString::fromStdString(name), QString::fromStdString(r.error)));
            ok = false;
            break;
        }
    }
    emit historyChanged();
    emit projectChanged();
    emit scriptFinished();
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
    emit historyChanged();
    emit projectOpened(data.value(QStringLiteral("view")).toMap());
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
    emit historyChanged();
    emit projectOpened({});
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
    const commands::CommandResult r = m_session.execute(
        "project.save", {{"path", target.toStdString()}, {"copy", true}});
    if (!r.ok) {
        setStatusMessage(tr("Autosave failed: %1").arg(QString::fromStdString(r.error)));
        return false;
    }
    setStatusMessage(tr("Recovery copy saved"));
    return true;
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
        {QStringLiteral("visible"), p->visible},
        {QStringLiteral("vertices"), QVariant::fromValue<qulonglong>(p->mesh ? p->mesh->vertexCount() : 0)},
        {QStringLiteral("triangles"), QVariant::fromValue<qulonglong>(p->mesh ? p->mesh->triangleCount() : 0)},
        {QStringLiteral("hasBrep"), p->shape.has_value()},
        {QStringLiteral("kind"), p->kind == model::PartKind::Body     ? QStringLiteral("body")
                                 : p->kind == model::PartKind::Sketch ? QStringLiteral("sketch")
                                                                      : QStringLiteral("imported")},
        {QStringLiteral("featureId"), QVariant::fromValue<qulonglong>(p->featureId)},
        {QStringLiteral("faces"), QVariant::fromValue<qulonglong>(p->shape ? p->shape->faceCount() : 0)},
        {QStringLiteral("solids"), QVariant::fromValue<qulonglong>(p->shape ? p->shape->solidCount() : 0)},
        {QStringLiteral("sizeX"), size.x},
        {QStringLiteral("sizeY"), size.y},
        {QStringLiteral("sizeZ"), size.z},
        {QStringLiteral("diagonal"), box.diagonal()},
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

QVariantMap AppController::pickSurface(const QVector3D& origin, const QVector3D& direction) const
{
    const core::Vec3 o{origin.x(), origin.y(), origin.z()};
    const core::Vec3 d{direction.x(), direction.y(), direction.z()};
    std::optional<core::RayHit> best;
    model::PartId bestPart = 0;
    for (const auto& p : m_session.document().parts()) {
        if (!p.visible || !p.mesh)
            continue;
        const auto hit = core::raycast(*p.mesh, o, d);
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

} // namespace aerocad::app
