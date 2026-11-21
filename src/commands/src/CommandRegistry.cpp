// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/CommandRegistry.hpp"

#include "ParamsValidation.hpp"

#include "aerocad/geometry/KernelError.hpp"

#include <exception>
#include <stdexcept>
#include <string>
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

void CommandRegistry::addPlanned(CommandInfo info, Planner planner)
{
    if (!planner)
        throw std::invalid_argument("CommandRegistry::addPlanned: empty planner");
    const std::string name = info.name;
    add(std::move(info), [planner](CommandContext& context, const Json& params) {
        const BackgroundWork work = planner(context, params);
        return work.run(JobControl{});
    });
    m_planners.emplace(name, std::move(planner));
}

bool CommandRegistry::isPlanned(std::string_view name) const
{
    return m_planners.find(name) != m_planners.end();
}

std::variant<BackgroundWork, CommandResult> CommandRegistry::plan(CommandContext& context, std::string_view name,
                                                                  const Json& params) const
{
    const auto planner = m_planners.find(name);
    if (planner == m_planners.end())
        return CommandResult::failure("Command cannot run in the background: " + std::string(name));
    const CommandInfo& info = m_commands.find(name)->second.info;
    const Json& effective = params.is_null() ? Json::object() : params;
    if (auto err = validateParams(info.params, effective))
        return CommandResult::failure("Invalid parameters for " + info.name + ": " + *err);
    try {
        try {
            return planner->second(context, effective);
        } catch (...) {
            geometry::rethrowAsKernelError();
        }
    } catch (const CommandError& e) {
        return CommandResult::failure(e.what());
    } catch (const std::exception& e) {
        return CommandResult::failure(info.name + " failed: " + e.what());
    } catch (...) {
        return CommandResult::failure(info.name + " failed: unknown error");
    }
}

WorkOutcome runWork(std::string_view name, const BackgroundWork& work, const JobControl& control)
{
    const std::string command(name);
    try {
        try {
            return {CommandResult::success(work.run(control)), false};
        } catch (...) {
            geometry::rethrowAsKernelError();
        }
    } catch (const core::Cancelled&) {
        return {CommandResult::failure(command + " cancelled"), true};
    } catch (const CommandError& e) {
        return {CommandResult::failure(e.what()), false};
    } catch (const std::exception& e) {
        return {CommandResult::failure(command + " failed: " + e.what()), false};
    } catch (...) {
        return {CommandResult::failure(command + " failed: unknown error"), false};
    }
}

bool CommandRegistry::contains(std::string_view name) const
{
    return m_commands.find(name) != m_commands.end();
}

void CommandRegistry::setEffects(std::string_view name, const CommandEffects& effects)
{
    auto it = m_commands.find(name);
    if (it == m_commands.end())
        throw std::invalid_argument("CommandRegistry::setEffects: unknown command " + std::string(name));
    it->second.info.effects = effects;
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
