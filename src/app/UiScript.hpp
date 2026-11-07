// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

class QQmlApplicationEngine;

namespace aerocad::app {

/// Runs a UI script (--ui-script): real mouse events, QML expressions and window grabs, for
/// checking a workflow on a real file by hand or in a test. See UiScript.cpp.
void startUiScript(QQmlApplicationEngine& engine, const QString& path);

} // namespace aerocad::app
