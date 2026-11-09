// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/model/Document.hpp"

#include <QAbstractListModel>
#include <QtQml/qqmlregistration.h>

namespace aerocad::app {

/// List of document parts for the parts panel and the viewport's Repeater3D.
class PartListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by AppController")
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        PartIdRole = Qt::UserRole + 1,
        NameRole,
        VisibleRole,
        TrianglesRole,
        HasBrepRole,
        SourceRole,
        RevisionRole,
        KindRole,      ///< "imported", "body" or "sketch"
        FeatureIdRole, ///< Timeline feature of a body / sketch (0 for imported parts)
        ShapeKindRole, ///< "surface" / "solid" for parts with exact geometry, "" for meshes
        ColorRole,     ///< Appearance colour "#rrggbb", "" = default
    };
    Q_ENUM(Role)

    explicit PartListModel(model::Document& document, QObject* parent = nullptr);
    ~PartListModel() override;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    /// Row of a part id, or -1.
    Q_INVOKABLE int rowOf(quint64 partId) const;

signals:
    void countChanged();

private:
    void onDocumentChanged(model::Document::Change change, model::PartId id);

    model::Document& m_document;
    model::Document::ObserverId m_observer = 0;
};

} // namespace aerocad::app
