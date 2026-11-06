// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"

namespace aerocad::commands {

Session::Session() : m_context{m_document, m_undoStack, &m_registry, &m_design, &m_project}
{
    registerBuiltinCommands(m_registry);
    registerDesignCommands(m_registry);
    registerProjectCommands(m_registry);
    m_design.setPartLookup([this](model::PartId id) { return m_document.part(id); });
}

CommandResult Session::execute(std::string_view name, const Json& params)
{
    const std::uint64_t before = m_project.changes;
    CommandResult result = m_registry.execute(m_context, name, params);
    // project.open / project.new reset the counters themselves.
    if (result.ok && m_project.changes == before) {
        const CommandInfo* info = m_registry.info(name);
        if (info != nullptr && info->modifiesDocument)
            ++m_project.changes;
    }
    return result;
}

} // namespace aerocad::commands
