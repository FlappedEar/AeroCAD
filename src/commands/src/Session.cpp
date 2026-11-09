// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"

#include "CommandHelpers.hpp"

#include "aerocad/cfd/RunManager.hpp"

namespace aerocad::commands {

namespace {

// Document notifications can make the UI query the session while a command still
// holds part references. Rebuild only at the outer operation boundary, never in such
// a query or in a notification emitted while mirroring regenerated parts.
struct OperationScope {
    std::size_t& depth;
    explicit OperationScope(std::size_t& d) : depth(d) { ++depth; }
    ~OperationScope() { --depth; }
};

} // namespace

Session::Session()
    : m_simulations(std::make_unique<cfd::RunManager>()),
      m_context{m_document, m_undoStack, &m_registry, &m_design, &m_inspect, &m_project, &m_importStage,
                m_simulations.get()}
{
    registerBuiltinCommands(m_registry);
    registerAgentCommands(m_registry);
    registerDesignCommands(m_registry);
    registerInspectCommands(m_registry);
    registerProjectCommands(m_registry);
    registerScanCommands(m_registry);
    registerSimulationCommands(m_registry);
    declareCommandEffects(m_registry);
    m_design.setPartLookup([this](model::PartId id) { return m_document.part(id); });
    registerSurfaceFeatures(m_design, m_document, &m_fitStage);
}

Session::~Session() = default;

std::optional<std::string> Session::synchronizeSources()
{
    const auto revision = m_document.importedGeometryRevision();
    if (m_operationDepth > 1 || m_sourceRevision == revision)
        return std::nullopt;
    if (m_design.features().empty()) {
        // A document can also be supplied without a timeline (e.g. for inspection).
        // There are no descendants to rebuild and no timeline mirror to replace.
        m_sourceRevision = revision;
        return std::nullopt;
    }
    try {
        m_design.regenerate();
        m_design.syncDocument(m_document);
        m_sourceRevision = revision;
        return std::nullopt;
    } catch (const std::exception& e) {
        return std::string("Could not regenerate source-dependent features: ") + e.what() +
               ". The source change remains applied; use Undo to restore it.";
    } catch (...) {
        return "Could not regenerate source-dependent features: unknown geometry error. "
               "The source change remains applied; use Undo to restore it.";
    }
}

CommandResult Session::execute(std::string_view name, const Json& params)
{
    const OperationScope operation(m_operationDepth);
    const CommandInfo* info = m_registry.info(name);
    const bool modifies = info != nullptr && info->modifiesDocument;
    // Saving must not store a live preview either.
    if (modifies || name.starts_with("project."))
        cancelPreview();
    // Also covers sources supplied directly through Document, before queries, export
    // and project.save consume geometry or persisted feature results.
    // Undo/redo must still work after a failed rebuild, so the user can recover the
    // previous source. The revision remains pending until a rebuild succeeds.
    if (name != "edit.undo" && name != "edit.redo")
        if (auto error = synchronizeSources())
            return CommandResult::failure(*error);
    const std::uint64_t before = m_project.changes;
    const auto sourceBefore = m_document.importedGeometryRevision();
    CommandResult result = m_registry.execute(m_context, name, params);
    // project.open / project.new reset the counters themselves.
    if (modifies && m_project.changes == before &&
        (result.ok || sourceBefore != m_document.importedGeometryRevision()))
        ++m_project.changes;
    if (auto error = synchronizeSources())
        return CommandResult::failure(*error);
    return result;
}

CommandResult Session::preview(std::string_view name, const Json& params)
{
    const OperationScope operation(m_operationDepth);
    cancelPreview();
    if (auto error = synchronizeSources())
        return CommandResult::failure(*error);
    auto base = m_undoStack.snapshot();
    const std::size_t before = base.index;
    CommandResult result = m_registry.execute(m_context, name, params);
    if (m_undoStack.position() == before + 1) {
        // Keep the rollback even if rebuilding the derived geometry fails below.
        m_previewBase = std::move(base);
    } else if (result.ok) {
        // The command did not record exactly one step (nothing changed); nothing to undo later.
        m_undoStack.restore(std::move(base));
    }
    if (auto error = synchronizeSources())
        return CommandResult::failure(*error);
    return result;
}

bool Session::cancelPreview()
{
    const OperationScope operation(m_operationDepth);
    if (!m_previewBase)
        return false;
    m_undoStack.undo();
    m_undoStack.restore(std::move(*m_previewBase));
    m_previewBase.reset();
    // A rebuild failure stays pending and is reported by the next execute/preview.
    // The preview source and history have nevertheless been restored here.
    (void)synchronizeSources();
    return true;
}

} // namespace aerocad::commands
