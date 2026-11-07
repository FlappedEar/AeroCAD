// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/CommandRegistry.hpp"

#include "ParamsValidation.hpp"

#include "aerocad/geometry/KernelError.hpp"

#include <exception>
#include <utility>

namespace aerocad::commands {

Json CommandResult::toJson() const
{
    if (ok)
        return Json{{"ok", true}, {"result", data}};
    return Json{{"ok", false}, {"error", error}};
}

void CommandRegistry::add(CommandInfo info, Handler handler)
{
    if (info.name.empty() || !handler)
        throw std::invalid_argument("CommandRegistry::add: empty name or handler");
    std::string name = info.name;
    auto [it, inserted] = m_commands.try_emplace(std::move(name), Entry{std::move(info), std::move(handler)});
    if (!inserted)
        throw std::invalid_argument("CommandRegistry::add: duplicate command " + it->first);
}

bool CommandRegistry::contains(std::string_view name) const
{
    return m_commands.find(name) != m_commands.end();
}

const CommandInfo* CommandRegistry::info(std::string_view name) const
{
    auto it = m_commands.find(name);
    return it == m_commands.end() ? nullptr : &it->second.info;
}

std::vector<CommandInfo> CommandRegistry::list() const
{
    std::vector<CommandInfo> out;
    out.reserve(m_commands.size());
    for (const auto& [_, entry] : m_commands)
        out.push_back(entry.info);
    return out;
}

CommandResult CommandRegistry::execute(CommandContext& context, std::string_view name, const Json& params) const
{
    auto it = m_commands.find(name);
    if (it == m_commands.end())
        return CommandResult::failure("Unknown command: " + std::string(name));

    const Entry& entry = it->second;
    const Json& effective = params.is_null() ? Json::object() : params;

    CommandResult result;
    if (auto err = validateParams(entry.info.params, effective)) {
        result = CommandResult::failure("Invalid parameters for " + entry.info.name + ": " + *err);
    } else {
        // No exception may leave a command: the UI, scripts and the MCP server all call
        // through here, and an escaping exception would end the process.
        try {
            try {
                result = CommandResult::success(entry.handler(context, effective));
            } catch (...) {
                geometry::rethrowAsKernelError(); // Open CASCADE errors are not std::exceptions
            }
        } catch (const CommandError& e) {
            result = CommandResult::failure(e.what());
        } catch (const std::exception& e) {
            result = CommandResult::failure(entry.info.name + " failed: " + e.what());
        } catch (...) {
            result = CommandResult::failure(entry.info.name + " failed: unknown error");
        }
    }
    if (m_listener)
        m_listener(entry.info, effective, result);
    return result;
}

} // namespace aerocad::commands
