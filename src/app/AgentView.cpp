// SPDX-License-Identifier: GPL-3.0-or-later
#include "AgentView.hpp"

#include "AppController.hpp"
#include "JsonBridge.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QImage>
#include <QJSValue>
#include <QQuickItem>
#include <QQuickWindow>

#include <algorithm>
#include <cmath>

namespace aerocad::app {

namespace {

using Json = nlohmann::json;

/// A QML function's return value as JSON (JS objects arrive as QJSValue).
Json fromQml(const QVariant& value)
{
    if (value.metaType() == QMetaType::fromType<QJSValue>())
        return toJson(value.value<QJSValue>().toVariant(QJSValue::ConvertJSObjects));
    return toJson(value);
}

QQuickItem* viewportItem(QQuickWindow* window)
{
    return window->findChild<QQuickItem*>(QStringLiteral("viewport"));
}

/// Checks the framing parameters shared by view.set and view.screenshot.
void checkFraming(const Json& p)
{
    static const char* const allowed[] = {"direction", "part", "center", "radius", "fit", "orthographic",
                                          "area", "maxSize"};
    for (auto it = p.begin(); it != p.end(); ++it)
        if (std::none_of(std::begin(allowed), std::end(allowed), [&](const char* a) { return it.key() == a; }))
            throw commands::CommandError("Unknown parameter '" + it.key() + "'");
    if (p.contains("direction")) {
        const Json& d = p["direction"];
        static const char* const names[] = {"front", "back", "right", "left", "top", "bottom", "iso", "home"};
        if (d.is_string()) {
            const auto s = d.get<std::string>();
            if (std::none_of(std::begin(names), std::end(names), [&](const char* n) { return s == n; }))
                throw commands::CommandError("direction '" + s +
                                             "' is not one of front, back, right, left, top, bottom, iso, home");
        } else if (!(d.is_array() && d.size() == 3 && d[0].is_number() && d[1].is_number() && d[2].is_number()) ||
                   std::hypot(d[0].get<double>(), d[1].get<double>(), d[2].get<double>()) < 1e-9) {
            throw commands::CommandError("direction must be a name (front, top, iso, ...) or a non-zero [x, y, z]");
        }
    }
    if (p.contains("center") && !(p["center"].is_array() && p["center"].size() == 3))
        throw commands::CommandError("center must be [x, y, z] in mm");
    if (p.contains("center") && !p.contains("radius"))
        throw commands::CommandError("center needs radius (mm): the size of the area to show around it");
    if (p.contains("part") && !p["part"].is_number_integer())
        throw commands::CommandError("part must be a part id (project_get_context)");
}

Json setView(QQuickWindow* window, const Json& params)
{
    QVariant ret;
    if (!QMetaObject::invokeMethod(window, "agentSetView", Q_RETURN_ARG(QVariant, ret),
                                   Q_ARG(QVariant, toVariant(params))))
        throw commands::CommandError("The 3D view is not available");
    Json out = fromQml(ret);
    if (out.is_object() && out.contains("error"))
        throw commands::CommandError(out["error"].get<std::string>());
    return out;
}

Json visibleParts(AppController& controller)
{
    Json parts = Json::array();
    for (const auto& p : controller.session().document().parts())
        if (p.visible && p.mesh && !p.mesh->isEmpty())
            parts.push_back({{"id", p.id}, {"name", p.name}});
    return parts;
}

Json screenshot(QQuickWindow* window, AppController& controller, const Json& params)
{
    Json camera = setView(window, params);
    // Let the camera, bindings and clipping settle, then render: grabWindow() renders a new
    // frame from the current scene.
    QCoreApplication::processEvents();
    QImage image = window->grabWindow();
    if (image.isNull())
        throw commands::CommandError("Could not capture the AeroCAD window");
    const std::string area = params.value("area", std::string("viewport"));
    if (area == "viewport") {
        QQuickItem* vp = viewportItem(window);
        if (vp == nullptr)
            throw commands::CommandError("The 3D view is not available");
        const qreal dpr = image.devicePixelRatio() > 0 ? image.devicePixelRatio() : window->effectiveDevicePixelRatio();
        const QPointF origin = vp->mapToScene(QPointF(0, 0));
        const QRect rect(qRound(origin.x() * dpr), qRound(origin.y() * dpr), qRound(vp->width() * dpr),
                         qRound(vp->height() * dpr));
        image = image.copy(rect.intersected(image.rect()));
    }
    const int maxSize = std::clamp(params.value("maxSize", 1024), 200, 2000);
    if (std::max(image.width(), image.height()) > maxSize)
        image = image.scaled(maxSize, maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    Json out{{"area", area},
             {"width", image.width()},
             {"height", image.height()},
             {"camera", camera},
             {"visibleParts", visibleParts(controller)},
             {"image", {{"mimeType", "image/png"}, {"data", bytes.toBase64().toStdString()}}}};
    if (area == "viewport")
        out["pick"] = "view_pick {points: [[x, y]]} with x, y = pixel / image size gives the 3D point there.";
    const QString status = controller.statusMessage();
    if (!status.isEmpty())
        out["statusMessage"] = status.toStdString();
    return out;
}

Json pick(QQuickWindow* window, AppController& controller, const Json& params)
{
    if (!params.contains("points") || !params["points"].is_array() || params["points"].empty())
        throw commands::CommandError("points: give one or more [x, y] image positions (0..1)");
    for (const auto& p : params["points"])
        if (!(p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number()) || p[0].get<double>() < 0 ||
            p[0].get<double>() > 1 || p[1].get<double>() < 0 || p[1].get<double>() > 1)
            throw commands::CommandError("Each point is [x, y] with 0 <= x, y <= 1 (fractions of the image size)");
    QVariant ret;
    if (!QMetaObject::invokeMethod(window, "agentPick", Q_RETURN_ARG(QVariant, ret),
                                   Q_ARG(QVariant, toVariant(params["points"]))))
        throw commands::CommandError("The 3D view is not available");
    Json hits = fromQml(ret);
    if (!hits.is_array())
        throw commands::CommandError("Pick failed");
    // The surface normal, from the same closest-point query geometry.measure uses.
    for (auto& h : hits) {
        if (!h.is_object())
            continue;
        const auto r = controller.session().execute(
            "geometry.measure", {{"kind", "point_to_part"}, {"id", h.at("part")}, {"point", h.at("point")}});
        if (r.ok && r.data.contains("normal"))
            h["normal"] = r.data["normal"];
        if (const auto* part = controller.session().document().part(h.at("part").get<model::PartId>()))
            h["partName"] = part->name;
    }
    return Json{{"hits", hits}};
}

} // namespace

commands::CommandResult runViewCommand(QQuickWindow* window, AppController& controller, const std::string& name,
                                       const nlohmann::json& params)
{
    if (window == nullptr)
        return commands::CommandResult::failure("The AeroCAD window is not available");
    const Json p = params.is_object() ? params : Json::object();
    try {
        if (name == "view.set") {
            checkFraming(p);
            if (p.contains("area") || p.contains("maxSize"))
                throw commands::CommandError("area and maxSize belong to view_screenshot");
            return commands::CommandResult::success(Json{{"camera", setView(window, p)}});
        }
        if (name == "view.screenshot") {
            checkFraming(p);
            if (p.contains("area") && p["area"] != "viewport" && p["area"] != "window")
                throw commands::CommandError("area is viewport or window");
            return commands::CommandResult::success(screenshot(window, controller, p));
        }
        if (name == "view.show_results") {
            QVariant ret;
            if (!QMetaObject::invokeMethod(window, "agentShowResults", Q_RETURN_ARG(QVariant, ret),
                                           Q_ARG(QVariant, toVariant(p))))
                throw commands::CommandError("The simulation panel is not available");
            Json out = fromQml(ret);
            if (out.is_object() && out.contains("error"))
                throw commands::CommandError(out["error"].get<std::string>());
            return commands::CommandResult::success(out);
        }
        if (name == "view.pick")
            return commands::CommandResult::success(pick(window, controller, p));
    } catch (const std::exception& e) {
        return commands::CommandResult::failure(e.what());
    }
    return commands::CommandResult::failure("Unknown view command " + name);
}

} // namespace aerocad::app
