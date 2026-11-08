// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cfd/Settings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace aerocad::cfd {

int Settings::effectiveIterations() const
{
    if (iterations > 0)
        return iterations;
    switch (mesh) {
    case MeshLevel::Coarse: return 300;
    case MeshLevel::Medium: return 500;
    case MeshLevel::Fine: return 800;
    }
    return 300;
}

int Settings::effectiveProcesses() const
{
    if (processes > 0)
        return processes;
    const unsigned hw = std::thread::hardware_concurrency();
    // Half of the logical cores (leave room for the UI), at least 1, at most 8: beyond
    // that the coarse meshes are too small to scale.
    return static_cast<int>(std::clamp(hw / 2U, 1U, 8U));
}

std::string partSizeWarning(double length)
{
    if (!(length > 0.0) || (length >= kMinPlausiblePartLength && length <= kMaxPlausiblePartLength))
        return {};
    char size[32];
    if (length < kMinPlausiblePartLength)
        std::snprintf(size, sizeof size, "only %.3g mm", length * 1000.0);
    else
        std::snprintf(size, sizeof size, "%.0f m", length);
    const std::string what = std::string(size) + " long";
    return "The part is " + what +
           ". Check its units: a model saved in metres or inches comes in at the wrong size. Convert it first "
           "(MESH > Scan Tools > \"This part is in\" > Convert); the simulation runs at the size shown.";
}

std::string_view meshLevelName(MeshLevel level)
{
    switch (level) {
    case MeshLevel::Coarse: return "coarse";
    case MeshLevel::Medium: return "medium";
    case MeshLevel::Fine: return "fine";
    }
    return "coarse";
}

std::optional<MeshLevel> parseMeshLevel(std::string_view name)
{
    if (name == "coarse")
        return MeshLevel::Coarse;
    if (name == "medium")
        return MeshLevel::Medium;
    if (name == "fine")
        return MeshLevel::Fine;
    return std::nullopt;
}

bool isValidFlowDirection(std::string_view direction)
{
    return direction == "+X" || direction == "-X" || direction == "+Y" || direction == "-Y";
}

Json toJson(const Settings& s)
{
    Json j{
        {"preset", s.preset},
        {"speed", s.speed},
        {"flowDirection", s.flowDirection},
        {"kinematicViscosity", s.kinematicViscosity},
        {"density", s.density},
        {"turbulenceIntensity", s.turbulenceIntensity},
        {"viscosityRatio", s.viscosityRatio},
        {"mesh", meshLevelName(s.mesh)},
        {"iterations", s.effectiveIterations()},
        {"ground", s.ground},
        {"groundMoving", s.groundMoving},
        {"processes", s.effectiveProcesses()},
    };
    j["groundHeight"] = s.groundHeight ? Json(*s.groundHeight) : Json(nullptr);
    j["referenceArea"] = s.referenceArea ? Json(*s.referenceArea) : Json(nullptr);
    j["referenceLength"] = s.referenceLength ? Json(*s.referenceLength) : Json(nullptr);
    return j;
}

namespace {

double positive(const Json& j, const char* key, double fallback)
{
    if (!j.contains(key) || j.at(key).is_null())
        return fallback;
    const double v = j.at(key).get<double>();
    if (!std::isfinite(v) || v <= 0.0)
        throw std::invalid_argument(std::string(key) + " must be a positive number");
    return v;
}

} // namespace

Settings settingsFromJson(const Json& j)
{
    Settings s;
    if (!j.is_object())
        return s;
    if (j.contains("preset")) {
        s.preset = j.at("preset").get<std::string>();
        if (s.preset != kExternalFlowPreset)
            throw std::invalid_argument("Unknown preset '" + s.preset + "' (available: " +
                                        std::string(kExternalFlowPreset) + ")");
    }
    s.speed = positive(j, "speed", s.speed);
    if (s.speed > 150.0)
        throw std::invalid_argument("speed above 150 m/s: the preset is incompressible (Mach < 0.3 only)");
    if (j.contains("flowDirection")) {
        s.flowDirection = j.at("flowDirection").get<std::string>();
        if (!isValidFlowDirection(s.flowDirection))
            throw std::invalid_argument("flowDirection must be one of +X, -X, +Y, -Y");
    }
    s.kinematicViscosity = positive(j, "kinematicViscosity", s.kinematicViscosity);
    s.density = positive(j, "density", s.density);
    s.turbulenceIntensity = positive(j, "turbulenceIntensity", s.turbulenceIntensity);
    s.viscosityRatio = positive(j, "viscosityRatio", s.viscosityRatio);
    if (j.contains("mesh")) {
        const auto level = parseMeshLevel(j.at("mesh").get<std::string>());
        if (!level)
            throw std::invalid_argument("mesh must be coarse, medium or fine");
        s.mesh = *level;
    }
    if (j.contains("iterations"))
        s.iterations = std::clamp(j.at("iterations").get<int>(), 0, 100000);
    if (j.contains("ground"))
        s.ground = j.at("ground").get<bool>();
    if (j.contains("groundMoving"))
        s.groundMoving = j.at("groundMoving").get<bool>();
    if (j.contains("groundHeight") && !j.at("groundHeight").is_null()) {
        const double z = j.at("groundHeight").get<double>();
        if (!std::isfinite(z))
            throw std::invalid_argument("groundHeight must be a number (mm)");
        s.groundHeight = z;
    }
    if (j.contains("referenceArea") && !j.at("referenceArea").is_null())
        s.referenceArea = positive(j, "referenceArea", 1.0);
    if (j.contains("referenceLength") && !j.at("referenceLength").is_null())
        s.referenceLength = positive(j, "referenceLength", 1.0);
    if (j.contains("processes"))
        s.processes = std::clamp(j.at("processes").get<int>(), 0, 256);
    return s;
}

Json settingsSchema()
{
    auto num = [](const char* description) { return Json{{"type", "number"}, {"description", description}}; };
    return Json{
        {"preset", {{"type", "string"}, {"enum", {kExternalFlowPreset}}, {"description", "Simulation preset."}}},
        {"speed", num("Free-stream air speed in m/s (default 20, max 150).")},
        {"flowDirection",
         {{"type", "string"},
          {"enum", {"+X", "-X", "+Y", "-Y"}},
          {"description", "Direction the air flows in, model axes (Z up). Default +Y (air hits the side seen in "
                          "the Front view)."}}},
        {"mesh",
         {{"type", "string"},
          {"enum", {"coarse", "medium", "fine"}},
          {"description", "Mesh resolution. coarse = minutes, fine = much slower but more accurate."}}},
        {"iterations",
         {{"type", "integer"},
          {"minimum", 0},
          {"description", "Maximum solver iterations (0 = preset default: 300/500/800)."}}},
        {"ground", {{"type", "boolean"}, {"description", "Add a floor below the geometry."}}},
        {"groundMoving",
         {{"type", "boolean"},
          {"description", "The floor moves with the air like a road (default true); false = fixed wind-tunnel "
                          "floor."}}},
        {"groundHeight",
         num("Floor height in model Z, mm (default: just below the lowest point). Not above the part; context "
             "parts such as supports may reach below it.")},
        {"referenceArea", num("Reference area for Cd/Cl in m^2 (default: frontal area of the part).")},
        {"referenceLength", num("Reference length in m (default: part length along the flow).")},
        {"kinematicViscosity", num("Kinematic viscosity in m^2/s (default 1.5e-5, air).")},
        {"density", num("Air density in kg/m^3 (default 1.205).")},
        {"turbulenceIntensity", num("Free-stream turbulence intensity as a fraction (default 0.01).")},
        {"viscosityRatio", num("Free-stream turbulent/laminar viscosity ratio (default 10).")},
        {"processes", {{"type", "integer"}, {"minimum", 0}, {"description", "Parallel processes (0 = automatic)."}}},
    };
}

std::vector<std::string> differingConditions(const Json& a, const Json& b)
{
    // Everything that defines the flow problem. Processes and iterations do not change
    // a converged result, so they are not compared.
    static constexpr std::array keys{"preset",  "speed",         "flowDirection", "kinematicViscosity",
                                     "density", "turbulenceIntensity", "viscosityRatio", "mesh",
                                     "ground",  "groundMoving", "groundHeight", "referenceArea",
                                     "referenceLength"};
    // Runs written before a setting existed used what is now its default.
    static const Json defaults{{"groundMoving", true}, {"groundHeight", nullptr}};
    std::vector<std::string> diff;
    for (const char* key : keys) {
        const Json fallback = defaults.value(key, Json());
        const Json va = a.value(key, fallback);
        const Json vb = b.value(key, fallback);
        if (va.is_number() && vb.is_number()) {
            const double x = va.get<double>();
            const double y = vb.get<double>();
            if (std::abs(x - y) > 1e-9 * std::max({1.0, std::abs(x), std::abs(y)}))
                diff.emplace_back(key);
        } else if (va != vb) {
            diff.emplace_back(key);
        }
    }
    return diff;
}

} // namespace aerocad::cfd
