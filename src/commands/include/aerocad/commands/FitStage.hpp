// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/geometry/RegionFit.hpp"
#include "aerocad/geometry/SurfaceFit.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

namespace aerocad::model {
class Document;
}
namespace aerocad::cad {
class Design;
}

namespace aerocad::commands {

/// Surface fits computed ahead of time, on a worker thread while the UI shows progress
/// and a Cancel button (CAD-259). The surface.fit_selection / surface.fit_regions step
/// takes a staged result that matches its mesh, region and options instead of fitting
/// again, so the document still changes only through the command (same undo, same
/// timeline step, same MCP-visible behaviour). Anything that does not match is fit as
/// before. Thread-safe.
class FitStage {
public:
    using Result = std::variant<geometry::SurfaceFitResult, geometry::RegionFitResult>;
    void put(const std::string& key, Result result);
    /// Removes and returns the staged result for `key`, if any.
    [[nodiscard]] std::optional<Result> take(const std::string& key);
    void clear();

private:
    std::mutex m_mutex;
    std::map<std::string, Result> m_results;
};

/// The fit that surface.fit_selection or surface.fit_regions (`command`) would run for
/// `params` on the current document, ready to run on another thread: `run` only reads
/// its own copies (the mesh is shared and immutable). Put its result in the session's
/// FitStage under `key`, then execute the command with the same params.
struct PreparedFit {
    std::string key;
    std::size_t triangles = 0; ///< Triangles to fit (the region, or the whole mesh).
    std::function<FitStage::Result(std::function<void(int done, int total)> progress,
                                   std::function<bool()> cancelled)>
        run;
};

/// Empty when the command would not fit anything (unknown part, no mesh or region, bad
/// options, too large): the command then reports the problem itself.
[[nodiscard]] std::optional<PreparedFit> prepareFit(const model::Document& document, const cad::Design& design,
                                                    const std::string& command, const nlohmann::json& params);

} // namespace aerocad::commands
