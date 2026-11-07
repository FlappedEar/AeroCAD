// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/cad/Variants.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace aerocad::commands {

/// Project-level state of a Session: file path, unsaved-changes tracking and variants.
struct ProjectState {
    /// Path of the project file (UTF-8), empty for an untitled project.
    std::string path;
    /// Number of successful document-modifying commands (bumped by Session::execute).
    std::uint64_t changes = 0;
    /// Value of `changes` at the last save / open.
    std::uint64_t savedChanges = 0;
    /// Bumped whenever the project is replaced (project.new / project.open). Work started
    /// for an earlier generation (a background import) must not touch the current one.
    std::uint64_t generation = 0;
    /// Opened from an autosave copy: unsaved until the next save.
    bool recovered = false;
    cad::VariantSet variants;
    /// View state (camera) of the last save / open, opaque JSON from the UI.
    nlohmann::json view = nlohmann::json::object();
    /// Written into project files ("AeroCAD 0.1.0"); set by the application.
    std::string generator = "AeroCAD";

    [[nodiscard]] bool dirty() const { return recovered || changes != savedChanges; }
};

} // namespace aerocad::commands
