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

QString AppController::version() const
{
    return QStringLiteral(AEROCAD_VERSION);
}

QVariantMap AppController::execute(const QString& name, const QVariantMap& params)
{
    const commands::CommandResult result = m_session.execute(name.toStdString(), toJson(params));
    const auto* info = m_session.registry().info(name.toStdString());
    if (info != nullptr && info->modifiesDocument)
        emit historyChanged();
    emit projectChanged();

    if (!result.ok)
        setStatusMessage(QString::fromStdString(result.error));
    return toVariant(result.toJson()).toMap();
}

bool AppController::importFile(const QUrl& url)
{
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    const QVariantMap r = execute(QStringLiteral("document.import"),
                                  {{QStringLiteral("path"), QDir::toNativeSeparators(path)}});
    if (!r.value(QStringLiteral("ok")).toBool())
        return false;

    const QVariantMap data = r.value(QStringLiteral("result")).toMap();
    setStatusMessage(tr("Imported %1 (%2, %3 triangles)")
                         .arg(QFileInfo(path).fileName(), data.value(QStringLiteral("format")).toString(),
                              QLocale().toString(data.value(QStringLiteral("triangles")).toLongLong())));
    emit partImported(data.value(QStringLiteral("id")).toULongLong());
    return true;
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
    if (!m_session.project().dirty())
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
        {QStringLiteral("sizeX"), size.x},
        {QStringLiteral("sizeY"), size.y},
        {QStringLiteral("sizeZ"), size.z},
    };
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
