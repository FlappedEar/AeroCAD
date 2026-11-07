// SPDX-License-Identifier: GPL-3.0-or-later
// Wind tunnel commands (simulation.*): run OpenFOAM on a part in the background, follow
// the run, read the results and compare two runs. The MCP server exposes them as
// simulation_run, simulation_status, simulation_compare, ... so an agent can do what
// the CFD panel does.
#include "aerocad/commands/Session.hpp"

#include "aerocad/cfd/RunManager.hpp"
#include "aerocad/core/Path.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace aerocad::commands {

namespace {

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
        if (cp->mesh && cp->kind != model::PartKind::Sketch)
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
        if (n > 0)
            scalars[name] = {{"min", lo}, {"max", hi}, {"mean", sum / static_cast<double>(n)}};
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
                      j["started"] = runs(ctx).startImagePull(env.docker);
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
           objectSchema(runParams(), {"id"})},
          [](CommandContext& ctx, const Json& params) {
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
                     {"domainSize",
                      {plan.domain.size().x, plan.domain.size().y, plan.domain.size().z}},
                     {"environment", cfd::detectEnvironment().toJson()},
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
              return statusJson(ctx, *runs(ctx).status(id));
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

    r.add({"simulation.list", "List simulations", "Lists all simulation runs, oldest first.",
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

    r.add({"simulation.results", "Simulation results",
           "Results of a run: summary (Cd, Cl, Cs; drag, lift, side force in N, averaged over the last 20 % of the "
           "iterations), histories per iteration, residuals, quality checks (mesh, mass balance, residuals, force "
           "convergence) and the simulation conditions.",
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

    r.add({"simulation.field", "Simulation field summary",
           "Statistics of a result field in model coordinates (mm): surface (pressure on the part: p, Cp, "
           "pressure in Pa), sectionHorizontal / sectionVertical (velocity sections through the part centre: speed "
           "in m/s, Cp) or streamlines. available = false until the run has written it.",
           objectSchema({{"run", runIdParam()},
                         {"kind",
                          {{"type", "string"},
                           {"enum", {"surface", "sectionHorizontal", "sectionVertical", "streamlines"}}}}},
                        {"run", "kind"})},
          [](CommandContext& ctx, const Json& params) {
              const std::string id = requireRun(ctx, params);
              try {
                  const auto data = runs(ctx).field(id, params.at("kind").get<std::string>());
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
           "reference area, whether the conditions (speed, direction, mesh, preset, ...) match, quality of both, "
           "and shared colour scale ranges. For a variant without a finished run, the values are null. "
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
