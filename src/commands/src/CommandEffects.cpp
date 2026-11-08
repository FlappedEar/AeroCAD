// SPDX-License-Identifier: GPL-3.0-or-later
// The effects of commands beyond the document (files, processes, network, undo). Kept in one
// table so that a client deciding what to allow (MCP annotations, permission prompts) reads
// one place; tests/commands checks the table against the registry.
#include "aerocad/commands/Session.hpp"

#include <string_view>
#include <utility>

namespace aerocad::commands {

void declareCommandEffects(CommandRegistry& r)
{
    CommandEffects readFile;
    readFile.readsFiles = true;

    // Writes or replaces files at a path the caller gives.
    CommandEffects writeFiles;
    writeFiles.writesFiles = true;
    writeFiles.destructive = true; // an existing file of that name is replaced

    // Replaces the open project and clears the undo history: unsaved work is gone.
    CommandEffects replaceProject;
    replaceProject.undoable = false;
    replaceProject.destructive = true;
    CommandEffects openProject = replaceProject;
    openProject.readsFiles = true;

    // Simulation runs write case files and run OpenFOAM, locally or on a remote engine.
    CommandEffects runSimulation;
    runSimulation.writesFiles = true;
    runSimulation.runsProcesses = true;
    runSimulation.network = true;
    // Asks the engine (Docker or OpenFOAM, possibly over SSH) without changing anything.
    CommandEffects queryEngine;
    queryEngine.network = true;
    CommandEffects cancelSimulation = queryEngine;
    cancelSimulation.runsProcesses = true; // stops the solver
    CommandEffects deleteRun;
    deleteRun.deletesFiles = true;
    deleteRun.destructive = true;
    CommandEffects installEngine;
    installEngine.writesFiles = true;
    installEngine.runsProcesses = true;
    installEngine.network = true;
    installEngine.installsSoftware = true;
    CommandEffects setEngine = queryEngine;
    setEngine.writesFiles = true; // saves the engine setting
    CommandEffects importRun;
    importRun.readsFiles = true;
    importRun.writesFiles = true; // registers the case in the runs folder

    CommandEffects saveRunLabel;
    saveRunLabel.writesFiles = true; // run.json of the run

    // Undoable, but removes user data from the document.
    CommandEffects removePart;
    removePart.destructive = true;

    const std::pair<std::string_view, CommandEffects> table[] = {
        {"part.remove", removePart},
        {"document.import", readFile},
        {"document.export", writeFiles},
        {"document.export_for_print", writeFiles},
        {"project.save", writeFiles},
        {"project.new", replaceProject},
        {"project.open", openProject},
        {"simulation.run", runSimulation},
        {"simulation.refine", runSimulation},
        {"simulation.cancel", cancelSimulation},
        {"simulation.environment", queryEngine},
        {"simulation.propose_run", queryEngine},
        {"simulation.set_engine", setEngine},
        {"simulation.install_engine", installEngine},
        {"simulation.delete", deleteRun},
        {"simulation.import", importRun},
        {"simulation.export_results", writeFiles},
        {"simulation.rename", saveRunLabel},
    };
    for (const auto& [name, effects] : table)
        r.setEffects(name, effects);
}

} // namespace aerocad::commands
