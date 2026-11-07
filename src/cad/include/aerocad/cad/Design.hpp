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
    std::string type; ///< sketch, extrude, revolve, fillet, chamfer, shell, combine, box, cylinder,
                      ///< base_body, mirror, rectangular_pattern, circular_pattern, loft, sweep, plane,
                      ///< part_on_surface, fin_on_surface (evaluated by a FeatureEvaluator)
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
    /// What an evaluated feature reports besides its bodies (fit deviation, warnings), from
    /// a FeatureEvaluator; null otherwise.
    Json report;
};

/// The id of the k-th body (k >= 1) a feature makes besides its first, whose id is the
/// feature id (e.g. a stacked, mirrored set of fins from one sketch).
[[nodiscard]] constexpr FeatureId extraBodyId(FeatureId feature, unsigned k)
{
    return feature | (static_cast<FeatureId>(k) << 40);
}
/// The feature that made a body (see extraBodyId).
[[nodiscard]] constexpr FeatureId featureOfBody(FeatureId body)
{
    return body & ((static_cast<FeatureId>(1) << 40) - 1);
}

/// Feature types whose geometry the timeline cannot build on its own: it needs document
/// parts such as a scanned mesh (part_on_surface, fin_on_surface). The commands layer
/// supplies them; the timeline stores, orders, regenerates and caches them like any other
/// feature.
struct FeatureEvaluator {
    struct Output {
        /// Bodies made, first one first; each with its default name.
        std::vector<std::pair<geometry::Shape, std::string>> bodies;
        /// Stable slot of each body (its id is extraBodyId(feature, slot), slot 0 the feature
        /// id), so a body keeps its id when bodies before it come and go. Empty: by position.
        std::vector<unsigned> bodySlots;
        Json report;
    };
    /// Fingerprint of the document inputs (parts the feature reads, by content); with the
    /// params, user parameters and the sketch it uses, it decides when a cached result can
    /// be reused.
    std::function<std::string(const Feature&)> inputs;
    /// Builds the bodies; throws on failure (the message is the feature's error).
    std::function<Output(const Feature&, const class Design&)> evaluate;
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

/// Construction plane (Fusion: Construct > Offset Plane, Plane at Angle, Midplane,
/// Plane Through Three Points). Sketches can be placed on it.
struct ConstructionPlane {
    FeatureId id = 0;
    std::string name;
    geometry::Plane plane;
    /// Half the edge length of the square drawn in the view (mm).
    double size = 25.0;
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
    /// The feature an id stands for: the id itself, or the feature that made the body when it
    /// is the id of a second or later body (extraBodyId). Unknown ids come back unchanged.
    [[nodiscard]] FeatureId owningFeature(FeatureId id) const;
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
    [[nodiscard]] const std::map<FeatureId, ConstructionPlane>& planes() const { return m_planes; }
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

    /// Part bookkeeping retained by syncDocument(): every timeline feature that had a
    /// document part in this session, including features that are now suppressed, rolled
    /// back or only exist in an inactive variant. Name / visibility are as of the last sync.
    struct PartInfo {
        FeatureId feature = 0;
        model::PartId part = 0;
        std::string name;
        bool visible = true;
    };
    [[nodiscard]] std::vector<PartInfo> partInfos() const;

    /// Gives the timeline access to document parts (for "base_body" features that turn an
    /// imported part into a body). Not part of the undoable state.
    using PartLookup = std::function<const model::Part*(model::PartId)>;
    void setPartLookup(PartLookup lookup) { m_partLookup = std::move(lookup); }

    /// Supplies the geometry of a feature type built from document parts (see
    /// FeatureEvaluator). Not part of the undoable state.
    void setFeatureEvaluator(const std::string& type, FeatureEvaluator evaluator);

    /// A FeatureEvaluator result with the fingerprint of its inputs, as stored in project
    /// files so that opening a project does not rebuild steps made on a scan.
    struct StoredResult {
        FeatureId feature = 0;
        std::string key;
        FeatureEvaluator::Output output;
    };
    /// The evaluator results the current model uses, one per evaluated feature.
    [[nodiscard]] std::vector<StoredResult> currentResults() const;
    /// Makes a stored result available to the next regeneration (after clear(), before
    /// restore() of a loaded project). Used only if its key still matches the inputs.
    void seedResult(StoredResult result);

    /// Number of times evaluators ran (cache misses), for tests.
    [[nodiscard]] std::size_t evaluatorRuns() const { return m_evaluatorRuns; }

    /// Feature types that the timeline knows.
    [[nodiscard]] static const std::vector<std::string>& featureTypes();

private:
    void evaluateFeature(const Feature& f);
    void evaluateExternal(const Feature& f, const FeatureEvaluator& ev);
    [[nodiscard]] std::string defaultName(const std::string& type) const;
    [[nodiscard]] std::size_t indexOf(FeatureId id) const;
    /// Regenerates and throws if feature `id` failed (used to reject bad edits).
    void regenerateChecked(FeatureId id, const State& rollback);

    State m_state;
    std::map<FeatureId, Body> m_bodies;
    std::map<FeatureId, SketchResult> m_sketches;
    std::map<FeatureId, ConstructionPlane> m_planes;
    std::map<FeatureId, FeatureStatus> m_statuses;
    std::map<FeatureId, Json> m_reports;
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
    /// Surfaces fitted to the part of a scan under a body (Combine with a scan as tool):
    /// cached per part, revision and region, since previews regenerate often.
    struct ScanSurface {
        model::PartId part = 0;
        std::uint64_t revision = 0;
        core::BoundingBox region;
        geometry::Shape face;
    };
    std::vector<ScanSurface> m_scanSurfaces;
    [[nodiscard]] geometry::Shape scanSurfaceUnder(const model::Part& scan, const core::BoundingBox& body);
    std::map<std::string, FeatureEvaluator> m_evaluators;
    /// Results of evaluators by feature, keyed by their inputs; the last few are kept so
    /// undo / redo and roll back do not recompute.
    struct EvaluatedFeature {
        std::string key;
        FeatureEvaluator::Output output;
    };
    std::map<FeatureId, std::vector<EvaluatedFeature>> m_evaluated;
    /// Key of the result each evaluated feature used in the last regeneration.
    std::map<FeatureId, std::string> m_usedKeys;
    std::size_t m_evaluatorRuns = 0;
    int m_bodyCounter = 0;
};

[[nodiscard]] std::string_view toString(FeatureStatus::State state);

/// JSON form of the undoable design state (features, parameters, frames, marker), as
/// stored in project files.
[[nodiscard]] Json stateToJson(const Design::State& state);
/// Reads stateToJson() output. Throws DesignError for malformed data.
[[nodiscard]] Design::State stateFromJson(const Json& json);

} // namespace aerocad::cad
