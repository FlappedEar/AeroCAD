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
//   ui.drop(640, 400, ["model.stl"])
#include "UiScript.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QDropEvent>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJSValue>
#include <QKeySequence>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QPointingDevice>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <type_traits>
#include <vector>

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
        send(QEvent::MouseButtonPress, x, y, toButton(button), mods);
    }
    Q_INVOKABLE void move(qreal x, qreal y, const QString& mods = "") { send(QEvent::MouseMove, x, y, Qt::NoButton, mods); }
    Q_INVOKABLE void release(qreal x, qreal y, const QString& button = "left", const QString& mods = "")
    {
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

    /// A key press and release ("Escape", "Return", "M", ...) as the window system delivers
    /// it, so shortcuts see it as they see a real key.
    Q_INVOKABLE void key(const QString& name, const QString& mods = "")
    {
        const QKeySequence seq = QKeySequence::fromString(name);
        if (seq.isEmpty()) {
            fail("unknown key: " + name);
            return;
        }
        // Window shortcuts only fire in the active window.
        if (QGuiApplication::focusWindow() != m_window) {
            m_window->requestActivate();
            if (!QTest::qWaitForWindowActive(m_window.data(), 1000))
                std::fprintf(stderr, "ui-script: window did not become active for a key press\n");
        }
        QTest::keyClick(m_window.data(), seq[0].key(), modifiers(mods));
    }

    /// Drops files at (x, y) as a file manager does: drag enter, move and drop events with
    /// file URLs. Relative paths are resolved against the working directory.
    Q_INVOKABLE void drop(qreal x, qreal y, const QStringList& paths)
    {
        QMimeData data;
        QList<QUrl> urls;
        for (const QString& path : paths)
            urls << QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath());
        data.setUrls(urls);
        const QPointF at(x, y);
        const Qt::DropActions actions = Qt::CopyAction | Qt::LinkAction;
        sendDrag<QDragEnterEvent>(at, actions, &data);
        sendDrag<QDragMoveEvent>(at, actions, &data);
        QDropEvent dropEvent(at, actions, &data, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(m_window, &dropEvent);
    }

    /// Clicks the visible, enabled item showing exactly this text. Dialogs that Qt shows as
    /// windows of their own (MessageDialog on newer Qt) are searched first, as they are on
    /// top, and the click goes to the window that shows the item.
    Q_INVOKABLE bool clickText(const QString& text)
    {
        std::vector<QQuickWindow*> windows;
        for (QWindow* w : QGuiApplication::topLevelWindows()) {
            auto* quick = qobject_cast<QQuickWindow*>(w);
            if (quick != nullptr && quick != m_window && quick->isVisible())
                windows.push_back(quick);
        }
        windows.push_back(m_window);
        for (QQuickWindow* window : windows) {
            // From the top: popups live in an overlay beside the content item.
            QQuickItem* top = window->contentItem();
            while (top->parentItem() != nullptr)
                top = top->parentItem();
            if (QQuickItem* item = find(top, text)) {
                const QPointF c = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
                if (window == m_window) {
                    click(c.x(), c.y());
                } else {
                    send(window, QEvent::MouseButtonPress, c.x(), c.y(), Qt::LeftButton, "");
                    send(window, QEvent::MouseButtonRelease, c.x(), c.y(), Qt::LeftButton, "");
                }
                return true;
            }
        }
        fail("no visible item with text \"" + text + "\"");
        return false;
    }

    bool failed = false;
    int takeDelay()
    {
        const int d = m_delay;
        m_delay = 0;
        return d;
    }

private:
    /// Newer Qt deprecates the QPoint constructors of the drag events for QPointF ones that
    /// older Qt (6.4) does not have: use whichever exists.
    template <typename Event>
    void sendDrag(const QPointF& at, Qt::DropActions actions, const QMimeData* data)
    {
        if constexpr (std::is_constructible_v<Event, QPointF, Qt::DropActions, const QMimeData*, Qt::MouseButtons,
                                              Qt::KeyboardModifiers>) {
            Event event(at, actions, data, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(m_window, &event);
        } else {
            Event event(at.toPoint(), actions, data, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(m_window, &event);
        }
    }

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
        send(m_window, type, x, y, button, mods);
    }

    /// Mouse event at scene position (x, y) of `window`, through the window system as a
    /// real mouse (QTest): Qt Quick then sees the same grabs and releases as in use. (Events
    /// sent straight to the window left items holding the mouse grab after their release.)
    void send(QQuickWindow* window, QEvent::Type type, qreal x, qreal y, Qt::MouseButton button, const QString& mods)
    {
        const QPoint at = QPointF(x, y).toPoint();
        const QTest::MouseAction action = type == QEvent::MouseButtonPress     ? QTest::MousePress
                                          : type == QEvent::MouseButtonRelease ? QTest::MouseRelease
                                                                               : QTest::MouseMove;
        QTest::mouseEvent(action, window, button, modifiers(mods), at);
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
