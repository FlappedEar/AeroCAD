// SPDX-License-Identifier: GPL-3.0-or-later
#include "PartListModel.hpp"

#include <QString>

namespace aerocad::app {

PartListModel::PartListModel(model::Document& document, QObject* parent)
    : QAbstractListModel(parent), m_document(document)
{
    m_observer = m_document.addObserver([this](model::Document::Change c, model::PartId id) { onDocumentChanged(c, id); });
}

PartListModel::~PartListModel()
{
    m_document.removeObserver(m_observer);
}

int PartListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_document.partCount());
}

QVariant PartListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount())
        return {};
    const model::Part& p = m_document.parts()[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return QString::fromStdString(p.name);
    case PartIdRole:
        return QVariant::fromValue<quint64>(p.id);
    case VisibleRole:
        return p.visible;
    case TrianglesRole:
        return QVariant::fromValue<qulonglong>(p.mesh ? p.mesh->triangleCount() : 0);
    case HasBrepRole:
        return p.shape.has_value();
    case SourceRole:
        return QString::fromStdString(p.source);
    case RevisionRole:
        return QVariant::fromValue<qulonglong>(p.revision);
    case KindRole:
        switch (p.kind) {
        case model::PartKind::Body: return QStringLiteral("body");
        case model::PartKind::Sketch: return QStringLiteral("sketch");
        case model::PartKind::Imported: break;
        }
        return QStringLiteral("imported");
    case FeatureIdRole:
        return QVariant::fromValue<qulonglong>(p.featureId);
    case ShapeKindRole:
        // Exact geometry of an imported/derived part: "solid" or "surface" (e.g. a fitted
        // surface); "" for a plain mesh.
        if (!p.shape || p.shape->isNull())
            return QString();
        return p.shape->solidCount() > 0 ? QStringLiteral("solid") : QStringLiteral("surface");
    default:
        return {};
    }
}

QHash<int, QByteArray> PartListModel::roleNames() const
{
    return {
        {PartIdRole, "partId"},       {NameRole, "name"},       {VisibleRole, "partVisible"},
        {TrianglesRole, "triangles"}, {HasBrepRole, "hasBrep"}, {SourceRole, "source"},
        {RevisionRole, "revision"},   {KindRole, "kind"},       {FeatureIdRole, "featureId"},
        {ShapeKindRole, "shapeKind"},
    };
}

int PartListModel::rowOf(quint64 partId) const
{
    const auto& parts = m_document.parts();
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].id == partId)
            return static_cast<int>(i);
    }
    return -1;
}

void PartListModel::onDocumentChanged(model::Document::Change change, model::PartId id)
{
    using Change = model::Document::Change;
    switch (change) {
    case Change::AboutToAdd: {
        const int row = rowCount(); // new parts are appended
        beginInsertRows({}, row, row);
        break;
    }
    case Change::Added:
        endInsertRows();
        emit countChanged();
        break;
    case Change::AboutToRemove: {
        const int row = rowOf(id);
        beginRemoveRows({}, row, row);
        break;
    }
    case Change::Removed:
        endRemoveRows();
        emit countChanged();
        break;
    case Change::Modified: {
        const int row = rowOf(id);
        if (row >= 0)
            emit dataChanged(index(row), index(row));
        break;
    }
    }
}

} // namespace aerocad::app
