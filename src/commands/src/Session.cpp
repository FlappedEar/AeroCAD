// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"

namespace aerocad::commands {

Session::Session() : m_context{m_document, m_undoStack, &m_registry, &m_design}
{
    registerBuiltinCommands(m_registry);
    registerDesignCommands(m_registry);
}

CommandResult Session::execute(std::string_view name, const Json& params)
{
    return m_registry.execute(m_context, name, params);
}

} // namespace aerocad::commands
