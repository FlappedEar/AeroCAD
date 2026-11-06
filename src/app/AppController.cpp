// SPDX-License-Identifier: GPL-3.0-or-later
#include "AppController.hpp"

#include "JsonBridge.hpp"

#include "aerocad/core/Path.hpp"
#include "aerocad/io/Import.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>

namespace aerocad::app {

AppController::AppController(QObject* parent) : QObject(parent), m_parts(m_session.document())
{
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

QVariantMap AppController::execute(const QString& name, const QVariantMap& params)
{
    const commands::CommandResult result = m_session.execute(name.toStdString(), toJson(params));
    const auto* info = m_session.registry().info(name.toStdString());
    if (info != nullptr && info->modifiesDocument)
        emit historyChanged();

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
    emit scriptFinished();
    return ok;
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
