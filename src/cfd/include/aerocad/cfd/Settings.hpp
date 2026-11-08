// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aerocad::cfd {

using Json = nlohmann::json;

/// The OpenFOAM release every case is generated for and validated against. One version
/// only (CAD-50): dictionaries differ between releases and between the openfoam.com and
/// openfoam.org lines, so supporting more than one would mean testing more than one.
inline constexpr std::string_view kOpenFoamVersion = "v2506";
/// checkMesh failures (the text after its "***" marker, as printed by OpenFOAM v2506's
/// primitiveMeshCheck) that make the solver diverge or crash. Allrun stops before the solve
/// when log.checkMesh reports one (CAD-103); every other "***" report is a warning. An
/// extended regular expression alternation, also matched literally by RunManager.
inline constexpr std::string_view kFatalMeshChecks =
    "Zero or negative cell volume|Error in face pyramids|Zero or negative face area|Open cells found|"
    "Cells with invalid face labels";
/// Official multi-arch (amd64 + arm64) image of that release, from OpenCFD on Docker Hub.
inline constexpr std::string_view kDockerImage = "opencfd/openfoam-default:2506";

/// Largest part dimensions (m) a wind tunnel run treats as plausible (CAD-109). A part outside
/// is most likely in the wrong unit, such as an STL in metres read as millimetres.
inline constexpr double kMinPlausiblePartLength = 0.02;
inline constexpr double kMaxPlausiblePartLength = 50.0;

/// A warning when a part whose largest dimension is `length` metres is implausibly small or
/// large for a wind tunnel run, else empty.
[[nodiscard]] std::string partSizeWarning(double length);

/// Identifier of the only preset so far: steady incompressible external flow around the
/// selected part (simpleFoam, k-omega SST, snappyHexMesh). Bump the suffix when the
/// generated case changes in a way that changes results, so runs stay comparable.
inline constexpr std::string_view kExternalFlowPreset = "external-flow-v1";

/// Mesh resolution of the preset. Coarse is meant for quick A/B checks on a laptop.
enum class MeshLevel { Coarse, Medium, Fine };

/// Settings of one run. Everything that changes the result is in here and is written to
/// the run directory, so any run can be reproduced and two runs can be checked for
/// identical conditions (A/B comparison).
struct Settings {
    std::string preset{kExternalFlowPreset};
    /// Free-stream speed, m/s.
    double speed = 20.0;
    /// Direction the air flows in, in model coordinates (Z up): "+X", "-X", "+Y" or "-Y".
    /// Default "+Y": the app's Front view looks from -Y, so the air hits the front.
    std::string flowDirection = "+Y";
    /// Kinematic viscosity, m^2/s (air at about 20 degC).
    double kinematicViscosity = 1.5e-5;
    /// Density used to turn kinematic pressure and forces into Pa and N, kg/m^3.
    double density = 1.205;
    /// Free-stream turbulence intensity (fraction, 0.01 = 1 %).
    double turbulenceIntensity = 0.01;
    /// Free-stream turbulent to laminar viscosity ratio.
    double viscosityRatio = 10.0;
    MeshLevel mesh = MeshLevel::Coarse;
    /// Maximum solver iterations (the solver stops earlier when the residuals converge).
    int iterations = 0; ///< 0 = preset default for the mesh level.
    /// Adds a no-slip floor below the geometry (road / tunnel floor). Off: the part is in
    /// free air.
    bool ground = false;
    /// The floor moves with the air (road); false = a fixed wind-tunnel floor.
    bool groundMoving = true;
    /// Height of the floor in model Z (mm); empty = just below the lowest point of the
    /// geometry. Must not be above the part; context parts (e.g. supports) may reach below it.
    std::optional<double> groundHeight;
    /// Reference area for Cd/Cl in m^2; empty = frontal area of the part (computed).
    std::optional<double> referenceArea;
    /// Reference length for moment coefficients in m; empty = part length along the flow.
    std::optional<double> referenceLength;
    /// Parallel processes; 0 = automatic.
    int processes = 0;

    [[nodiscard]] int effectiveIterations() const;
    [[nodiscard]] int effectiveProcesses() const;
};

[[nodiscard]] std::string_view meshLevelName(MeshLevel level);
[[nodiscard]] std::optional<MeshLevel> parseMeshLevel(std::string_view name);
[[nodiscard]] bool isValidFlowDirection(std::string_view direction);

[[nodiscard]] Json toJson(const Settings& settings);
/// Reads settings; missing keys keep their defaults. Throws std::invalid_argument on
/// invalid values.
[[nodiscard]] Settings settingsFromJson(const Json& json);

/// JSON Schema of the settings object (for the command layer / MCP tools).
[[nodiscard]] Json settingsSchema();

/// The settings that must be equal for an A/B comparison to be meaningful. Returns the
/// names of the differing settings (empty = comparable). Reference area/length are
/// compared on their effective values, which the caller passes in.
[[nodiscard]] std::vector<std::string> differingConditions(const Json& a, const Json& b);

} // namespace aerocad::cfd
