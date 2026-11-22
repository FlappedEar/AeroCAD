// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Cancel.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <string>

namespace aerocad::commands {

/// How a long-running command reports to, and is stopped by, whoever runs it (CAD-30). Both
/// parts are optional: a command run directly (MCP, scripts, tests) gets an empty control.
struct JobControl {
    /// Called with the fraction done (0..1, negative: unknown) and what is being done.
    std::function<void(double fraction, const std::string& text)> onProgress;
    /// Returns true once the user pressed Cancel.
    core::CancelCheck cancelled;

    void progress(double fraction, const std::string& text) const
    {
        if (onProgress)
            onProgress(fraction, text);
    }
    /// Throws core::Cancelled when cancelled.
    void checkCancelled() const { core::throwIfCancelled(cancelled); }
};

/// The slow part of a command, cut loose from the document so that it can run on a worker
/// thread while the UI stays usable. A command that supports it is registered with
/// CommandRegistry::addPlanned(): its planner reads the document on the calling thread
/// (validates the parameters, copies what the work needs, shares the immutable meshes and
/// shapes) and returns this; `run` touches no document, session or UI state. Throws
/// CommandError like a handler and core::Cancelled when stopped.
struct BackgroundWork {
    /// Rough size of the work, in triangles; the UI runs small jobs at once instead.
    std::size_t weight = 0;
    std::function<nlohmann::json(const JobControl&)> run;
};

} // namespace aerocad::commands
