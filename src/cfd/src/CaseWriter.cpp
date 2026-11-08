// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cfd/CaseWriter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace aerocad::cfd {

namespace fs = std::filesystem;
using core::BoundingBox;
using core::TriangleMesh;
using core::Vec3;

// --- CaseFrame --------------------------------------------------------------------------

CaseFrame CaseFrame::forFlow(std::string_view flowDirection, const Vec3& origin)
{
    CaseFrame f;
    f.origin = origin;
    // Rotation (counter-clockwise about +Z) that turns the flow direction into +X.
    if (flowDirection == "+X")
        f.quarterTurns = 0;
    else if (flowDirection == "+Y")
        f.quarterTurns = 3; // +Y -> +X is -90 degrees.
    else if (flowDirection == "-X")
        f.quarterTurns = 2;
    else if (flowDirection == "-Y")
        f.quarterTurns = 1;
    else
        throw std::invalid_argument("flowDirection must be one of +X, -X, +Y, -Y");
    return f;
}

namespace {

Vec3 rotateQuarter(const Vec3& v, int turns)
{
    switch (((turns % 4) + 4) % 4) {
    case 1: return {-v.y, v.x, v.z};
    case 2: return {-v.x, -v.y, v.z};
    case 3: return {v.y, -v.x, v.z};
    default: return v;
    }
}

} // namespace

Vec3 CaseFrame::rotate(const Vec3& v) const { return rotateQuarter(v, quarterTurns); }
Vec3 CaseFrame::unrotate(const Vec3& v) const { return rotateQuarter(v, -quarterTurns); }
Vec3 CaseFrame::toCase(const Vec3& modelMm) const { return rotate(modelMm - origin) * 0.001; }
Vec3 CaseFrame::toModel(const Vec3& caseM) const { return unrotate(caseM * 1000.0) + origin; }

namespace {

Json vec(const Vec3& v) { return Json::array({v.x, v.y, v.z}); }
Vec3 vecFrom(const Json& j) { return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()}; }

Json boxJson(const BoundingBox& b) { return Json{{"min", vec(b.min)}, {"max", vec(b.max)}}; }
BoundingBox boxFrom(const Json& j)
{
    BoundingBox b;
    b.extend(vecFrom(j.at("min")));
    b.extend(vecFrom(j.at("max")));
    return b;
}

} // namespace

Json CaseFrame::toJson() const { return Json{{"origin", vec(origin)}, {"quarterTurns", quarterTurns}, {"scale", 0.001}}; }

CaseFrame CaseFrame::fromJson(const Json& j)
{
    CaseFrame f;
    f.origin = vecFrom(j.at("origin"));
    f.quarterTurns = j.at("quarterTurns").get<int>();
    return f;
}

Json CaseInfo::toJson() const
{
    return Json{
        {"openfoam", kOpenFoamVersion},
        {"frame", frame.toJson()},
        {"settings", cfd::toJson(settings)},
        {"referenceArea", referenceArea},
        {"referenceLength", referenceLength},
        {"characteristicLength", characteristicLength},
        {"partBounds", boxJson(partBounds)},
        {"domain", boxJson(domain)},
        {"baseCellSize", baseCellSize},
        {"geometryHash", geometryHash},
        {"contextHash", contextHash ? Json(*contextHash) : Json(nullptr)},
        {"wheels", [&] {
             Json list = Json::array();
             for (const Wheel& w : wheels)
                 list.push_back(w.toJson());
             return list;
         }()},
    };
}

CaseInfo CaseInfo::fromJson(const Json& j)
{
    CaseInfo c;
    c.frame = CaseFrame::fromJson(j.at("frame"));
    c.settings = settingsFromJson(j.at("settings"));
    c.referenceArea = j.at("referenceArea").get<double>();
    c.referenceLength = j.at("referenceLength").get<double>();
    c.characteristicLength = j.value("characteristicLength", 0.0);
    c.partBounds = boxFrom(j.at("partBounds"));
    c.domain = boxFrom(j.at("domain"));
    c.baseCellSize = j.value("baseCellSize", 0.0);
    c.geometryHash = j.value("geometryHash", std::uint64_t{0});
    if (auto it = j.find("contextHash"); it != j.end() && it->is_number_unsigned())
        c.contextHash = it->get<std::uint64_t>();
    c.openfoam = j.value("openfoam", std::string());
    if (auto it = j.find("wheels"); it != j.end() && it->is_array())
        for (const Json& w : *it)
            c.wheels.push_back(Wheel::fromJson(w));
    return c;
}

// --- Frontal area -----------------------------------------------------------------------

double frontalArea(const TriangleMesh& mesh, int resolution)
{
    const BoundingBox b = mesh.bounds();
    if (b.isEmpty() || resolution < 1)
        return 0.0;
    const double y0 = b.min.y;
    const double z0 = b.min.z;
    const double dy = (b.max.y - b.min.y) / resolution;
    const double dz = (b.max.z - b.min.z) / resolution;
    if (dy <= 0.0 || dz <= 0.0)
        return 0.0;
    const auto n = static_cast<std::size_t>(resolution);
    std::vector<char> covered(n * n, 0);
    const auto& v = mesh.vertices();
    for (const auto& t : mesh.triangles()) {
        const double ay = v[t[0]].y, az = v[t[0]].z;
        const double by = v[t[1]].y, bz = v[t[1]].z;
        const double cy = v[t[2]].y, cz = v[t[2]].z;
        const double area2 = (by - ay) * (cz - az) - (cy - ay) * (bz - az);
        if (std::abs(area2) < 1e-30)
            continue;
        auto cell = [](double value, double origin, double step, std::size_t count) {
            const double c = std::floor((value - origin) / step);
            return static_cast<std::size_t>(std::clamp(c, 0.0, static_cast<double>(count - 1)));
        };
        const std::size_t iy0 = cell(std::min({ay, by, cy}), y0, dy, n);
        const std::size_t iy1 = cell(std::max({ay, by, cy}), y0, dy, n);
        const std::size_t iz0 = cell(std::min({az, bz, cz}), z0, dz, n);
        const std::size_t iz1 = cell(std::max({az, bz, cz}), z0, dz, n);
        const double s = area2 > 0 ? 1.0 : -1.0;
        for (std::size_t iz = iz0; iz <= iz1; ++iz) {
            const double pz = z0 + (static_cast<double>(iz) + 0.5) * dz;
            for (std::size_t iy = iy0; iy <= iy1; ++iy) {
                const double py = y0 + (static_cast<double>(iy) + 0.5) * dy;
                const double w0 = s * ((by - ay) * (pz - az) - (py - ay) * (bz - az));
                const double w1 = s * ((cy - by) * (pz - bz) - (py - by) * (cz - bz));
                const double w2 = s * ((ay - cy) * (pz - cz) - (py - cy) * (az - cz));
                if (w0 >= 0 && w1 >= 0 && w2 >= 0)
                    covered[iz * n + iy] = 1;
            }
        }
    }
    const auto count = static_cast<double>(std::count(covered.begin(), covered.end(), char{1}));
    return count * dy * dz;
}

// --- Case writer ------------------------------------------------------------------------

namespace {

/// Mesh resolution parameters of the preset per level.
struct MeshPlan {
    double cellsPerLength;   ///< Background cells per characteristic length.
    int surfaceMin, surfaceMax; ///< snappyHexMesh surface refinement levels.
    int nearLevel;           ///< Refinement level of the box close around the part and its near wake.
    int wakeLevel;           ///< Refinement level of the larger box around the part and its far wake.
    int layers;              ///< Prism layers on the part (0 = none).
};

/// The cells go where the pressure on the part is decided (CAD-153): the far wake, which
/// held three quarters of a fine mesh, is one level coarser than the region just around
/// the part, and the fine surface gets one more level (2.2 mm instead of 4.4 mm on a
/// 1.1 m car, 3.6 times the faces) for about the same cell count.
MeshPlan meshPlan(MeshLevel level)
{
    switch (level) {
    case MeshLevel::Coarse: return {2.0, 3, 4, 2, 1, 0};
    case MeshLevel::Medium: return {3.0, 4, 5, 3, 2, 2};
    case MeshLevel::Fine: return {4.0, 6, 7, 4, 3, 3};
    }
    return {2.0, 3, 4, 2, 1, 0};
}

/// Number formatting independent of the global locale.
class Text {
public:
    Text()
    {
        m_out.imbue(std::locale::classic());
        m_out.precision(10);
    }
    template <class T>
    Text& operator<<(const T& v)
    {
        m_out << v;
        return *this;
    }
    Text& operator<<(const Vec3& v)
    {
        m_out << '(' << v.x << ' ' << v.y << ' ' << v.z << ')';
        return *this;
    }
    [[nodiscard]] std::string str() const { return m_out.str(); }

private:
    std::ostringstream m_out;
};

std::string header(std::string_view cls, std::string_view object)
{
    Text t;
    t << "/* Generated by AeroCAD for OpenFOAM " << kOpenFoamVersion << " (preset " << kExternalFlowPreset
      << "). */\nFoamFile\n{\n    version     2.0;\n    format      ascii;\n    class       " << cls
      << ";\n    object      " << object << ";\n}\n\n";
    return t.str();
}

/// Patch of the wheels on an axle (CAD-154); "wheels0" is the front axle.
std::string wheelPatch(int axle)
{
    return "wheels" + std::to_string(axle);
}

int axleCount(const std::vector<Wheel>& wheels)
{
    int n = 0;
    for (const Wheel& w : wheels)
        n = std::max(n, w.axle + 1);
    return n;
}

/// The patches whose forces count: the part and its turning wheels.
std::string partPatches(const std::vector<Wheel>& wheels)
{
    std::string out = "part";
    for (int a = 0; a < axleCount(wheels); ++a)
        out += " " + wheelPatch(a);
    return out;
}

void writeFile(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("Cannot write " + path.string());
    out << content;
    if (!out)
        throw std::runtime_error("Cannot write " + path.string());
}

std::uint64_t fnv1a(std::string_view data, std::uint64_t hash = 14695981039346656037ULL)
{
    for (const unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string asciiStl(const TriangleMesh& mesh, std::string_view name)
{
    Text t;
    t << "solid " << name << '\n';
    const auto& v = mesh.vertices();
    for (std::size_t i = 0; i < mesh.triangleCount(); ++i) {
        const auto& tri = mesh.triangles()[i];
        const Vec3 n = mesh.faceNormal(i);
        t << "facet normal " << n.x << ' ' << n.y << ' ' << n.z << "\n outer loop\n";
        for (const auto idx : tri)
            t << "  vertex " << v[idx].x << ' ' << v[idx].y << ' ' << v[idx].z << '\n';
        t << " endloop\nendfacet\n";
    }
    t << "endsolid " << name << '\n';
    return t.str();
}

TriangleMesh toCaseMesh(const TriangleMesh& mesh, const CaseFrame& frame)
{
    std::vector<Vec3> vertices;
    vertices.reserve(mesh.vertexCount());
    for (const Vec3& p : mesh.vertices())
        vertices.push_back(frame.toCase(p));
    // A rotation keeps the orientation of the triangles, so outward normals stay outward.
    return TriangleMesh(std::move(vertices), mesh.triangles());
}

std::string blockMeshDict(const BoundingBox& d, const int cells[3], bool ground)
{
    Text t;
    t << header("dictionary", "blockMeshDict") << "scale 1;\n\nvertices\n(\n";
    const double xs[2] = {d.min.x, d.max.x};
    const double ys[2] = {d.min.y, d.max.y};
    const double zs[2] = {d.min.z, d.max.z};
    // OpenFOAM hex vertex order: bottom face counter-clockwise, then top face.
    const int order[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int k = 0; k < 2; ++k)
        for (const auto& o : order)
            t << "    " << Vec3{xs[o[0]], ys[o[1]], zs[k]} << '\n';
    t << ");\n\nblocks\n(\n    hex (0 1 2 3 4 5 6 7) (" << cells[0] << ' ' << cells[1] << ' ' << cells[2]
      << ") simpleGrading (1 1 1)\n);\n\nedges ();\n\nboundary\n(\n"
      << "    inlet  { type patch; faces ((0 4 7 3)); }\n"
      << "    outlet { type patch; faces ((1 2 6 5)); }\n"
      << "    farfield\n    {\n        type patch;\n        faces\n        (\n"
      << "            (0 1 5 4)\n            (3 7 6 2)\n            (4 5 6 7)\n";
    if (!ground)
        t << "            (0 3 2 1)\n";
    t << "        );\n    }\n";
    if (ground)
        t << "    ground { type wall; faces ((0 3 2 1)); }\n";
    t << ");\n";
    return t.str();
}

std::string snappyHexMeshDict(const MeshPlan& plan, bool hasContext, int axles, const BoundingBox& nearBox,
                              const BoundingBox& wakeBox, const Vec3& locationInMesh)
{
    Text t;
    t << header("dictionary", "snappyHexMeshDict") << "castellatedMesh true;\nsnap            true;\naddLayers       "
      << (plan.layers > 0 ? "true" : "false") << ";\n\ngeometry\n{\n"
      << "    part.stl { type triSurfaceMesh; name part; }\n";
    for (int a = 0; a < axles; ++a)
        t << "    " << wheelPatch(a) << ".stl { type triSurfaceMesh; name " << wheelPatch(a) << "; }\n";
    if (hasContext)
        t << "    context.stl { type triSurfaceMesh; name context; }\n";
    t << "    near { type box; min " << nearBox.min << "; max " << nearBox.max << "; }\n"
      << "    wake { type box; min " << wakeBox.min << "; max " << wakeBox.max << "; }\n}\n\n"
      << "castellatedMeshControls\n{\n"
      << "    maxLocalCells 3000000;\n    maxGlobalCells 12000000;\n    minRefinementCells 10;\n"
      << "    maxLoadUnbalance 0.10;\n    nCellsBetweenLevels 3;\n\n    features\n    (\n"
      << "        { file \"part.eMesh\"; level " << plan.surfaceMax << "; }\n";
    for (int a = 0; a < axles; ++a)
        t << "        { file \"" << wheelPatch(a) << ".eMesh\"; level " << plan.surfaceMax << "; }\n";
    if (hasContext)
        t << "        { file \"context.eMesh\"; level " << plan.surfaceMin << "; }\n";
    t << "    );\n\n    refinementSurfaces\n    {\n"
      << "        part { level (" << plan.surfaceMin << ' ' << plan.surfaceMax
      << "); patchInfo { type wall; inGroups (walls); } }\n";
    for (int a = 0; a < axles; ++a)
        t << "        " << wheelPatch(a) << " { level (" << plan.surfaceMin << ' ' << plan.surfaceMax
          << "); patchInfo { type wall; inGroups (walls); } }\n";
    if (hasContext)
        t << "        context { level (" << plan.surfaceMin << ' ' << plan.surfaceMin
          << "); patchInfo { type wall; inGroups (walls); } }\n";
    t << "    }\n\n    resolveFeatureAngle 30;\n\n    refinementRegions\n    {\n"
      << "        near { mode inside; levels ((1e15 " << plan.nearLevel << ")); }\n"
      << "        wake { mode inside; levels ((1e15 " << plan.wakeLevel << ")); }\n    }\n\n"
      << "    locationInMesh " << locationInMesh << ";\n    allowFreeStandingZoneFaces true;\n}\n\n"
      << "snapControls\n{\n    nSmoothPatch 3;\n    tolerance 2.0;\n    nSolveIter 30;\n    nRelaxIter 5;\n"
      << "    nFeatureSnapIter 10;\n    implicitFeatureSnap false;\n    explicitFeatureSnap true;\n"
      << "    multiRegionFeatureSnap false;\n}\n\n"
      << "addLayersControls\n{\n    relativeSizes true;\n    layers\n    {\n"
      << "        " << [&] {
             if (axles == 0)
                 return std::string("part");
             std::string names = "\"(part";
             for (int a = 0; a < axles; ++a)
                 names += "|" + wheelPatch(a);
             return names + ")\"";
         }() << " { nSurfaceLayers " << std::max(plan.layers, 1) << "; }\n    }\n"
      << "    expansionRatio 1.2;\n    finalLayerThickness 0.5;\n    minThickness 0.1;\n    nGrow 0;\n"
      << "    featureAngle 60;\n    slipFeatureAngle 30;\n    nRelaxIter 3;\n    nSmoothSurfaceNormals 1;\n"
      << "    nSmoothNormals 3;\n    nSmoothThickness 10;\n    maxFaceThicknessRatio 0.5;\n"
      << "    maxThicknessToMedialRatio 0.3;\n    minMedialAxisAngle 90;\n    nBufferCellsNoExtrude 0;\n"
      << "    nLayerIter 50;\n}\n\n"
      << "meshQualityControls\n{\n    #includeEtc \"caseDicts/meshQualityDict\"\n    minFaceWeight 0.02;\n"
      << "    nSmoothScale 4;\n    errorReduction 0.75;\n}\n\nmergeTolerance 1e-6;\n";
    return t.str();
}

std::string surfaceFeatureExtractDict(bool hasContext, int axles)
{
    Text t;
    t << header("dictionary", "surfaceFeatureExtractDict");
    auto entry = [&](const std::string& file) {
        t << file
          << "\n{\n    extractionMethod extractFromSurface;\n    includedAngle 150;\n"
             "    subsetFeatures { nonManifoldEdges no; openEdges yes; }\n    writeObj no;\n}\n\n";
    };
    entry("part.stl");
    for (int a = 0; a < axles; ++a)
        entry(wheelPatch(a) + ".stl");
    if (hasContext)
        entry("context.stl");
    return t.str();
}

/// Iterations over which Cd and Cl are averaged to tell that the forces have settled, and
/// how far that mean may still move (CAD-155).
constexpr int kSettleWindow = 50;
constexpr double kSettleTolerance = 5e-4;

std::string controlDict(const Settings& s, const CaseInfo& info)
{
    const core::BoundingBox region = resultRegion(info);
    const int iterations = s.effectiveIterations();
    const BoundingBox& pb = info.partBounds;
    const Vec3 c = pb.center();
    const double L = info.characteristicLength;
    Text t;
    t << header("dictionary", "controlDict") << "application     simpleFoam;\nstartFrom       startTime;\n"
      << "startTime       0;\nstopAt          endTime;\nendTime         " << iterations
      << ";\ndeltaT          1;\nwriteControl    timeStep;\nwriteInterval   " << iterations
      << ";\npurgeWrite      0;\nwriteFormat     binary;\nwritePrecision  8;\nwriteCompression off;\n"
      << "timeFormat      general;\ntimePrecision   6;\nrunTimeModifiable true;\n\n"
      << "aerocadRhoInf   " << s.density << ";\naerocadUInf     " << s.speed << ";\n\nfunctions\n{\n"
      // Forces in N on the part only (rho = rhoInf: the solver works with kinematic pressure).
      << "    forces\n    {\n        type forces;\n        libs (forces);\n        writeControl timeStep;\n"
      << "        writeInterval 1;\n        log no;\n        patches (" << partPatches(info.wheels) << ");\n        rho rhoInf;\n"
      << "        rhoInf $aerocadRhoInf;\n        CofR " << c << ";\n    }\n"
      << "    forceCoeffs\n    {\n        type forceCoeffs;\n        libs (forces);\n"
      << "        writeControl timeStep;\n        writeInterval 1;\n        log yes;\n        patches (" << partPatches(info.wheels) << ");\n"
      << "        rho rhoInf;\n        rhoInf $aerocadRhoInf;\n        magUInf $aerocadUInf;\n"
      << "        lRef " << info.referenceLength << ";\n        Aref " << info.referenceArea << ";\n"
      << "        CofR " << c << ";\n        dragDir (1 0 0);\n        liftDir (0 0 1);\n"
      << "        pitchAxis (0 1 0);\n    }\n"
      // Stop once the forces have settled (CAD-155): the mean of Cd and Cl over the last
      // 50 iterations stays within 5e-4 of its own mean over the 50 before. Residuals of a
      // bluff body often never reach the targets while the forces only wobble around a
      // settled mean, so the run would otherwise go on to the iteration limit. On the F1
      // fine run this stops at iteration 319 of 800 with Cd and Cl within 0.6 % of the
      // full run's means.
      << "    coeffsMean\n    {\n        type valueAverage;\n        libs (fieldFunctionObjects);\n"
      << "        functionObject forceCoeffs;\n        fields (Cd Cl);\n        window " << kSettleWindow
      << ";\n        windowType exact;\n        log no;\n    }\n"
      << "    forcesSettled\n    {\n        type runTimeControl;\n        libs (utilityFunctionObjects);\n"
      << "        conditions\n        {\n            forces\n            {\n                type average;\n"
      << "                functionObject coeffsMean;\n                fields (CdMean ClMean);\n"
      << "                tolerance " << kSettleTolerance << ";\n                window " << kSettleWindow
      << ";\n                windowType exact;\n                nIterStartUp " << 2 * kSettleWindow
      << ";\n            }\n        }\n        satisfiedAction end;\n    }\n"
      << "    solverInfo\n    {\n        type solverInfo;\n        libs (utilityFunctionObjects);\n"
      << "        fields (U p k omega);\n        writeResidualFields no;\n    }\n"
      // Fields for the result view: pressure on the part, two velocity sections through
      // the part centre, streamlines from a grid of seeds upstream.
      << "    surfaces\n    {\n        type surfaces;\n        libs (sampling);\n"
      << "        writeControl writeTime;\n        surfaceFormat vtk;\n"
      << "        formatOptions { vtk { format ascii; legacy true; } }\n        fields (p U);\n"
      << "        interpolationScheme cellPoint;\n        surfaces\n        {\n"
      << "            part { type patch; patches (" << partPatches(info.wheels) << "); interpolate true; }\n"
      << "            sectionHorizontal { type cuttingPlane; point " << c
      << "; normal (0 0 1); interpolate true; }\n"
      << "            sectionVertical { type cuttingPlane; point " << c
      << "; normal (0 1 0); interpolate true; }\n";
    // Stacks of sections the result view can step through (CAD-148), cropped to the
    // region around the part so even a fine mesh writes only a few MB.
    for (const SectionStack& stack : sectionStacks()) {
        const int a = stack.axis - 'x';
        for (std::size_t i = 0; i < stack.fractions.size(); ++i) {
            Vec3 point = c;
            Vec3 normal{0, 0, 0};
            const double lo = a == 0 ? pb.min.x : a == 1 ? pb.min.y : pb.min.z;
            const double hi = a == 0 ? pb.max.x : a == 1 ? pb.max.y : pb.max.z;
            const double at = lo + stack.fractions[i] * (hi - lo);
            (a == 0 ? point.x : a == 1 ? point.y : point.z) = at;
            (a == 0 ? normal.x : a == 1 ? normal.y : normal.z) = 1.0;
            t << "            " << sectionName(stack.axis, static_cast<int>(i)) << " { type cuttingPlane; point "
              << point << "; normal " << normal << "; interpolate true; bounds " << region.min << ' '
              << region.max << "; }\n";
        }
    }
    t << "        }\n    }\n";
    // Streamlines (CAD-135): a dense 16 x 10 rake just ahead of the part, a little wider
    // and taller than it, so the lines wrap the body closely; and an 8 x 5 rake just behind
    // it traced both ways, which shows the recirculation of the wake that lines from
    // upstream never enter.
    const Vec3 size = pb.size();
    auto streamLineObject = [&](const char* name, const char* direction, double x, int ny, int nz, double y0,
                                double y1, double z0, double z1) {
        t << "    " << name << "\n    {\n        type streamLine;\n        libs (fieldFunctionObjects);\n"
          << "        writeControl writeTime;\n        setFormat vtk;\n        formatOptions { vtk { format ascii; legacy true; } }\n"
          << "        direction " << direction << ";\n        fields (p U);\n        lifeTime 10000;\n        nSubCycle 5;\n"
          << "        cloud " << name << "Tracks;\n        seedSampleSet\n        {\n            type cloud;\n"
          << "            axis xyz;\n            points\n            (\n";
        for (int iz = 0; iz < nz; ++iz)
            for (int iy = 0; iy < ny; ++iy)
                t << "                "
                  << Vec3{x, pb.min.y + size.y * (y0 + (y1 - y0) * (iy + 0.5) / ny),
                          pb.min.z + size.z * (z0 + (z1 - z0) * (iz + 0.5) / nz)}
                  << '\n';
        t << "            );\n        }\n    }\n";
    };
    streamLineObject("streamLines", "forward", pb.min.x - 0.05 * L, 16, 10, -0.15, 1.15, -0.1, 1.25);
    streamLineObject("wakeLines", "bidirectional", pb.max.x + 0.1 * L, 8, 5, 0.05, 0.95, 0.05, 0.95);
    t << "}\n";
    return t.str();
}

std::string fvSchemes()
{
    return header("dictionary", "fvSchemes") +
           "ddtSchemes { default steadyState; }\n\n"
           "gradSchemes\n{\n    default Gauss linear;\n    grad(U) cellLimited Gauss linear 1;\n}\n\n"
           "divSchemes\n{\n    default none;\n    div(phi,U) bounded Gauss linearUpwindV grad(U);\n"
           "    turbulence bounded Gauss upwind;\n    div(phi,k) $turbulence;\n    div(phi,omega) $turbulence;\n"
           "    div((nuEff*dev2(T(grad(U))))) Gauss linear;\n}\n\n"
           "laplacianSchemes { default Gauss linear limited corrected 0.5; }\n\n"
           "interpolationSchemes { default linear; }\n\n"
           "snGradSchemes { default limited corrected 0.5; }\n\n"
           "wallDist { method meshWave; }\n";
}

std::string fvSolution()
{
    return header("dictionary", "fvSolution") +
           "solvers\n{\n"
           "    p { solver GAMG; smoother GaussSeidel; tolerance 1e-7; relTol 0.01; }\n"
           "    Phi { $p; }\n"
           "    \"(U|k|omega)\" { solver smoothSolver; smoother GaussSeidel; tolerance 1e-8; relTol 0.1; nSweeps 1; }\n"
           "}\n\n"
           "SIMPLE\n{\n    nNonOrthogonalCorrectors 0;\n    consistent yes;\n"
           "    residualControl { p 1e-4; U 1e-5; \"(k|omega)\" 1e-5; }\n}\n\n"
           "potentialFlow { nNonOrthogonalCorrectors 10; }\n\n"
           "relaxationFactors { equations { U 0.9; \"(k|omega)\" 0.7; } }\n\n"
           "cache { grad(U); }\n";
}

std::string decomposeParDict(int processes)
{
    Text t;
    t << header("dictionary", "decomposeParDict") << "numberOfSubdomains " << processes
      << ";\nmethod          scotch;\n";
    return t.str();
}

/// Initial and boundary conditions. Patches: inlet, outlet, farfield (slip), ground
/// (optional moving floor), part and context (walls, group "walls").
void writeFields(const fs::path& zero, const Settings& s, const std::vector<Wheel>& wheels)
{
    const double U = s.speed;
    const double k = 1.5 * std::pow(U * s.turbulenceIntensity, 2);
    const double omega = k / (s.kinematicViscosity * s.viscosityRatio);
    Text ic;
    ic << "flowVelocity    (" << U << " 0 0);\nturbulentKE     " << k << ";\nturbulentOmega  " << omega << ";\n";
    writeFile(zero / "include" / "initialConditions", ic.str());

    auto field = [&](const char* cls, const char* name, const char* dims, const char* internal,
                     const std::string& patches) {
        writeFile(zero / name, header(cls, name) + "#include \"include/initialConditions\"\n\ndimensions      " + dims +
                                   ";\ninternalField   " + internal + ";\n\nboundaryField\n{\n    #includeEtc \"caseDicts/setConstraintTypes\"\n" + patches + "}\n");
    };
    const bool ground = s.ground;
    std::string u = "    inlet { type fixedValue; value $internalField; }\n"
                    "    outlet { type inletOutlet; inletValue uniform (0 0 0); value $internalField; }\n"
                    "    farfield { type slip; }\n"
                    "    walls { type noSlip; }\n";
    if (ground && s.groundMoving)
        u += "    ground { type fixedValue; value $internalField; }\n"; // Floor moving with the air.
    else if (ground)
        u += "    ground { type noSlip; }\n"; // Fixed tunnel floor.
    // CAD-154: each axle turns so that its tyres' bottom moves with the floor (+X at U):
    // about -Y, at U / r.
    for (int a = 0; a < axleCount(wheels); ++a) {
        const auto w = std::find_if(wheels.begin(), wheels.end(), [a](const Wheel& x) { return x.axle == a; });
        Text t;
        t << "    " << wheelPatch(a) << " { type rotatingWallVelocity; origin " << w->centre
          << "; axis (0 -1 0); omega " << U / w->radius << "; value uniform (0 0 0); }\n";
        u += t.str();
    }
    field("volVectorField", "U", "[0 1 -1 0 0 0 0]", "uniform $flowVelocity", u);

    std::string p = "    inlet { type zeroGradient; }\n"
                    "    outlet { type fixedValue; value $internalField; }\n"
                    "    farfield { type slip; }\n"
                    "    walls { type zeroGradient; }\n";
    if (ground)
        p += "    ground { type zeroGradient; }\n";
    field("volScalarField", "p", "[0 2 -2 0 0 0 0]", "uniform 0", p);

    auto turbulence = [&](const char* name, const char* dims, const char* value, const char* wallFunction) {
        std::string patches = "    inlet { type fixedValue; value $internalField; }\n"
                              "    outlet { type inletOutlet; inletValue $internalField; value $internalField; }\n"
                              "    farfield { type slip; }\n";
        const std::string wall = std::string("{ type ") + wallFunction + "; value $internalField; }\n";
        patches += "    walls " + wall;
        if (ground)
            patches += "    ground " + wall;
        field("volScalarField", name, dims, value, patches);
    };
    turbulence("k", "[0 2 -2 0 0 0 0]", "uniform $turbulentKE", "kqRWallFunction");
    turbulence("omega", "[0 0 -1 0 0 0 0]", "uniform $turbulentOmega", "omegaWallFunction");

    std::string nut = "    inlet { type calculated; value uniform 0; }\n"
                      "    outlet { type calculated; value uniform 0; }\n"
                      "    farfield { type calculated; value uniform 0; }\n"
                      "    walls { type nutkWallFunction; value uniform 0; }\n";
    if (ground)
        nut += "    ground { type nutkWallFunction; value uniform 0; }\n";
    field("volScalarField", "nut", "[0 2 -1 0 0 0 0]", "uniform 0", nut);
}

std::string allrun(int processes)
{
    Text t;
    t << "#!/bin/sh\n"
         "# Generated by AeroCAD. Meshes and solves this case; run inside an OpenFOAM "
      << kOpenFoamVersion
      << "\n# environment. Progress: aerocad.stage (current step), aerocad.status (result).\n"
         "cd \"${0%/*}\" || exit 1\n"
         ". \"${WM_PROJECT_DIR:?}\"/bin/tools/RunFunctions\n"
         "rm -f aerocad.status\n"
         "# The engine that actually runs the solver, for A/B comparisons. RunFunctions skips an\n"
         "# application whose log exists, so reused results keep the engine that produced them.\n"
         "engine() { [ -f log.simpleFoam ] || printf '%s\\n' \"${WM_PROJECT_VERSION:-}\" > aerocad.engine; }\n"
         "step() { echo \"$1\" > aerocad.stage; }\n"
         "fail() { echo \"failed $1\" > aerocad.status; exit 1; }\n"
         "# checkMesh marks failed checks with \"***\". These defects make the solver diverge or\n"
         "# crash, so the run stops before it; other reports (non-orthogonality, skewness, aspect\n"
         "# ratio, face tets, ...) are warnings.\n"
         "meshok() { ! grep -qE '[*]{3}("
      << kFatalMeshChecks
      << ")' log.checkMesh 2>/dev/null; }\n\n"
         "step features;   runApplication surfaceFeatureExtract || fail features\n"
         "step background; runApplication blockMesh || fail background\n";
    if (processes > 1) {
        t << "step decompose;  runApplication decomposePar -force || fail decompose\n"
             "step mesh;       runParallel snappyHexMesh -overwrite || fail mesh\n"
             "step check;      runParallel checkMesh -constant\n"
             "meshok || fail check\n"
             "restore0Dir -processor\n"
             "step initialise; runParallel potentialFoam -writephi || fail initialise\n"
             "engine\n"
             "step solve;      runParallel simpleFoam || fail solve\n"
             "step reconstruct\n"
             "runApplication reconstructParMesh -constant || fail reconstruct\n"
             "runApplication reconstructPar -latestTime || fail reconstruct\n";
    } else {
        t << "step mesh;       runApplication snappyHexMesh -overwrite || fail mesh\n"
             "step check;      runApplication checkMesh -constant\n"
             "meshok || fail check\n"
             "restore0Dir\n"
             "step initialise; runApplication potentialFoam -writephi || fail initialise\n"
             "engine\n"
             "step solve;      runApplication simpleFoam || fail solve\n";
    }
    t << "step done\necho finished > aerocad.status\n";
    return t.str();
}

} // namespace

core::BoundingBox resultRegion(const CaseInfo& info, double wake)
{
    core::BoundingBox region;
    const core::BoundingBox& pb = info.partBounds;
    if (pb.isEmpty())
        return region;
    const double m = std::max(info.characteristicLength, 1e-9);
    region.extend({pb.min.x - 0.4 * m, pb.min.y - 0.4 * m,
                   info.settings.ground ? info.domain.min.z : pb.min.z - 0.4 * m});
    region.extend({pb.max.x + wake * m, pb.max.y + 0.4 * m, pb.max.z + 0.4 * m});
    return region;
}

const std::vector<SectionStack>& sectionStacks()
{
    static const std::vector<SectionStack> stacks{
        {'x', {-0.1, 0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5}, 12},
        {'y', {0.1, 0.25, 0.5, 0.75, 0.9}, 2},
        {'z', {0.05, 0.15, 0.3, 0.5, 0.7, 0.9}, 3},
    };
    return stacks;
}

std::string sectionName(char axis, int index)
{
    return std::string("section") + static_cast<char>(axis - 'a' + 'A') + "_" + std::to_string(index);
}

CaseInfo planCase(const CaseGeometry& geometry, const Settings& s)
{
    if (geometry.part.isEmpty())
        throw std::runtime_error("The part has no surface triangles to simulate");

    CaseInfo info;
    info.settings = s;
    const BoundingBox modelBounds = geometry.part.bounds();
    info.frame = CaseFrame::forFlow(s.flowDirection, modelBounds.center());

    const TriangleMesh part = toCaseMesh(geometry.part, info.frame);
    info.partBounds = part.bounds();
    BoundingBox all = info.partBounds;
    all.merge(toCaseMesh(geometry.context, info.frame).bounds());
    const Vec3 size = info.partBounds.size();
    const double L = std::max({size.x, size.y, size.z});
    if (!(L > 0.0))
        throw std::runtime_error("The part has zero size");
    info.characteristicLength = L;
    info.referenceArea = s.referenceArea.value_or(frontalArea(part));
    if (!(info.referenceArea > 0.0))
        info.referenceArea = size.y * size.z > 0 ? size.y * size.z : L * L;
    info.referenceLength = s.referenceLength.value_or(size.x > 0 ? size.x : L);

    // Domain around all geometry: 3 L upstream, 8 L downstream, 2.5 L to the sides and
    // top, and either 2.5 L below or a floor just under the lowest point. Background
    // cells are cubes; the far bounds are snapped to whole cells.
    const double h = L / meshPlan(s.mesh).cellsPerLength;
    BoundingBox d;
    double floor = all.min.z - 0.02 * L;
    if (s.rotatingWheels) {
        // CAD-154: the tyres stand on the moving floor. It is put a little above their
        // bottom, so each tyre meets it in a contact patch instead of a line of degenerate
        // cells; their surfaces then move with it there.
        if (!s.ground || !s.groundMoving)
            throw std::runtime_error("Rotating wheels need a moving floor: turn on the floor and let it move");
        if (s.groundHeight)
            throw std::runtime_error(
                "With rotating wheels the floor is put where the tyres touch it: leave the floor height empty");
        info.wheels = detectWheels(part, L);
        if (info.wheels.empty())
            throw std::runtime_error("No wheels found: rotating wheels need tyres standing on the bottom of the part");
        double r = info.wheels.front().radius;
        for (const Wheel& w : info.wheels)
            r = std::min(r, w.radius);
        floor = info.partBounds.min.z + 0.015 * r;
        if (all.min.z < info.partBounds.min.z - 1e-9)
            throw std::runtime_error("With rotating wheels no other part may reach below the tyres");
    }
    if (s.ground && s.groundHeight) {
        floor = info.frame.toCase({0.0, 0.0, *s.groundHeight}).z; // Rotation is about Z only.
        if (floor > info.partBounds.min.z + 1e-9) {
            const double bottom = info.frame.toModel({0.0, 0.0, info.partBounds.min.z}).z;
            char text[200];
            std::snprintf(text, sizeof text,
                          "The floor at Z = %.1f mm is above the lowest point of the part (Z = %.1f mm): put it at "
                          "or below the part's bottom",
                          *s.groundHeight, bottom);
            throw std::runtime_error(text);
        }
    }
    d.min = {all.min.x - 3.0 * L, all.min.y - 2.5 * L, s.ground ? floor : all.min.z - 2.5 * L};
    d.max = {all.max.x + 8.0 * L, all.max.y + 2.5 * L, all.max.z + 2.5 * L};
    const Vec3 ds = d.size();
    auto cellsAlong = [h](double length) { return std::max(1, static_cast<int>(std::ceil(length / h - 1e-9))); };
    d.max = {d.min.x + cellsAlong(ds.x) * h, d.min.y + cellsAlong(ds.y) * h, d.min.z + cellsAlong(ds.z) * h};
    info.domain = d;
    info.baseCellSize = h;
    return info;
}

std::uint64_t meshFingerprint(const TriangleMesh& mesh)
{
    std::uint64_t hash = fnv1a({});
    const auto mix = [&hash](const auto& value) {
        hash = fnv1a(std::string_view(reinterpret_cast<const char*>(&value), sizeof(value)), hash);
    };
    for (const Vec3& v : mesh.vertices()) {
        mix(v.x);
        mix(v.y);
        mix(v.z);
    }
    for (const auto& t : mesh.triangles())
        mix(t);
    return hash;
}

namespace {

/// Fingerprint of a surface that does not depend on the order of its triangles or of the
/// parts it was gathered from (context: [2, 3] and [3, 2] are the same surroundings, CAD-84).
/// Each triangle is hashed from its vertex positions, starting at its smallest vertex so the
/// orientation is kept; the triangle hashes are sorted before they are combined.
std::uint64_t unorderedSurfaceFingerprint(const TriangleMesh& mesh)
{
    const auto& v = mesh.vertices();
    std::vector<std::uint64_t> hashes;
    hashes.reserve(mesh.triangles().size());
    for (const auto& t : mesh.triangles()) {
        const auto less = [&v](std::uint32_t a, std::uint32_t b) {
            return std::tie(v[a].x, v[a].y, v[a].z) < std::tie(v[b].x, v[b].y, v[b].z);
        };
        std::size_t first = 0;
        for (std::size_t k = 1; k < 3; ++k)
            if (less(t[k], t[first]))
                first = k;
        std::uint64_t h = fnv1a({});
        for (std::size_t k = 0; k < 3; ++k) {
            const Vec3& p = v[t[(first + k) % 3]];
            for (const double c : {p.x, p.y, p.z})
                h = fnv1a(std::string_view(reinterpret_cast<const char*>(&c), sizeof(c)), h);
        }
        hashes.push_back(h);
    }
    std::sort(hashes.begin(), hashes.end());
    return fnv1a(std::string_view(reinterpret_cast<const char*>(hashes.data()), hashes.size() * sizeof(std::uint64_t)));
}

} // namespace

std::uint64_t caseGeometryHash(const CaseGeometry& geometry, const Settings& s)
{
    const CaseInfo info = planCase(geometry, s);
    const TriangleMesh context = toCaseMesh(geometry.context, info.frame);
    std::uint64_t hash = fnv1a(asciiStl(toCaseMesh(geometry.part, info.frame), "part"));
    if (!context.isEmpty())
        hash = fnv1a(asciiStl(context, "context"), hash);
    return hash;
}

CaseInfo writeCase(const fs::path& dir, const CaseGeometry& geometry, const Settings& s)
{
    CaseInfo info = planCase(geometry, s);
    const TriangleMesh part = toCaseMesh(geometry.part, info.frame);
    const TriangleMesh context = toCaseMesh(geometry.context, info.frame);
    const bool hasContext = !context.isEmpty();
    const MeshPlan plan = meshPlan(s.mesh);
    const double L = info.characteristicLength;
    const double h = info.baseCellSize;
    const BoundingBox& d = info.domain;
    const Vec3 ds = d.size();
    const int cells[3] = {static_cast<int>(std::lround(ds.x / h)), static_cast<int>(std::lround(ds.y / h)),
                          static_cast<int>(std::lround(ds.z / h))};

    // Close around the part and its near wake, where the surface pressure is decided.
    BoundingBox nearBox;
    nearBox.min = {info.partBounds.min.x - 0.15 * L, info.partBounds.min.y - 0.1 * L,
                   s.ground ? d.min.z : info.partBounds.min.z - 0.1 * L};
    nearBox.max = {info.partBounds.max.x + 0.75 * L, info.partBounds.max.y + 0.1 * L,
                   info.partBounds.max.z + 0.1 * L};
    BoundingBox wake;
    wake.min = {info.partBounds.min.x - 0.5 * L, info.partBounds.min.y - 0.5 * L,
                s.ground ? d.min.z : info.partBounds.min.z - 0.5 * L};
    wake.max = {info.partBounds.max.x + 3.0 * L, info.partBounds.max.y + 0.5 * L, info.partBounds.max.z + 0.75 * L};

    // A point in the fluid near the inlet, off any cell face or edge.
    const Vec3 location{d.min.x + 1.137 * h, d.min.y + 1.173 * h, d.max.z - 1.219 * h};

    fs::create_directories(dir);
    const std::string partStl = asciiStl(part, "part");
    info.geometryHash = fnv1a(partStl);
    if (info.wheels.empty()) {
        writeFile(dir / "constant" / "triSurface" / "part.stl", partStl);
    } else {
        // The turning wheels are surfaces of their own (one per axle), so they can move.
        const WheelSplit split = splitWheels(part, info.wheels);
        writeFile(dir / "constant" / "triSurface" / "part.stl", asciiStl(split.body, "part"));
        for (std::size_t a = 0; a < split.axles.size(); ++a)
            writeFile(dir / "constant" / "triSurface" / (wheelPatch(static_cast<int>(a)) + ".stl"),
                      asciiStl(split.axles[a], wheelPatch(static_cast<int>(a))));
    }
    info.contextHash = 0;
    if (hasContext) {
        const std::string contextStl = asciiStl(context, "context");
        writeFile(dir / "constant" / "triSurface" / "context.stl", contextStl);
        info.geometryHash = fnv1a(contextStl, info.geometryHash);
        // In model coordinates (the case frame is centred on the target part, so the same
        // surroundings next to a variant of another size are written at other coordinates),
        // independent of the order of the context parts.
        info.contextHash = unorderedSurfaceFingerprint(geometry.context);
    }

    writeFile(dir / "constant" / "transportProperties",
              header("dictionary", "transportProperties") + "transportModel Newtonian;\nnu " +
                  (Text() << s.kinematicViscosity).str() + ";\n");
    writeFile(dir / "constant" / "turbulenceProperties",
              header("dictionary", "turbulenceProperties") +
                  "simulationType RAS;\n\nRAS\n{\n    RASModel kOmegaSST;\n    turbulence on;\n    printCoeffs on;\n}\n");
    writeFile(dir / "system" / "blockMeshDict", blockMeshDict(d, cells, s.ground));
    writeFile(dir / "system" / "snappyHexMeshDict", snappyHexMeshDict(plan, hasContext, axleCount(info.wheels), nearBox, wake, location));
    writeFile(dir / "system" / "surfaceFeatureExtractDict", surfaceFeatureExtractDict(hasContext, axleCount(info.wheels)));
    writeFile(dir / "system" / "controlDict", controlDict(s, info));
    writeFile(dir / "system" / "fvSchemes", fvSchemes());
    writeFile(dir / "system" / "fvSolution", fvSolution());
    const int processes = s.effectiveProcesses();
    writeFile(dir / "system" / "decomposeParDict", decomposeParDict(processes));
    fs::remove_all(dir / "0");
    fs::remove_all(dir / "0.orig");
    writeFields(dir / "0.orig", s, info.wheels);

    const fs::path script = dir / "Allrun";
    writeFile(script, allrun(processes));
    fs::permissions(script,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                        fs::perms::others_exec,
                    fs::perm_options::replace);
    // ParaView opens a case through an (empty) .foam file.
    writeFile(dir / "case.foam", "");
    writeFile(dir / "case.json", info.toJson().dump(2) + "\n");
    return info;
}

} // namespace aerocad::cfd
