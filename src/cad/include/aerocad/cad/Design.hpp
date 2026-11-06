// SPDX-License-Identifier: GPL-3.0-or-later
// The parametric design: an ordered timeline of features (sketch, extrude, fillet, ...)
// with user parameters. Regeneration replays the timeline and produces bodies and
// sketches, which are then mirrored into the document as parts.
#pragma once

#include "aerocad/geometry/Shape.hpp"
#include "aerocad/geometry/Sketch.hpp"
#include "aerocad/model/Document.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace aerocad::cad {

using Json = nlohmann::json;
using FeatureId = std::uint64_t;

/// Invalid design operation (unknown feature, bad parameter, failed feature, ...).
class DesignError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// One step of the timeline. `params` follow the schema of the feature type (see
/// docs/cad.md); numbers may be given as expressions using user parameters.
struct Feature {
    FeatureId id = 0;
    std::string type; ///< sketch, extrude, revolve, fillet, chamfer, shell, combine, box, cylinder
    std::string name; ///< e.g. "Extrude1"
    Json params = Json::object();
    bool suppressed = false;
};

/// A named user parameter ("Change Parameters" in Fusion).
struct Parameter {
    std::string name;
    std::string expression;
    std::string comment;
};

/// Result of evaluating one feature during regeneration.
struct FeatureStatus {
    enum class State { Ok, Error, Suppressed, RolledBack };
    State state = State::Ok;
    std::string message;
};

/// A solid produced by the timeline. Its id is the id of the feature that created it.
struct Body {
    FeatureId id = 0;
    std::string name;
    geometry::Shape shape;
};

struct SketchResult {
    FeatureId id = 0;
    std::string name;
    geometry::Plane plane;
    std::vector<geometry::SketchCurve> curves;
    geometry::SketchGeometry geometry;
    /// True when a later active feature uses its profiles (Fusion then hides it).
    bool consumed = false;
};

class Design {
public:
    /// Everything that is undoable. Cheap to copy.
    struct State {
        std::vector<Feature> features;
        std::vector<Parameter> parameters;
        FeatureId nextId = 1;
        /// Number of features before the timeline marker; features after it are rolled
        /// back (not evaluated). New features are inserted at the marker.
        std::size_t marker = 0;
    };

    [[nodiscard]] const State& state() const { return m_state; }
    /// Replaces the state (undo / redo / load) and regenerates.
    void restore(State state);

    [[nodiscard]] const std::vector<Feature>& features() const { return m_state.features; }
    [[nodiscard]] const Feature* feature(FeatureId id) const;
    [[nodiscard]] std::size_t marker() const { return m_state.marker; }

    /// Inserts a feature at the marker, moves the marker after it and regenerates.
    /// Throws DesignError (and leaves the design unchanged) when the new feature fails.
    FeatureId addFeature(std::string type, Json params, std::string name = {});
    /// Merges `patch` into the feature's params (JSON merge patch) and regenerates.
    /// Throws DesignError (and leaves the design unchanged) when the feature then fails.
    void editFeature(FeatureId id, const Json& patch);
    void renameFeature(FeatureId id, std::string name);
    void setSuppressed(FeatureId id, bool suppressed);
    /// Removes a feature. Later features that depended on it will report errors.
    void removeFeature(FeatureId id);
    /// Moves the timeline marker (0 = before the first feature, size() = at the end).
    void setMarker(std::size_t marker);

    /// Creates or updates a user parameter. Throws DesignError for invalid names, invalid
    /// expressions or cycles; the design is unchanged then.
    void setParameter(const std::string& name, const std::string& expression, const std::string& comment = {});
    void removeParameter(const std::string& name);
    [[nodiscard]] const std::vector<Parameter>& parameters() const { return m_state.parameters; }
    /// Value of a parameter (mm or degrees), std::nullopt if unknown.
    [[nodiscard]] std::optional<double> parameterValue(std::string_view name) const;

    /// Evaluates a number or an expression string with the user parameters.
    [[nodiscard]] double evaluate(const Json& value) const;

    /// Replays the timeline. Never throws: failing features get an error status.
    void regenerate();

    [[nodiscard]] const std::map<FeatureId, Body>& bodies() const { return m_bodies; }
    [[nodiscard]] const std::map<FeatureId, SketchResult>& sketches() const { return m_sketches; }
    [[nodiscard]] const std::map<FeatureId, FeatureStatus>& statuses() const { return m_statuses; }
    [[nodiscard]] FeatureStatus status(FeatureId id) const;

    /// Mirrors bodies and sketches into the document: adds, updates and removes parts of
    /// kind Body / Sketch. Part ids stay stable for a feature across regenerations,
    /// names and visibility set by the user are kept.
    void syncDocument(model::Document& document);

    /// Feature types that the timeline knows.
    [[nodiscard]] static const std::vector<std::string>& featureTypes();

private:
    void evaluateFeature(const Feature& f);
    [[nodiscard]] std::string defaultName(const std::string& type) const;
    [[nodiscard]] std::size_t indexOf(FeatureId id) const;
    /// Regenerates and throws if feature `id` failed (used to reject bad edits).
    void regenerateChecked(FeatureId id, const State& rollback);

    State m_state;
    std::map<FeatureId, Body> m_bodies;
    std::map<FeatureId, SketchResult> m_sketches;
    std::map<FeatureId, FeatureStatus> m_statuses;
    /// Body / sketch feature id -> document part id (kept across undo).
    std::map<FeatureId, model::PartId> m_partIds;
    std::map<FeatureId, bool> m_sketchConsumed;
    int m_bodyCounter = 0;
};

[[nodiscard]] std::string_view toString(FeatureStatus::State state);

} // namespace aerocad::cad
