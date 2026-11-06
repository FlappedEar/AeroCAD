// SPDX-License-Identifier: GPL-3.0-or-later
#include "AppController.hpp"

#include "JsonBridge.hpp"

#include "aerocad/core/Path.hpp"
#include "aerocad/io/Import.hpp"

#include <QDir>
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
    b.mode = mode == QStringLiteral("subtract") ? mode : QStringLiteral("add");
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
    b.preview = core::combineSelection(b.base, b.stroke,
                                       b.mode == QStringLiteral("subtract") ? core::SelectionMode::Subtract
                                                                            : core::SelectionMode::Add);
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
