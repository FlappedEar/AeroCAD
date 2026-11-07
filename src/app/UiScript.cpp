// SPDX-License-Identifier: GPL-3.0-or-later
// --ui-script <file>: drives the application the way a user does. Each line of the file is
// one JavaScript statement, evaluated in the context of Main.qml (its ids are visible), with
// the `ui` object below. Lines run with the event loop turning in between; `ui.wait(ms)`
// adds time. '#' starts a comment line. The application exits after the last line, with
// code 1 if a line threw or ui.fail() was called.
//
//   viewport.setView(0, 0)
//   ui.drag(600, 400, 700, 400, "middle", "shift", 8)
//   ui.clickText("Sketch on the scan")
//   ui.shot("/tmp/sketch.png")
#include "UiScript.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJSValue>
#include <QMouseEvent>
#include <QPointer>
#include <QPointingDevice>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>

namespace aerocad::app {

namespace {

class UiDriver : public QObject {
    Q_OBJECT
public:
    UiDriver(QQuickWindow* window, QObject* parent) : QObject(parent), m_window(window) {}

    /// Delay before the next line.
    Q_INVOKABLE void wait(int ms) { m_delay = ms; }
    Q_INVOKABLE void fail(const QString& why)
    {
        std::fprintf(stderr, "ui-script: FAIL: %s\n", qPrintable(why));
        failed = true;
    }
    Q_INVOKABLE void log(const QVariant& v)
    {
        std::printf("%s\n", qPrintable(v.toString()));
        std::fflush(stdout);
    }
    Q_INVOKABLE qint64 elapsed() const { return m_clock.elapsed(); }
    Q_INVOKABLE void startClock() { m_clock.start(); }

    Q_INVOKABLE void shot(const QString& path)
    {
        // Twice: the first grab can precede geometry updates queued by the last event.
        m_window->grabWindow();
        m_window->grabWindow().save(path);
    }

    Q_INVOKABLE void press(qreal x, qreal y, const QString& button = "left", const QString& mods = "")
    {
        m_buttons |= toButton(button);
        send(QEvent::MouseButtonPress, x, y, toButton(button), mods);
    }
    Q_INVOKABLE void move(qreal x, qreal y, const QString& mods = "") { send(QEvent::MouseMove, x, y, Qt::NoButton, mods); }
    Q_INVOKABLE void release(qreal x, qreal y, const QString& button = "left", const QString& mods = "")
    {
        m_buttons &= ~Qt::MouseButtons(toButton(button));
        send(QEvent::MouseButtonRelease, x, y, toButton(button), mods);
    }
    Q_INVOKABLE void click(qreal x, qreal y, const QString& button = "left", const QString& mods = "")
    {
        move(x, y, mods);
        press(x, y, button, mods);
        release(x, y, button, mods);
    }
    Q_INVOKABLE void drag(qreal x0, qreal y0, qreal x1, qreal y1, const QString& button = "left",
                          const QString& mods = "", int steps = 10)
    {
        move(x0, y0, mods);
        press(x0, y0, button, mods);
        for (int i = 1; i <= steps; ++i) {
            move(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, mods);
            QCoreApplication::processEvents();
        }
        release(x1, y1, button, mods);
    }

    /// A touchpad scroll (pixel delta, from a touchpad device as macOS reports a trackpad
    /// or Magic Mouse). `phase`: "begin", "update" (default), "end" or "momentum", as a
    /// real gesture sends them.
    Q_INVOKABLE void wheel(qreal x, qreal y, int dx, int dy, const QString& mods = "",
                           const QString& phase = "update")
    {
        static const QPointingDevice touchpad(QStringLiteral("ui-script touchpad"), 0x75500001,
                                              QInputDevice::DeviceType::TouchPad, QPointingDevice::PointerType::Finger,
                                              QInputDevice::Capability::Position | QInputDevice::Capability::Scroll |
                                                  QInputDevice::Capability::PixelScroll,
                                              2, 0);
        const QPointF local(x, y);
        const Qt::ScrollPhase p = phase == "begin"      ? Qt::ScrollBegin
                                  : phase == "end"      ? Qt::ScrollEnd
                                  : phase == "momentum" ? Qt::ScrollMomentum
                                                        : Qt::ScrollUpdate;
        QWheelEvent event(local, m_window->mapToGlobal(local), QPoint(dx, dy), QPoint(dx * 2, dy * 2), Qt::NoButton,
                          modifiers(mods), p, false, Qt::MouseEventSynthesizedBySystem, &touchpad);
        QCoreApplication::sendEvent(m_window, &event);
    }

    /// A mouse wheel turned by `notches` (positive = away from the user). `pixels` > 0 adds
    /// the pixel delta macOS reports for every mouse wheel, so scripts can check that it
    /// still zooms there.
    Q_INVOKABLE void mouseWheel(qreal x, qreal y, int notches, int pixels = 0, const QString& mods = "")
    {
        const QPointF local(x, y);
        QWheelEvent event(local, m_window->mapToGlobal(local), QPoint(0, notches * pixels), QPoint(0, notches * 120),
                          Qt::NoButton, modifiers(mods), Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(m_window, &event);
    }

    /// Clicks the visible, enabled item showing exactly this text.
    Q_INVOKABLE bool clickText(const QString& text)
    {
        // From the top: popups live in an overlay beside the content item.
        QQuickItem* top = m_window->contentItem();
        while (top->parentItem() != nullptr)
            top = top->parentItem();
        QQuickItem* item = find(top, text);
        if (item == nullptr) {
            fail("no visible item with text \"" + text + "\"");
            return false;
        }
        const QPointF c = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        click(c.x(), c.y());
        return true;
    }

    bool failed = false;
    int takeDelay()
    {
        const int d = m_delay;
        m_delay = 0;
        return d;
    }

private:
    static Qt::MouseButton toButton(const QString& b)
    {
        return b == "middle" ? Qt::MiddleButton : b == "right" ? Qt::RightButton : Qt::LeftButton;
    }

    static Qt::KeyboardModifiers modifiers(const QString& mods)
    {
        Qt::KeyboardModifiers m;
        if (mods.contains("shift"))
            m |= Qt::ShiftModifier;
        if (mods.contains("ctrl"))
            m |= Qt::ControlModifier;
        return m;
    }

    void send(QEvent::Type type, qreal x, qreal y, Qt::MouseButton button, const QString& mods)
    {
        const QPointF local(x, y);
        QMouseEvent event(type, local, local, m_window->mapToGlobal(local), button, m_buttons, modifiers(mods));
        QCoreApplication::sendEvent(m_window, &event);
    }

    static QQuickItem* find(QQuickItem* item, const QString& text)
    {
        if (!item->isVisible() || !item->isEnabled())
            return nullptr;
        const auto children = item->childItems();
        // Top-most first, as a click would land.
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if (QQuickItem* hit = find(*it, text))
                return hit;
        }
        return item->property("text").toString() == text && item->width() > 0 ? item : nullptr;
    }

    QPointer<QQuickWindow> m_window;
    Qt::MouseButtons m_buttons;
    int m_delay = 0;
    QElapsedTimer m_clock;
};

} // namespace

void startUiScript(QQmlApplicationEngine& engine, const QString& path)
{
    auto* root = engine.rootObjects().first();
    auto* window = qobject_cast<QQuickWindow*>(root);
    QFile file(path);
    if (window == nullptr || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr, "ui-script: cannot run %s\n", qPrintable(path));
        QCoreApplication::exit(2);
        return;
    }
    auto lines = QString::fromUtf8(file.readAll()).split('\n');
    auto* driver = new UiDriver(window, root);
    engine.rootContext()->setContextProperty(QStringLiteral("ui"), driver);
    auto* context = qmlContext(root);

    auto next = std::make_shared<std::function<void(int)>>();
    *next = [=](int index) {
        while (index < lines.size()) {
            const QString line = lines[index].trimmed();
            ++index;
            if (line.isEmpty() || line.startsWith('#'))
                continue;
            QQmlExpression expression(context, root, line);
            expression.evaluate();
            if (expression.hasError()) {
                std::fprintf(stderr, "ui-script: line %d: %s\n", index, qPrintable(expression.error().toString()));
                driver->failed = true;
            }
            window->update();
            QTimer::singleShot(std::max(50, driver->takeDelay()), driver, [next, index] { (*next)(index); });
            return;
        }
        QCoreApplication::exit(driver->failed ? 1 : 0);
    };
    QTimer::singleShot(1000, driver, [next] { (*next)(0); });
}

} // namespace aerocad::app

#include "UiScript.moc"
