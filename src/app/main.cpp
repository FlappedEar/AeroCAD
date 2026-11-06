// SPDX-License-Identifier: GPL-3.0-or-later
#include "AppController.hpp"

#include <QCommandLineParser>
#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cstdlib>
#include <cstdio>

namespace {

/// --smoke-test <file>: import the file, wait for frames to be rendered, verify, quit.
/// Exit code 0 on success. Used by CTest (app_smoke_test) and CI.
int runSmokeTest(QGuiApplication& app, QQmlApplicationEngine& engine, aerocad::app::AppController& controller,
                 const QString& file, const QString& screenshotPath, const QString& scriptPath)
{
    auto* window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (window == nullptr) {
        std::fprintf(stderr, "smoke test: no root window\n");
        return 2;
    }
    if (!controller.openFile(QUrl::fromLocalFile(file))) {
        std::fprintf(stderr, "smoke test: import failed: %s\n", qPrintable(controller.statusMessage()));
        return 3;
    }
    if (!scriptPath.isEmpty() && !controller.runScript(scriptPath)) {
        std::fprintf(stderr, "smoke test: script failed: %s\n", qPrintable(controller.statusMessage()));
        return 7;
    }

    // Keep requesting frames (nothing animates, so the window would otherwise go idle),
    // then grab the window and check that the model is actually on screen. The viewport
    // background is a vertical gradient, so each row is compared with the viewport's
    // leftmost pixel in that row.
    int frames = 0;
    int rc = -1;
    QObject::connect(window, &QQuickWindow::frameSwapped, &app, [&] {
        if (++frames < 5) {
            window->update();
            return;
        }
        if (rc != -1)
            return;
        const QImage image = window->grabWindow();
        if (!screenshotPath.isEmpty())
            image.save(screenshotPath);
        auto* viewport = window->findChild<QQuickItem*>(QStringLiteral("viewport"));
        if (viewport == nullptr) {
            std::fprintf(stderr, "smoke test: viewport item not found\n");
            app.exit(6);
            return;
        }
        const qreal dpr = window->effectiveDevicePixelRatio();
        const QRectF area = viewport->mapRectToScene(QRectF(0, 0, viewport->width(), viewport->height()));
        const QRect pixels = QRect(QPoint(qRound(area.left() * dpr), qRound(area.top() * dpr)),
                                   QPoint(qRound(area.right() * dpr) - 1, qRound(area.bottom() * dpr) - 1))
                                 .intersected(image.rect());
        qsizetype bright = 0;
        // Only the central part is checked: the model is framed there (fit on import),
        // while the ViewCube and navigation bar overlays live near the edges.
        const QRect centre = pixels.adjusted(pixels.width() / 5, pixels.height() / 5, -pixels.width() / 5,
                                             -pixels.height() / 5);
        const int referenceX = std::min(pixels.left() + 2, pixels.right());
        for (int y = centre.top(); y <= centre.bottom(); y += 2) {
            const QColor bg = image.pixelColor(referenceX, y);
            for (int x = centre.left(); x <= centre.right(); x += 2) {
                const QColor c = image.pixelColor(x, y);
                const int diff = std::abs(c.red() - bg.red()) + std::abs(c.green() - bg.green()) +
                                 std::abs(c.blue() - bg.blue());
                if (diff > 60)
                    ++bright;
            }
        }
        if (bright == 0)
            std::fprintf(stderr, "smoke test: model not visible in the rendered frame\n");
        rc = bright > 0 ? 0 : 5;
        std::printf("smoke test: %d parts, %d frames, %lld model pixels, graphics API %d\n",
                    controller.parts()->rowCount(), frames, static_cast<long long>(bright),
                    static_cast<int>(window->rendererInterface()->graphicsApi()));
        app.exit(rc);
    });
    QTimer::singleShot(30000, &app, [&] {
        std::fprintf(stderr, "smoke test: timed out after %d frames\n", frames);
        app.exit(4);
    });
    window->update();
    rc = QGuiApplication::exec();
    return rc;
}

} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("AeroCAD"));
    QGuiApplication::setOrganizationName(QStringLiteral("FlappedEar"));
    QGuiApplication::setApplicationVersion(QStringLiteral(AEROCAD_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("AeroCAD"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption smokeTest(QStringLiteral("smoke-test"),
                                 QStringLiteral("Open <file> (project or 3D file), render frames, exit (for CI)."),
                                 QStringLiteral("file"));
    parser.addOption(smokeTest);
    QCommandLineOption screenshot(QStringLiteral("screenshot"),
                                  QStringLiteral("With --smoke-test: save the rendered window to <png>."),
                                  QStringLiteral("png"));
    parser.addOption(screenshot);
    QCommandLineOption script(QStringLiteral("run"),
                              QStringLiteral("Run a JSON command script (array of {name, params}) after loading files."),
                              QStringLiteral("script.json"));
    parser.addOption(script);
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("Projects (.aerocad) or 3D files to open."),
                                 QStringLiteral("[files...]"));
    parser.process(app);

    aerocad::app::AppController controller;

    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/qt/qml"));
    engine.setInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&controller)}});
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/AeroCAD/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;

    if (parser.isSet(smokeTest))
        return runSmokeTest(app, engine, controller, parser.value(smokeTest), parser.value(screenshot),
                            parser.value(script));

    QMetaObject::invokeMethod(engine.rootObjects().first(), "offerRecovery");
    for (const QString& file : parser.positionalArguments())
        controller.openFile(QUrl::fromLocalFile(file));
    if (parser.isSet(script))
        controller.runScript(parser.value(script));

    return QGuiApplication::exec();
}
