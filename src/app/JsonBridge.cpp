// SPDX-License-Identifier: GPL-3.0-or-later
#include "JsonBridge.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace aerocad::app {

nlohmann::json toJson(const QVariant& value)
{
    if (!value.isValid() || value.isNull())
        return nlohmann::json::object();
    const QJsonValue json = QJsonValue::fromVariant(value);
    // Wrap so scalars survive the QJsonDocument round trip.
    QJsonObject wrapper{{QStringLiteral("v"), json}};
    const QByteArray bytes = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
    return nlohmann::json::parse(bytes.constData(), bytes.constData() + bytes.size()).at("v");
}

QVariant toVariant(const nlohmann::json& value)
{
    const std::string text = nlohmann::json{{"v", value}}.dump();
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(text));
    return doc.object().value(QStringLiteral("v")).toVariant();
}

} // namespace aerocad::app
