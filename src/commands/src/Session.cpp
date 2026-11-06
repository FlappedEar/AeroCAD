// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"

#include "aerocad/cfd/RunManager.hpp"

namespace aerocad::commands {

Session::Session()
    : m_simulations(std::make_unique<cfd::RunManager>()),
      m_context{m_document, m_undoStack, &m_registry, &m_design, m_simulations.get()}
{
    registerBuiltinCommands(m_registry);
    registerDesignCommands(m_registry);
    registerSimulationCommands(m_registry);
    m_design.setPartLookup([this](model::PartId id) { return m_document.part(id); });
}

Session::~Session() = default;

CommandResult Session::execute(std::string_view name, const Json& params)
{
    return m_registry.execute(m_context, name, params);
}

} // namespace aerocad::commands
