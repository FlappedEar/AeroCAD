// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/commands/CommandRegistry.hpp"

namespace aerocad::commands {

/// Registers document.export and part.check.
void registerExportCommands(CommandRegistry& registry);

} // namespace aerocad::commands
