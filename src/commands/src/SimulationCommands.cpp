// SPDX-License-Identifier: GPL-3.0-or-later
// Wind tunnel commands (simulation.*): run OpenFOAM on a part in the background, follow
// the run, read the results and compare two runs. The MCP server exposes them as
// simulation_run, simulation_status, simulation_compare, ... so an agent can do what
// the CFD panel does.
#include "aerocad/commands/Session.hpp"

#include "aerocad/cfd/RunManager.hpp"
#include "aerocad/core/MeshCheck.hpp"
#include "aerocad/core/Path.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace aerocad::commands {

namespace {

/// Warnings about the part to show before a run starts: size (CAD-109), open mesh (CAD-145).
Json partWarnings(double characteristicLength, const core::TriangleMesh& part)
{
    Json w = Json::array();
    if (const std::string size = cfd::partSizeWarning(characteristicLength); !size.empty())
        w.push_back(size);
    if (const std::string open = cfd::openPartWarning(core::checkMesh(part).boundaryEdges); !open.empty())
        w.push_back(open);
    return w;
}

Json objectSchema(Json properties, Json required = Json::array())
{
    return Json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)},
                {"additionalProperties", false}};
}

Json runIdParam(const char* description = "Simulation run id (see simulation.list).")
{
    return Json{{"type", "string"}, {"minLength", 1}, {"description", description}};
}

cfd::RunManager& runs(CommandContext& ctx)
{
    if (ctx.simulations == nullptr)
        throw CommandError("Simulations are not available in this session");
    return *ctx.simulations;
}

std::string requireRun(CommandContext& ctx, const Json& params, const char* key = "run")
{
    const std::string id = params.at(key).get<std::string>();
    if (!runs(ctx).status(id))
        throw CommandError("No simulation run '" + id + "' (see simulation.list)");
    return id;
}

/// True when the open document still holds the part a run was made from: same id and the
/// same geometry. Part ids are per document, so an id alone may name another part.
bool sourceInDocument(const CommandContext& ctx, const cfd::RunSource& source)
{
    if (source.partFingerprint == 0)
        return false;
    const model::Part* part = ctx.document.part(source.partId);
    return part != nullptr && part->mesh && cfd::meshFingerprint(*part->mesh) == source.partFingerprint;
}

Json statusJson(const CommandContext& ctx, const cfd::RunStatus& status)
{
    Json j = status.toJson();
    j["sourceInDocument"] = sourceInDocument(ctx, status.source);
    return j;
}

Json setupJson(const cfd::SetupStep& step)
{
    return Json{{"action", step.action}, {"message", step.message}};
}

/// Part and context geometry for a run, plus what it was made from.
struct RunInput {
    cfd::CaseGeometry geometry;
    cfd::RunSource source;
    cfd::Settings settings;
};

/// A box of the case frame (m) as an axis-aligned box in model coordinates (mm): the case
/// frame only turns about Z in quarter turns, so the corners map onto a box again.
Json modelBox(const cfd::CaseFrame& frame, const core::BoundingBox& caseBox)
{
    core::BoundingBox out;
    for (int i = 0; i < 8; ++i) {
        const core::Vec3 c{i & 1 ? caseBox.max.x : caseBox.min.x, i & 2 ? caseBox.max.y : caseBox.min.y,
                           i & 4 ? caseBox.max.z : caseBox.min.z};
        out.extend(frame.toModel(c));
    }
    return {{"min", {out.min.x, out.min.y, out.min.z}}, {"max", {out.max.x, out.max.y, out.max.z}}};
}

/// Direction the air flows in model coordinates (unit vector): +X in the case frame.
Json flowVector(const cfd::CaseFrame& frame)
{
    const core::Vec3 v = frame.unrotate({1.0, 0.0, 0.0});
    return {v.x, v.y, v.z};
}

RunInput collectInput(CommandContext& ctx, const Json& params)
{
    RunInput in;
    const auto id = params.at("id").get<model::PartId>();
    const model::Part* part = ctx.document.part(id);
    if (part == nullptr)
        throw CommandError("No part with id " + std::to_string(id));
    if (!part->mesh || part->mesh->isEmpty())
        throw CommandError("Part '" + part->name + "' has no surface to simulate");
    if (part->kind == model::PartKind::Sketch)
        throw CommandError("'" + part->name + "' is a sketch; simulate a body or an imported part");
    if (part->kind == model::PartKind::Plane)
        throw CommandError("'" + part->name + "' is a construction plane; simulate a body or an imported part");
    in.geometry.part = *part->mesh;
    in.source.partId = part->id;
    in.source.partName = part->name;
    in.source.partRevision = part->revision;
    in.source.partFingerprint = cfd::meshFingerprint(*part->mesh);
    for (const auto& c : params.value("context", Json::array())) {
        const auto cid = c.get<model::PartId>();
        if (cid == id)
            continue;
        const model::Part* cp = ctx.document.part(cid);
        if (cp == nullptr)
            throw CommandError("No part with id " + std::to_string(cid) + " (context)");
        // Sketches and construction planes are drawing aids, not walls in the flow (CAD-130).
        if (cp->mesh && cp->kind != model::PartKind::Sketch && cp->kind != model::PartKind::Plane)
            in.geometry.context.append(*cp->mesh);
        in.source.contextPartIds.push_back(cid);
    }
    in.source.label = params.value("label", std::string());
    try {
        in.settings = cfd::settingsFromJson(params);
    } catch (const std::exception& e) {
        throw CommandError(e.what());
    }
    return in;
}

Json runParams()
{
    Json props = cfd::settingsSchema();
    props["id"] = {{"type", "integer"}, {"minimum", 1}, {"description", "Part to simulate (see document.parts)."}};
    props["context"] = {{"type", "array"},
                        {"items", {{"type", "integer"}, {"minimum", 1}}},
                        {"description", "Other parts in the flow, e.g. the body section the part is mounted on. "
                                        "They shape the flow but are not included in the forces."}};
    props["label"] = {{"type", "string"}, {"description", "Optional label shown with the run, e.g. \"A\" or \"v2\"."}};
    return props;
}

/// simulation.propose_run: the run parameters, and whether to check the engine (slow: it
/// asks Docker, over SSH for a remote engine). The wind tunnel panel previews without it.
Json proposeParams()
{
    Json props = runParams();
    props["checkEngine"] = {{"type", "boolean"},
                            {"description", "Also check the simulation engine (default true; slow when remote)."}};
    return props;
}

Json polyDataSummary(const cfd::PolyData& d)
{
    Json scalars = Json::object();
    for (const auto& [name, values] : d.scalars) {
        double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest(), sum = 0;
        std::size_t n = 0;
        for (const double v : values)
            if (std::isfinite(v)) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                sum += v;
                ++n;
            }
        if (n > 0) {
            // Robust range for colour scales (CAD-135): a few extreme points at the stagnation
            // point or a sharp edge should not wash out the rest of the map.
            std::vector<double> sorted;
            sorted.reserve(n);
            for (const double v : values)
                if (std::isfinite(v))
                    sorted.push_back(v);
            std::sort(sorted.begin(), sorted.end());
            auto at = [&](double q) { return sorted[static_cast<std::size_t>(q * static_cast<double>(n - 1) + 0.5)]; };
            scalars[name] = {{"min", lo}, {"max", hi}, {"mean", sum / static_cast<double>(n)},
                             {"p02", at(0.02)}, {"p98", at(0.98)}};
        }
    }
    core::BoundingBox box;
    for (const auto& p : d.points)
        box.extend(p);
    Json j{{"points", d.points.size()},
           {"polygons", d.polygons.size()},
           {"lines", d.lines.size()},
           {"scalars", std::move(scalars)}};
    if (!box.isEmpty())
        j["bounds"] = {{"min", {box.min.x, box.min.y, box.min.z}}, {"max", {box.max.x, box.max.y, box.max.z}}};
    return j;
}

} // namespace

void registerSimulationCommands(CommandRegistry& r)
{
    r.add({"simulation.environment", "Check simulation engine",
           "Checks whether a simulation engine is available (Docker with the OpenFOAM image, or a local OpenFOAM) "
           "and returns the OpenFOAM version, the preset, the settings schema and where runs are stored.",
           objectSchema(Json::object())},
          [](CommandContext& ctx, const Json&) {
              const cfd::Environment env = cfd::detectEnvironment();
              Json j = env.toJson();
              j["preset"] = cfd::kExternalFlowPreset;
              j["presetDescription"] =
                  "Steady incompressible external flow (simpleFoam, k-omega SST, snappyHexMesh) around the part in a "
                  "virtual wind tunnel; optional moving floor; forces on the part only.";
              j["defaults"] = cfd::toJson(cfd::Settings{});
              j["runsDirectory"] = runs(ctx).root().string();
              j["imagePull"] = runs(ctx).imagePullStatus();
              j["setup"] = setupJson(cfd::planEngineSetup(env, cfd::currentPlatform(), cfd::findSetupTools()));
              return j;
          });

    r.add({"simulation.set_engine", "Set simulation engine",
           "Chooses where simulations run: on this machine (remoteHost empty) or on another machine with Docker, "
           "reached over SSH (remoteHost \"ssh://user@host\"; key-based login, the user may run docker there). The "
           "connection is tested first and the setting is saved only when Docker answers there. Returns the "
           "environment of the chosen engine.",
           objectSchema({{"remoteHost",
                          {{"type", "string"},
                           {"description", "ssh://user@host or ssh://user@host:port; empty = this machine."}}}},
                        {"remoteHost"})},
          [](CommandContext& ctx, const Json& params) {
              const std::string host = params.at("remoteHost").get<std::string>();
              if (!cfd::isValidEngineHost(host))
                  throw CommandError("remoteHost must be ssh://user@host (optionally :port), or empty");
              const cfd::Environment env = cfd::detectEnvironment(host);
              if (!host.empty() && !env.dockerRunning)
                  throw CommandError(env.message + (env.remoteError.empty() ? "" : " (" + env.remoteError + ")"));
              try {
                  cfd::setEngineHost(host);
              } catch (const std::exception& e) {
                  throw CommandError(e.what());
              }
              Json j = env.toJson();
              j["runsDirectory"] = runs(ctx).root().string();
              return j;
          });

    r.add({"simulation.install_engine", "Install simulation engine",
           "Does the next step to get a simulation engine (\"Install for me\"): downloads the OpenFOAM image when "
           "Docker runs; on macOS starts Colima, installing the docker command line tool and Colima with Homebrew "
           "first when missing (in a Terminal window, no Docker Desktop); otherwise opens an install page. Returns "
           "the step taken; call "
           "simulation.environment to follow the download (imagePull).",
           objectSchema(Json::object())},
          [](CommandContext& ctx, const Json&) {
              const cfd::Environment env = cfd::detectEnvironment();
              const cfd::SetupStep step = cfd::planEngineSetup(env, cfd::currentPlatform(), cfd::findSetupTools());
              Json j = setupJson(step);
              try {
                  if (step.action == "pull") {
                      j["started"] = runs(ctx).startImagePull(env.docker, env.remoteHost);
                  } else if (!step.command.empty()) {
                      const auto out = cfd::runCommand(step.command, std::chrono::seconds(20));
                      if (out.exitCode != 0)
                          throw CommandError("Could not " + step.action + ": " + out.output);
                      j["started"] = true;
                  } else {
                      j["started"] = false;
                  }
              } catch (const CommandError&) {
                  throw;
              } catch (const std::exception& e) {
                  throw CommandError(e.what());
              }
              j["imagePull"] = runs(ctx).imagePullStatus();
              return j;
          });

    r.add({"simulation.propose_run", "Propose simulation",
           "Prepares the settings of a wind tunnel run for a part without starting it: resolved settings, "
           "reference area, domain and engine check, so they can be reviewed before simulation.run.",
           objectSchema(proposeParams(), {"id"})},
          [](CommandContext& ctx, const Json& given) {
              Json params = given;
              const bool checkEngine = params.value("checkEngine", true);
              params.erase("checkEngine");
              const RunInput in = collectInput(ctx, params);
              cfd::CaseInfo plan;
              try {
                  plan = cfd::planCase(in.geometry, in.settings);
              } catch (const std::exception& e) {
                  throw CommandError(e.what());
              }
              Json j{{"part", {{"id", in.source.partId}, {"name", in.source.partName}}},
                     {"context", in.source.contextPartIds},
                     {"settings", cfd::toJson(in.settings)},
                     {"referenceArea", plan.referenceArea},
                     {"referenceLength", plan.referenceLength},
                     {"characteristicLength", plan.characteristicLength},
                     {"baseCellSize", plan.baseCellSize},
                     {"domainSize", {plan.domain.size().x, plan.domain.size().y, plan.domain.size().z}},
                     {"domainModel", modelBox(plan.frame, plan.domain)},
                     {"partModel", modelBox(plan.frame, plan.partBounds)},
                     {"flowModel", flowVector(plan.frame)},
                     {"environment", checkEngine ? cfd::detectEnvironment().toJson() : Json()},
                     {"warnings", partWarnings(plan.characteristicLength, in.geometry.part)},
                     {"next", "Call simulation.run with the same parameters to start it."}};
              return j;
          });

    r.add({"simulation.run", "Run simulation",
           "Starts a wind tunnel simulation (OpenFOAM) of a part in the background and returns the run id at once. "
           "Follow it with simulation.status, stop it with simulation.cancel, read it with simulation.results. "
           "Units: settings in SI (m/s, m^2); the part geometry is converted from mm.",
           objectSchema(
               [] {
                   Json p = runParams();
                   p["backend"] = {{"type", "string"},
                                   {"enum", {"docker", "local"}},
                                   {"description", "Engine (default: the one simulation.environment recommends)."}};
                   p["prepareOnly"] = {{"type", "boolean"},
                                       {"description", "Only write the OpenFOAM case (to run it elsewhere)."}};
                   return p;
               }(),
               {"id"})},
          [](CommandContext& ctx, const Json& params) {
              const RunInput in = collectInput(ctx, params);
              std::string id;
              try {
                  if (params.value("prepareOnly", false)) {
                      id = runs(ctx).prepare(in.geometry, in.settings, in.source);
                  } else {
                      std::optional<cfd::Backend> backend;
                      if (params.contains("backend"))
                          backend = cfd::parseBackend(params.at("backend").get<std::string>());
                      id = runs(ctx).start(in.geometry, in.settings, in.source, backend);
                  }
              } catch (const std::exception& e) {
                  throw CommandError(e.what());
              }
              Json j = statusJson(ctx, *runs(ctx).status(id));
              const auto info = runs(ctx).caseInfo(id);
              j["warnings"] = partWarnings(info ? info->characteristicLength : 0.0, in.geometry.part);
              return j;
          });

    r.add({"simulation.refine", "Run on another mesh",
           "Starts the same simulation as an earlier run (same part, surroundings and conditions) on another mesh "
           "level, to check that a result or an A/B difference holds under mesh refinement. The part must still be "
           "in the open document. Returns the new run.",
           objectSchema({{"run", runIdParam()},
                         {"mesh",
                          {{"type", "string"},
                           {"enum", {"coarse", "medium", "fine"}},
                           {"description", "Mesh level (default: the next finer one)."}}},
                         {"prepareOnly", {{"type", "boolean"}, {"description", "Only write the OpenFOAM case."}}}},
                        {"run"})},
          [](CommandContext& ctx, const Json& params) {
              const std::string id = requireRun(ctx, params);
              const auto status = runs(ctx).status(id);
              const auto info = runs(ctx).caseInfo(id);
              if (!info)
                  throw CommandError("Run '" + id + "' has no case information");
              if (!sourceInDocument(ctx, status->source))
                  throw CommandError("The part of run '" + id + "' is not in the open document (or has changed)");
              Json settings = cfd::toJson(info->settings);
              const auto current = static_cast<int>(info->settings.mesh);
              const std::string mesh = params.contains("mesh")
                                           ? params.at("mesh").get<std::string>()
                                           : std::string(cfd::meshLevelName(
                                                 static_cast<cfd::MeshLevel>(std::min(current + 1, 2))));
              if (mesh == settings.at("mesh").get<std::string>())
                  throw CommandError("Run '" + id + "' already uses the " + mesh + " mesh");
              settings["mesh"] = mesh;
              settings["iterations"] = 0; // Default for the new level.
              settings.erase("processes");
              settings["id"] = status->source.partId;
              settings["context"] = status->source.contextPartIds;
              settings["label"] = status->source.label;
              const RunInput in = collectInput(ctx, settings);
              // The surroundings must also be unchanged, or this is a different case.
              if (cfd::caseGeometryHash(in.geometry, in.settings) != info->geometryHash)
                  throw CommandError("The geometry of run '" + id +
                                     "' has changed since it ran (the part or the other parts in the flow): start a "
                                     "new run instead");
              std::string newId;
              try {
                  newId = params.value("prepareOnly", false) ? runs(ctx).prepare(in.geometry, in.settings, in.source)
                                                             : runs(ctx).start(in.geometry, in.settings, in.source);
              } catch (const std::exception& e) {
                  throw CommandError(e.what());
              }
              return statusJson(ctx, *runs(ctx).status(newId));
          });

    r.add({"simulation.status", "Simulation status",
           "State (running, finished, failed, cancelled, interrupted), step, progress 0..1, iteration and the latest "
           "Cd/Cl and forces in N of a run.",
           objectSchema({{"run", runIdParam()}}, {"run"})},
          [](CommandContext& ctx, const Json& params) {
              return statusJson(ctx, *runs(ctx).status(requireRun(ctx, params)));
          });

    r.add({"simulation.list", "List simulations",
           "Lists all simulation runs, oldest first. Only reads: a remote run started by another AeroCAD "
           "session is listed but not followed here (that session follows it, or this app after a restart).",
           objectSchema(Json::object())},
          [](CommandContext& ctx, const Json&) {
              runs(ctx).rescan();
              Json out = Json::array();
              for (const auto& s : runs(ctx).list())
                  out.push_back(statusJson(ctx, s));
              return Json{{"runs", std::move(out)}};
          });

    r.add({"simulation.cancel", "Cancel simulation", "Stops a running simulation.",
           objectSchema({{"run", runIdParam()}}, {"run"})},
          [](CommandContext& ctx, const Json& params) {
              const std::string id = requireRun(ctx, params);
              if (!runs(ctx).cancel(id))
                  throw CommandError("Run '" + id + "' is not running, or it runs on a local OpenFOAM started by "
                                     "an earlier session of the app (stop its Allrun process manually)");
              return statusJson(ctx, *runs(ctx).status(id));
          });

    r.add({"simulation.delete", "Delete simulation", "Deletes a run that is not running, with its files.",
           objectSchema({{"run", runIdParam()}}, {"run"})},
          [](CommandContext& ctx, const Json& params) {
              const std::string id = requireRun(ctx, params);
              if (!runs(ctx).remove(id))
                  throw CommandError("Run '" + id + "' is still running; cancel it first");
              return Json{{"deleted", id}};
          });

    r.add({"simulation.rename", "Rename simulation run",
           "Sets the label of a run (the name shown in the run list, Results and A/B), also while it runs. "
           "An empty label clears it. Saved with the run.",
           objectSchema(
               {{"run", runIdParam()},
                {"label", {{"type", "string"}, {"maxLength", 80}, {"description", "New label; \"\" clears it."}}}},
               {"run", "label"})},
          [](CommandContext& ctx, const Json& params) {
              const std::string id = requireRun(ctx, params);
              std::string label = params.at("label").get<std::string>();
              const auto first = label.find_first_not_of(" \t\r\n");
              label = first == std::string::npos ? std::string()
                                                 : label.substr(first, label.find_last_not_of(" \t\r\n") - first + 1);
              if (label.find_first_of("\r\n") != std::string::npos)
                  throw CommandError("label must be one line");
              // Characters, not UTF-8 bytes: count the bytes that start a character.
              if (std::count_if(label.begin(), label.end(), [](char c) { return (c & 0xC0) != 0x80; }) > 80)
                  throw CommandError("label is longer than 80 characters");
              try {
                  if (!runs(ctx).setLabel(id, label))
                      throw CommandError("No simulation run '" + id + "'");
              } catch (const CommandError&) {
                  throw;
              } catch (const std::exception& e) {
                  throw CommandError(e.what());
              }
              return Json{{"run", id}, {"label", label}};
          });

    r.add({"simulation.results", "Simulation results",
           "Results of a run: summary (Cd, Cl, Cs; drag, lift, side force in N, averaged over the last 20 % of the "
           "iterations), histories per iteration, residuals, quality checks (mesh, mass balance, residuals, force "
           "convergence) and the simulation conditions. Summary values are null until the run finishes successfully "
           "with complete finite force/coefficient outputs; finalResults gives availability and reasons. "
           "Histories remain available and provisional is true until final results are available. While the run is "
           "running, current gives the same means over the iterations recorded so far (never final; null otherwise).",
           objectSchema({{"run", runIdParam()},
                         {"maxPoints",
                          {{"type", "integer"},
                           {"minimum", 2},
                           {"maximum", 5000},
                           {"description", "Maximum points per history (default 200)."}}}},
                        {"run"})},
          [](CommandContext& ctx, const Json& params) {
              const std::string id = requireRun(ctx, params);
              try {
                  return runs(ctx).results(id, params.value("maxPoints", 200));
              } catch (const std::exception& e) {
                  throw CommandError(e.what());
              }
          });

    r.add({"simulation.export_results", "Export simulation results (CSV)",
           "Write a run's results to a CSV file: a Quantity,Value block with the run, its conditions and its "
           "final values (Cd, Cl, Cs; drag, lift, side, pressure and viscous drag in N; or the provisional means "
           "while it is not finished), a blank line, then the full history per iteration. Returns the path and "
           "the number of history rows.",
           objectSchema({{"run", runIdParam()},
                         {"path", {{"type", "string"}, {"description", "File to write (.csv), overwritten."}}}},
                        {"run", "path"})},
          [](CommandContext& ctx, const Json& params) {
              const std::string id = requireRun(ctx, params);
              const std::string path = params.at("path").get<std::string>();
              if (path.empty())
                  throw CommandError("path is empty");
              try {
                  const std::size_t rows = runs(ctx).exportResultsCsv(id, core::pathFromUtf8(path));
                  return Json{{"path", path}, {"rows", rows}};
              } catch (const std::exception& e) {
                  throw CommandError(e.what());
              }
          });

    r.add({"simulation.field", "Simulation field summary",
           "Statistics of a result field in model coordinates (mm): surface (pressure on the part: p, Cp, "
           "pressure in Pa), sectionHorizontal / sectionVertical (velocity sections through the part centre: speed "
           "in m/s, Cp), a plane of a section stack (sectionX_<i> across the flow, sectionY_<i> vertical along "
           "it, sectionZ_<i> horizontal; positions in simulation.results fields.sections), wake (isosurface of zero "
           "total pressure, Cp0 = 0, around and behind the part: speed, Cp), vortices (vortex cores: isosurface of "
           "the Q-criterion at 20 (U / L)^2, from three surface cells off the part: speed, Cp) or streamlines. "
           "available = false until the run has written it. Scalars have min, max, "
           "mean and the 2nd/98th percentiles p02/p98. With compare (run B) and kind surface: the pressure "
           "difference B - A on run's part surface (dCp, dpressure in Pa), NaN where B's part has no surface "
           "within 1 % of the part size.",
           objectSchema({{"run", runIdParam()},
                         {"kind",
                          {{"type", "string"},
                           {"pattern", "^(surface|sectionHorizontal|sectionVertical|wake|vortices|streamlines|section[XYZ]_[0-9]+)$"}}},
                         {"compare", runIdParam("Run B: return the surface difference B - A.")}},
                        {"run", "kind"})},
          [](CommandContext& ctx, const Json& params) {
              const std::string id = requireRun(ctx, params);
              try {
                  const std::string kind = params.at("kind").get<std::string>();
                  if (params.contains("compare") && kind != "surface")
                      throw CommandError("compare works with kind surface only");
                  const auto data = params.contains("compare")
                                        ? runs(ctx).surfaceDifference(id, requireRun(ctx, params, "compare"))
                                        : runs(ctx).field(id, kind);
                  if (data.empty())
                      return Json{{"available", false}}; // Written at the end of a run.
                  Json j = polyDataSummary(data);
                  j["available"] = true;
                  return j;
              } catch (const std::exception& e) {
                  throw CommandError(e.what());
              }
          });

    r.add({"simulation.compare", "Compare simulations",
           "A/B comparison of two runs: drag/lift/side force differences in N and %, coefficients on a shared "
           "reference area, whether the conditions match (settings such as speed, direction, mesh and preset, and "
           "the resolved setup: surroundings/context geometry, flow domain and engine; unknownConditions lists what an "
           "older run did not record), quality of both, "
           "and shared colour scale ranges. Both runs need successful completion and complete finite force/coefficient "
           "outputs; otherwise verdict is not comparable, deltas/scales are empty, and states/reasons explain why. "
           "Verdict \"different domains\" when the only difference is the flow domain of a target-part variant. "
           "meshSensitivity tells whether the difference holds on another mesh level (A and B run there too, see "
           "simulation.refine); verdict \"difference inconclusive\" when refinement changes it about as much as its "
           "size.",
           objectSchema({{"a", runIdParam("Run A (baseline).")}, {"b", runIdParam("Run B (variant).")}}, {"a", "b"})},
          [](CommandContext& ctx, const Json& params) {
              const std::string a = requireRun(ctx, params, "a");
              const std::string b = requireRun(ctx, params, "b");
              try {
                  return runs(ctx).compare(a, b);
              } catch (const std::exception& e) {
                  throw CommandError(e.what());
              }
          });

    r.add({"simulation.import", "Import simulation",
           "Registers an AeroCAD OpenFOAM case directory that was run outside the app (for example on a cluster) "
           "so its results can be viewed and compared.",
           objectSchema({{"path", {{"type", "string"}, {"minLength", 1}, {"description", "Case directory."}}},
                         {"label", {{"type", "string"}}}},
                        {"path"})},
          [](CommandContext& ctx, const Json& params) {
              try {
                  const std::string id = runs(ctx).importCase(core::pathFromUtf8(params.at("path").get<std::string>()),
                                                              params.value("label", std::string()));
                  return statusJson(ctx, *runs(ctx).status(id));
              } catch (const std::exception& e) {
                  throw CommandError(e.what());
              }
          });
}

} // namespace aerocad::commands
