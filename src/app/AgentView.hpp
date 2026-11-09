// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/commands/CommandRegistry.hpp"

#include <string>

class QQuickWindow;

namespace aerocad::app {

class AppController;

/// Runs view.screenshot, view.set and view.pick for an AI agent on the main window: the
/// camera is set through Main.qml's agentSetView(), the image is grabbed from the window and
/// points are picked through agentPick(). See src/commands/src/AgentCommands.cpp for the schemas.
commands::CommandResult runViewCommand(QQuickWindow* window, AppController& controller, const std::string& name,
                                       const nlohmann::json& params);

} // namespace aerocad::app
