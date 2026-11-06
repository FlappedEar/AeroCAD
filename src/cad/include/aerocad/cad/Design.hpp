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
#include <functional>
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
    std::string type; ///< sketch, extrude, revolve, fillet, chamfer, shell, combine, box, cylinder, base_body
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

/// A named reference frame (work plane): an origin with an orthonormal frame. Sketches can
/// use it as their plane by name (sketch.create plane: "<frame name>"). Typical use: the
/// alignment of a scanned or imported reference model.
struct ReferenceFrame {
    std::string name;
    core::Vec3 origin;
    core::Vec3 normal{0, 0, 1}; ///< Z axis of the frame; the sketch plane normal.
    core::Vec3 xDir{1, 0, 0};   ///< X axis of the frame (made orthogonal to the normal).
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
    /// Degrees of freedom left by the constraints (0 = fully constrained).
    int degreesOfFreedom = 0;
    /// Number of user constraints and dimensions (params "constraints").
    std::size_t constraintCount = 0;
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
        std::vector<ReferenceFrame> frames;
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

    /// Creates or replaces a reference frame (by name) and regenerates. Throws DesignError
    /// for an invalid name (empty, XY / XZ / YZ) or degenerate axes; the design is
    /// unchanged then. The axes are normalised, xDir is made orthogonal to the normal.
    void setFrame(ReferenceFrame frame);
    void removeFrame(const std::string& name);
    [[nodiscard]] const std::vector<ReferenceFrame>& frames() const { return m_state.frames; }
    [[nodiscard]] const ReferenceFrame* frame(std::string_view name) const;

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

    /// Empties the design (new / opened project): state, cached results and the part
    /// bookkeeping of syncDocument().
    void clear();

    /// Seeds the document part of a timeline feature before the first syncDocument() of a
    /// loaded project, so parts keep their saved ids, names and visibility.
    void setPartInfo(FeatureId feature, model::PartId part, std::string name, bool visible);

    /// Gives the timeline access to document parts (for "base_body" features that turn an
    /// imported part into a body). Not part of the undoable state.
    using PartLookup = std::function<const model::Part*(model::PartId)>;
    void setPartLookup(PartLookup lookup) { m_partLookup = std::move(lookup); }

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
    struct PartMeta {
        std::string name;
        bool visible = true;
    };
    std::map<FeatureId, PartMeta> m_partMeta;
    PartLookup m_partLookup;
    /// Mesh to B-rep conversions are slow: cached per part id and revision.
    struct ConvertedPart {
        std::uint64_t revision = 0;
        geometry::Shape shape;
    };
    std::map<model::PartId, ConvertedPart> m_convertedParts;
    int m_bodyCounter = 0;
};

[[nodiscard]] std::string_view toString(FeatureStatus::State state);

/// JSON form of the undoable design state (features, parameters, frames, marker), as
/// stored in project files.
[[nodiscard]] Json stateToJson(const Design::State& state);
/// Reads stateToJson() output. Throws DesignError for malformed data.
[[nodiscard]] Design::State stateFromJson(const Json& json);

} // namespace aerocad::cad
