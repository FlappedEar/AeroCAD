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
    const auto* info = m_registry.info(name);
    if (info != nullptr && info->modifiesDocument)
        cancelPreview();
    return m_registry.execute(m_context, name, params);
}

CommandResult Session::preview(std::string_view name, const Json& params)
{
    cancelPreview();
    auto base = m_undoStack.snapshot();
    const std::size_t before = base.index;
    CommandResult result = m_registry.execute(m_context, name, params);
    if (result.ok && m_undoStack.position() == before + 1) {
        m_previewBase = std::move(base);
    } else if (result.ok) {
        // The command did not record exactly one step (nothing changed); nothing to undo later.
        m_undoStack.restore(std::move(base));
    }
    return result;
}

bool Session::cancelPreview()
{
    if (!m_previewBase)
        return false;
    m_undoStack.undo();
    m_undoStack.restore(std::move(*m_previewBase));
    m_previewBase.reset();
    return true;
}

} // namespace aerocad::commands
