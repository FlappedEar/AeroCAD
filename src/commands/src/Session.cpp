// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"

namespace aerocad::commands {

Session::Session()
    : m_context{m_document, m_undoStack, &m_registry, &m_design, &m_inspect, &m_project, &m_importStage}
{
    registerBuiltinCommands(m_registry);
    registerDesignCommands(m_registry);
    registerInspectCommands(m_registry);
    registerProjectCommands(m_registry);
    registerScanCommands(m_registry);
    m_design.setPartLookup([this](model::PartId id) { return m_document.part(id); });
}

CommandResult Session::execute(std::string_view name, const Json& params)
{
    const CommandInfo* info = m_registry.info(name);
    const bool modifies = info != nullptr && info->modifiesDocument;
    // Saving must not store a live preview either.
    if (modifies || name.starts_with("project."))
        cancelPreview();
    const std::uint64_t before = m_project.changes;
    CommandResult result = m_registry.execute(m_context, name, params);
    // project.open / project.new reset the counters themselves.
    if (result.ok && modifies && m_project.changes == before)
        ++m_project.changes;
    return result;
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
