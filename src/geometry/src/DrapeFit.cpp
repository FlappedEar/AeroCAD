// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/DrapeFit.hpp"

#include "aerocad/geometry/OcctShape.hpp"
#include "aerocad/geometry/SurfaceFit.hpp"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFill.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_Curve.hxx>
#include <Precision.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeFix_Shape.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace aerocad::geometry {
namespace {

/// Samples on a regular grid over the area; `known` marks those on the skin.
struct Raster {
    int nu = 0;
    int nv = 0;
    double du = 1.0;
    double dv = 1.0;
    std::vector<double> z;
    std::vector<char> known;

    [[nodiscard]] std::size_t at(int i, int j) const { return static_cast<std::size_t>(j) * nu + i; }
};

/// First hit from outside at every sample (height along the normal).
Raster sampleSkin(const core::MeshProjector& scan, const DrapeArea& a, double step)
{
    Raster r;
    r.nu = std::clamp(static_cast<int>(std::ceil((a.u1 - a.u0) / step)) + 1, 8, 2048);
    r.nv = std::clamp(static_cast<int>(std::ceil((a.v1 - a.v0) / step)) + 1, 8, 2048);
    r.du = (a.u1 - a.u0) / (r.nu - 1);
    r.dv = (a.v1 - a.v0) / (r.nv - 1);
    r.z.assign(static_cast<std::size_t>(r.nu) * r.nv, 0.0);
    r.known.assign(r.z.size(), 0);
    for (int j = 0; j < r.nv; ++j) {
        const double v = a.v0 + j * r.dv;
        for (int i = 0; i < r.nu; ++i) {
            const double u = a.u0 + i * r.du;
            const auto hit = scan.drop(a.origin + a.x * u + a.y * v);
            if (hit) {
                r.z[r.at(i, j)] = (*hit - a.origin).dot(a.normal);
                r.known[r.at(i, j)] = 1;
            }
        }
    }
    return r;
}

/// The scan's own noise on the followed skin, one standard deviation in mm: a robust
/// (median) estimate from fourth differences of samples `lag` apart inside the outline,
/// along both directions. For independent noise of deviation s the difference
/// a - 4b + 6c - 4d + e has deviation s * sqrt(70), while a curved shape adds nothing up to
/// its cubic terms; creases and steps are too rare to move the median. Samples closer
/// together than the scan's triangles share noise, so a lag shorter than them makes the
/// estimate smaller, never larger.
double estimateNoise(const Raster& r, const DrapeArea& a, int lag)
{
    std::vector<float> d;
    const auto inside = [&](int i, int j) {
        const double u = a.u0 + i * r.du, v = a.v0 + j * r.dv;
        return u >= a.insideU0 && u <= a.insideU1 && v >= a.insideV0 && v <= a.insideV1;
    };
    const auto fourth = [&](int i, int j, int di, int dj) {
        constexpr std::array<double, 5> c{1.0, -4.0, 6.0, -4.0, 1.0};
        double sum = 0.0;
        for (int k = -2; k <= 2; ++k) {
            const std::size_t s = r.at(i + k * di, j + k * dj);
            if (!r.known[s])
                return false;
            sum += c[static_cast<std::size_t>(k + 2)] * r.z[s];
        }
        d.push_back(static_cast<float>(std::abs(sum)));
        return true;
    };
    // At most about 40,000 differences: every step-th sample in both directions.
    const std::size_t samples = static_cast<std::size_t>(r.nu) * static_cast<std::size_t>(r.nv);
    const int step = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(samples) / 20000.0)));
    // Each direction needs room only along itself: a long, thin outline (a fin's root)
    // still gets the differences along its length.
    const int reach = 2 * lag;
    for (int j = 0; j < r.nv; j += step)
        for (int i = 0; i < r.nu; i += step) {
            if (!inside(i, j) || !r.known[r.at(i, j)])
                continue;
            if (i >= reach && i + reach < r.nu)
                fourth(i, j, lag, 0);
            if (j >= reach && j + reach < r.nv)
                fourth(i, j, 0, lag);
        }
    if (d.size() < 32)
        return 0.0;
    auto mid = d.begin() + static_cast<std::ptrdiff_t>(d.size() / 2);
    std::nth_element(d.begin(), mid, d.end());
    // Median of |N(0, s^2)| is 0.6745 s.
    return static_cast<double>(*mid) / 0.6745 / std::sqrt(70.0);
}

/// Whether what is left after a fit is spread like scan noise rather than shape: its
/// correlation between samples 1 to 4 times `lag` apart, along each direction, stays
/// within +-0.3. Scan noise is independent from one scanned point to the next; missed
/// shape (ridges, ripples, a corrugation finer than the control points) is not: a wave
/// cannot be near zero at both x and 2x (cos 2x = 2 cos^2 x - 1), and waves across the
/// direction add correlation at every distance. NaN marks samples with no residual. A
/// direction with too few pairs (a thin outline) is not judged.
bool residualLooksLikeNoise(const std::vector<double>& residual, int nu, int nv, int lag)
{
    for (int shift : {lag, 2 * lag, 3 * lag, 4 * lag})
        for (int axis = 0; axis < 2; ++axis) {
            double ab = 0.0, aa = 0.0, bb = 0.0;
            std::size_t pairs = 0;
            const int di = axis == 0 ? shift : 0, dj = axis == 0 ? 0 : shift;
            for (int j = 0; j + dj < nv; ++j)
                for (int i = 0; i + di < nu; ++i) {
                    const double a = residual[static_cast<std::size_t>(j) * nu + i];
                    const double b = residual[static_cast<std::size_t>(j + dj) * nu + i + di];
                    if (std::isnan(a) || std::isnan(b))
                        continue;
                    ab += a * b;
                    aa += a * a;
                    bb += b * b;
                    ++pairs;
                }
            if (pairs < 200 || !(aa > 0.0) || !(bb > 0.0))
                continue;
            if (std::abs(ab / std::sqrt(aa * bb)) > 0.3)
                return false;
        }
    return true;
}

/// Keeps the largest sheet under the outline: samples joined by steps no steeper than
/// `maxSlope`. Whatever is seen through a hole or past an edge is forgotten.
void keepMainSheet(Raster& r, const DrapeArea& a, double maxSlope)
{
    std::vector<int> label(r.z.size(), -1);
    const double jumpU = maxSlope * r.du, jumpV = maxSlope * r.dv;
    int best = -1;
    std::size_t bestCount = 0;
    int labels = 0;
    std::vector<std::size_t> stack;
    for (std::size_t s = 0; s < r.z.size(); ++s) {
        if (!r.known[s] || label[s] >= 0)
            continue;
        std::size_t count = 0;
        label[s] = labels;
        stack.push_back(s);
        while (!stack.empty()) {
            const std::size_t c = stack.back();
            stack.pop_back();
            const int i = static_cast<int>(c % r.nu), j = static_cast<int>(c / r.nu);
            const double u = a.u0 + i * r.du, v = a.v0 + j * r.dv;
            if (u >= a.insideU0 && u <= a.insideU1 && v >= a.insideV0 && v <= a.insideV1)
                ++count;
            auto visit = [&](int ni, int nj, double jump) {
                if (ni < 0 || nj < 0 || ni >= r.nu || nj >= r.nv)
                    return;
                const std::size_t n = r.at(ni, nj);
                if (r.known[n] && label[n] < 0 && std::abs(r.z[n] - r.z[c]) <= jump) {
                    label[n] = labels;
                    stack.push_back(n);
                }
            };
            visit(i - 1, j, jumpU);
            visit(i + 1, j, jumpU);
            visit(i, j - 1, jumpV);
            visit(i, j + 1, jumpV);
        }
        if (count > bestCount) {
            bestCount = count;
            best = labels;
        }
        ++labels;
    }
    for (std::size_t s = 0; s < r.z.size(); ++s)
        if (label[s] != best)
            r.known[s] = 0;
}

/// Fills unknown samples smoothly from the known ones: a coarse-to-fine pyramid, then
/// relaxation on the unknown samples. Returns the filled heights.
std::vector<double> fillGaps(const Raster& r)
{
    struct Level {
        int nu, nv;
        std::vector<double> z, w;
    };
    std::vector<Level> levels;
    Level base{r.nu, r.nv, r.z, std::vector<double>(r.z.size(), 0.0)};
    for (std::size_t s = 0; s < r.z.size(); ++s)
        base.w[s] = r.known[s] ? 1.0 : 0.0;
    levels.push_back(std::move(base));
    while (levels.back().nu > 2 || levels.back().nv > 2) {
        const Level& f = levels.back();
        Level c{std::max(1, (f.nu + 1) / 2), std::max(1, (f.nv + 1) / 2), {}, {}};
        c.z.assign(static_cast<std::size_t>(c.nu) * c.nv, 0.0);
        c.w.assign(c.z.size(), 0.0);
        for (int j = 0; j < f.nv; ++j)
            for (int i = 0; i < f.nu; ++i) {
                const std::size_t s = static_cast<std::size_t>(j) * f.nu + i;
                const std::size_t t = static_cast<std::size_t>(j / 2) * c.nu + i / 2;
                c.z[t] += f.z[s] * f.w[s];
                c.w[t] += f.w[s];
            }
        for (std::size_t s = 0; s < c.z.size(); ++s) {
            if (c.w[s] > 0.0)
                c.z[s] /= c.w[s];
            c.w[s] = std::min(1.0, c.w[s]);
        }
        levels.push_back(std::move(c));
    }
    // Coarsest level: unknown cells take the average of the known ones.
    {
        Level& top = levels.back();
        double sum = 0.0, n = 0.0;
        for (std::size_t s = 0; s < top.z.size(); ++s) {
            if (top.w[s] > 0.0) {
                sum += top.z[s];
                n += 1.0;
            }
        }
        for (std::size_t s = 0; s < top.z.size(); ++s)
            if (top.w[s] <= 0.0)
                top.z[s] = n > 0.0 ? sum / n : 0.0;
    }
    for (std::size_t l = levels.size() - 1; l-- > 0;) {
        const Level& c = levels[l + 1];
        Level& f = levels[l];
        auto coarse = [&](int i, int j) {
            return c.z[static_cast<std::size_t>(std::clamp(j, 0, c.nv - 1)) * c.nu + std::clamp(i, 0, c.nu - 1)];
        };
        std::vector<std::size_t> open;
        for (int j = 0; j < f.nv; ++j)
            for (int i = 0; i < f.nu; ++i) {
                const std::size_t s = static_cast<std::size_t>(j) * f.nu + i;
                if (f.w[s] > 0.0)
                    continue;
                // Bilinear from the coarse level.
                const double x = 0.5 * i - 0.25, y = 0.5 * j - 0.25;
                const int i0 = static_cast<int>(std::floor(x)), j0 = static_cast<int>(std::floor(y));
                const double tx = x - i0, ty = y - j0;
                f.z[s] = (1 - ty) * ((1 - tx) * coarse(i0, j0) + tx * coarse(i0 + 1, j0)) +
                         ty * ((1 - tx) * coarse(i0, j0 + 1) + tx * coarse(i0 + 1, j0 + 1));
                open.push_back(s);
            }
        // A few sweeps remove the blockiness of the coarse values.
        for (int sweep = 0; sweep < 8; ++sweep) {
            for (const std::size_t s : open) {
                const int i = static_cast<int>(s % f.nu), j = static_cast<int>(s / f.nu);
                double sum = 0.0;
                int n = 0;
                if (i > 0) { sum += f.z[s - 1]; ++n; }
                if (i + 1 < f.nu) { sum += f.z[s + 1]; ++n; }
                if (j > 0) { sum += f.z[s - f.nu]; ++n; }
                if (j + 1 < f.nv) { sum += f.z[s + f.nu]; ++n; }
                if (n > 0)
                    f.z[s] = sum / n;
            }
        }
    }
    return levels.front().z;
}

/// Uniform clamped cubic B-spline over [0, 1]: `poles` control points.
struct Basis {
    int poles;
    int spans;
    /// Index of the first of the four non-zero basis functions at t, and their values.
    void eval(double t, int& first, std::array<double, 4>& w) const
    {
        const double x = std::clamp(t, 0.0, 1.0) * spans;
        const int span = std::min(static_cast<int>(x), spans - 1);
        first = span;
        // Cox-de Boor on the clamped knot vector.
        auto knot = [&](int k) { return static_cast<double>(std::clamp(k - 3, 0, spans)); };
        const int s = span + 3; // knot span index
        std::array<double, 4> n{1.0, 0.0, 0.0, 0.0};
        std::array<double, 4> left{}, right{};
        for (int d = 1; d <= 3; ++d) {
            left[static_cast<std::size_t>(d)] = x - knot(s + 1 - d);
            right[static_cast<std::size_t>(d)] = knot(s + d) - x;
            double saved = 0.0;
            for (int r = 0; r < d; ++r) {
                const auto ur = static_cast<std::size_t>(r);
                const double denom = right[ur + 1] + left[static_cast<std::size_t>(d - r)];
                const double temp = denom > 0.0 ? n[ur] / denom : 0.0;
                n[ur] = saved + right[ur + 1] * temp;
                saved = left[static_cast<std::size_t>(d - r)] * temp;
            }
            n[static_cast<std::size_t>(d)] = saved;
        }
        w = n;
    }
};

/// Least squares of one axis of the tensor product, with a small second-difference penalty:
/// from `m` samples per line to `p` values per line, for `lines` lines. Samples and results
/// are read and written with the given strides.
class AxisFit {
public:
    AxisFit(int samples, int poles) : m_m(samples), m_p(poles), m_first(static_cast<std::size_t>(samples)),
                                      m_w(static_cast<std::size_t>(samples)),
                                      m_chol(static_cast<std::size_t>(poles))
    {
        const Basis b{poles, poles - 3};
        std::vector<std::array<double, 4>> ata(static_cast<std::size_t>(poles), std::array<double, 4>{0, 0, 0, 0});
        for (int k = 0; k < samples; ++k) {
            const auto uk = static_cast<std::size_t>(k);
            b.eval(static_cast<double>(k) / (samples - 1), m_first[uk], m_w[uk]);
            for (int a = 0; a < 4; ++a)
                for (int c = a; c < 4; ++c)
                    ata[static_cast<std::size_t>(m_first[uk] + a)][static_cast<std::size_t>(c - a)] +=
                        m_w[uk][static_cast<std::size_t>(a)] * m_w[uk][static_cast<std::size_t>(c)];
        }
        // Keeps the system solvable where few samples fall under a pole, without visibly
        // smoothing: relative to the average weight of a pole.
        const double lambda = 1e-3 * static_cast<double>(samples) / poles;
        for (int k = 0; k + 2 < poles; ++k) {
            const std::array<double, 3> c{1.0, -2.0, 1.0};
            for (int a = 0; a < 3; ++a)
                for (int d = a; d < 3; ++d)
                    ata[static_cast<std::size_t>(k + a)][static_cast<std::size_t>(d - a)] +=
                        lambda * c[static_cast<std::size_t>(a)] * c[static_cast<std::size_t>(d)];
        }
        // Banded Cholesky (bandwidth 3).
        for (int i = 0; i < poles; ++i) {
            auto& row = m_chol[static_cast<std::size_t>(i)];
            row = ata[static_cast<std::size_t>(i)];
            for (int k = std::max(0, i - 3); k < i; ++k) {
                const auto& rk = m_chol[static_cast<std::size_t>(k)];
                const double f = rk[static_cast<std::size_t>(i - k)];
                for (int j = i; j <= std::min(poles - 1, k + 3); ++j)
                    row[static_cast<std::size_t>(j - i)] -= f * rk[static_cast<std::size_t>(j - k)];
            }
            const double d = std::sqrt(std::max(row[0], 1e-12));
            for (auto& x : row)
                x /= d;
        }
    }

    void solve(const double* in, std::ptrdiff_t inStride, double* out, std::ptrdiff_t outStride) const
    {
        std::vector<double> y(static_cast<std::size_t>(m_p), 0.0);
        for (int k = 0; k < m_m; ++k) {
            const auto uk = static_cast<std::size_t>(k);
            const double s = in[k * inStride];
            for (int a = 0; a < 4; ++a)
                y[static_cast<std::size_t>(m_first[uk] + a)] += m_w[uk][static_cast<std::size_t>(a)] * s;
        }
        for (int i = 0; i < m_p; ++i) {
            double v = y[static_cast<std::size_t>(i)];
            for (int k = std::max(0, i - 3); k < i; ++k)
                v -= m_chol[static_cast<std::size_t>(k)][static_cast<std::size_t>(i - k)] * y[static_cast<std::size_t>(k)];
            y[static_cast<std::size_t>(i)] = v / m_chol[static_cast<std::size_t>(i)][0];
        }
        for (int i = m_p - 1; i >= 0; --i) {
            double v = y[static_cast<std::size_t>(i)];
            for (int k = i + 1; k <= std::min(m_p - 1, i + 3); ++k)
                v -= m_chol[static_cast<std::size_t>(i)][static_cast<std::size_t>(k - i)] * y[static_cast<std::size_t>(k)];
            y[static_cast<std::size_t>(i)] = v / m_chol[static_cast<std::size_t>(i)][0];
        }
        for (int i = 0; i < m_p; ++i)
            out[i * outStride] = y[static_cast<std::size_t>(i)];
    }

    [[nodiscard]] int first(int k) const { return m_first[static_cast<std::size_t>(k)]; }
    [[nodiscard]] const std::array<double, 4>& weights(int k) const { return m_w[static_cast<std::size_t>(k)]; }

private:
    int m_m;
    int m_p;
    std::vector<int> m_first;
    std::vector<std::array<double, 4>> m_w;
    std::vector<std::array<double, 4>> m_chol;
};

/// Rolling ball over a height field: the true offset of the skin by `distance` (> 0 up,
/// < 0 down). Unlike moving each point along its normal it cannot fold in sharp concave
/// details. The ball touches between samples when it is small against their spacing (on a
/// slope, off to the side), so it is sampled finer than the grid, on the heights interpolated
/// between samples.
std::vector<double> rollBall(const std::vector<double>& h, const Raster& r, double distance)
{
    const double t = std::abs(distance);
    const double sign = distance > 0.0 ? 1.0 : -1.0;
    struct Offset {
        int di, dj;     ///< Sample cell, relative.
        double fu, fv;  ///< Position in the cell, 0 to 1.
        double lift;
    };
    std::vector<Offset> disk;
    // At least 4 steps per radius, in whole fractions of the sample spacing.
    const int subU = std::max(1, static_cast<int>(std::ceil(4.0 * r.du / t)));
    const int subV = std::max(1, static_cast<int>(std::ceil(4.0 * r.dv / t)));
    const double stepU = r.du / subU, stepV = r.dv / subV;
    const int ri = static_cast<int>(t / stepU), rj = static_cast<int>(t / stepV);
    for (int j = -rj; j <= rj; ++j)
        for (int i = -ri; i <= ri; ++i) {
            const double x = i * stepU, y = j * stepV;
            const double d2 = x * x + y * y;
            if (d2 > t * t)
                continue;
            const int ci = static_cast<int>(std::floor(static_cast<double>(i) / subU));
            const int cj = static_cast<int>(std::floor(static_cast<double>(j) / subV));
            disk.push_back({ci, cj, static_cast<double>(i - ci * subU) / subU,
                            static_cast<double>(j - cj * subV) / subV, std::sqrt(t * t - d2)});
        }
    std::vector<double> out(h.size());
    for (int j = 0; j < r.nv; ++j)
        for (int i = 0; i < r.nu; ++i) {
            double best = -std::numeric_limits<double>::max();
            for (const auto& o : disk) {
                const int a = i + o.di, b = j + o.dj;
                const int a1 = o.fu > 0.0 ? a + 1 : a, b1 = o.fv > 0.0 ? b + 1 : b;
                if (a < 0 || b < 0 || a1 >= r.nu || b1 >= r.nv)
                    continue;
                const double z = (1.0 - o.fv) * ((1.0 - o.fu) * h[r.at(a, b)] + o.fu * h[r.at(a1, b)]) +
                                 o.fv * ((1.0 - o.fu) * h[r.at(a, b1)] + o.fu * h[r.at(a1, b1)]);
                best = std::max(best, sign * z + o.lift);
            }
            out[r.at(i, j)] = sign * best;
        }
    return out;
}

std::vector<double> solvePoles(const std::vector<double>& heights, const Raster& r, const AxisFit& fitU,
                               const AxisFit& fitV, int pu, int pv)
{
    // Tensor product least squares, axis by axis: every row, then every column.
    std::vector<double> rows(static_cast<std::size_t>(pu) * r.nv);
    for (int j = 0; j < r.nv; ++j)
        fitU.solve(&heights[r.at(0, j)], 1, &rows[static_cast<std::size_t>(j) * pu], 1);
    std::vector<double> poles(static_cast<std::size_t>(pu) * pv, 0.0);
    for (int i = 0; i < pu; ++i)
        fitV.solve(&rows[static_cast<std::size_t>(i)], pu, &poles[static_cast<std::size_t>(i)], pu);
    return poles;
}

/// The surface with the sketch coordinates as its (u, v) parameters.
Handle(Geom_BSplineSurface) makeSurface(const DrapeArea& area, const std::vector<double>& poles, int pu, int pv)
{
    const double wide = area.u1 - area.u0, high = area.v1 - area.v0;
    // Greville abscissae: on them the surface's u and v are exactly the sketch coordinates.
    auto greville = [](int k, int n) {
        const int spans = n - 3;
        double sum = 0.0;
        for (int d = 1; d <= 3; ++d)
            sum += std::clamp(k + d - 3, 0, spans);
        return sum / 3.0 / spans;
    };
    TColgp_Array2OfPnt net(1, pu, 1, pv);
    for (int j = 0; j < pv; ++j)
        for (int i = 0; i < pu; ++i) {
            const double u = area.u0 + wide * greville(i, pu), v = area.v0 + high * greville(j, pv);
            const core::Vec3 p =
                area.origin + area.x * u + area.y * v + area.normal * poles[static_cast<std::size_t>(j) * pu + i];
            net.SetValue(i + 1, j + 1, gp_Pnt(p.x, p.y, p.z));
        }
    auto knots = [](int n, double lo, double size, TColStd_Array1OfReal& k, TColStd_Array1OfInteger& m) {
        const int spans = n - 3;
        for (int s = 0; s <= spans; ++s) {
            k.SetValue(s + 1, lo + size * s / spans);
            m.SetValue(s + 1, s == 0 || s == spans ? 4 : 1);
        }
    };
    TColStd_Array1OfReal ku(1, pu - 2), kv(1, pv - 2);
    TColStd_Array1OfInteger mu(1, pu - 2), mv(1, pv - 2);
    knots(pu, area.u0, wide, ku, mu);
    knots(pv, area.v0, high, kv, mv);
    return new Geom_BSplineSurface(net, ku, kv, mu, mv, 3, 3);
}

/// A wire of a planar outline carried over to a height-field surface: the same curves in
/// sketch coordinates, as curves on the surface.
struct LaidWire {
    TopoDS_Wire wire;
    std::vector<TopoDS_Edge> edges; ///< In wire order.
};

LaidWire layWire(const TopoDS_Wire& source, const gp_Pln& plane, const Handle(Geom_BSplineSurface)& surface)
{
    LaidWire out;
    BRepBuilderAPI_MakeWire maker;
    for (BRepTools_WireExplorer e(source); e.More(); e.Next()) {
        const TopoDS_Edge& edge = e.Current();
        double first = 0.0, last = 0.0;
        const Handle(Geom_Curve) curve = BRep_Tool::Curve(edge, first, last);
        if (curve.IsNull())
            continue;
        const Handle(Geom2d_Curve) onPlane = GeomAPI::To2d(curve, plane);
        BRepBuilderAPI_MakeEdge makeEdge(onPlane, surface, first, last);
        if (!makeEdge.IsDone())
            throw SurfaceFitError("Could not lay the outline on the surface");
        TopoDS_Edge laid = makeEdge.Edge();
        BRepLib::BuildCurves3d(laid, 1e-3);
        laid.Orientation(edge.Orientation());
        maker.Add(laid);
        if (!maker.IsDone())
            throw SurfaceFitError("The outline is not a closed loop");
        out.edges.push_back(laid);
    }
    out.wire = maker.Wire();
    return out;
}

/// Bottom, top and walls between them for one outline (a planar face).
TopoDS_Shape buildSolid(const TopoDS_Shape& outline, const gp_Pln& plane, const Handle(Geom_BSplineSurface)& bottom,
                        const Handle(Geom_BSplineSurface)& top, double& bottomArea)
{
    BRepBuilderAPI_Sewing sewing(1e-2);
    const TopoDS_Face source = TopoDS::Face(outline);
    const TopoDS_Wire outer = BRepTools::OuterWire(source);
    auto facesOn = [&](const Handle(Geom_BSplineSurface)& surface, std::vector<LaidWire>& laid) {
        laid.clear();
        laid.push_back(layWire(outer, plane, surface));
        BRepBuilderAPI_MakeFace onOuter(surface, laid.front().wire);
        for (TopExp_Explorer w(source, TopAbs_WIRE); w.More(); w.Next()) {
            const TopoDS_Wire& wire = TopoDS::Wire(w.Current());
            if (wire.IsSame(outer))
                continue;
            laid.push_back(layWire(wire, plane, surface));
            onOuter.Add(laid.back().wire);
        }
        ShapeFix_Face fix(onOuter.Face());
        fix.FixOrientation();
        fix.Perform();
        return fix.Face();
    };
    std::vector<LaidWire> lower, upper;
    const TopoDS_Face bottomFace = facesOn(bottom, lower);
    const TopoDS_Face topFace = facesOn(top, upper);
    {
        GProp_GProps props;
        BRepGProp::SurfaceProperties(bottomFace, props);
        bottomArea += props.Mass();
    }
    sewing.Add(bottomFace);
    sewing.Add(topFace);
    for (std::size_t w = 0; w < lower.size(); ++w)
        for (std::size_t e = 0; e < lower[w].edges.size(); ++e)
            sewing.Add(BRepFill::Face(lower[w].edges[e], upper[w].edges[e]));
    sewing.Perform();
    const TopoDS_Shape sewn = sewing.SewedShape();
    TopExp_Explorer shells(sewn, TopAbs_SHELL);
    if (!shells.More())
        throw SurfaceFitError("Could not close the part: the outline's walls do not meet the surfaces");
    BRepBuilderAPI_MakeSolid makeSolid(TopoDS::Shell(shells.Current()));
    if (!makeSolid.IsDone())
        throw SurfaceFitError("Could not close the part into a solid");
    TopoDS_Shape solid = makeSolid.Solid();
    ShapeFix_Shape fix(solid);
    fix.Perform();
    solid = fix.Shape();
    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    if (props.Mass() < 0.0)
        solid.Reverse();
    return solid;
}

} // namespace

namespace {

/// The skin under the area: samples, gaps filled, and its B-spline poles refined until
/// the skin under the inside rectangle is followed within the tolerance.
struct SkinFit {
    Raster raster;
    std::vector<double> skin;
    std::vector<double> poles;
    int pu = 0;
    int pv = 0;
    DrapeFitReport report;
};

SkinFit fitSkin(const core::MeshProjector& scan, const DrapeArea& area, const DrapeFitOptions& options)
{
    const double wide = area.u1 - area.u0, high = area.v1 - area.v0;
    if (!(wide > 0.0) || !(high > 0.0))
        throw SurfaceFitError("The outline is empty");
    const double longest = std::max(wide, high);
    const double finest = std::max(options.minSpacing, longest / 64.0);

    // Samples at a third of the finest pole spacing, and enough of them across a small
    // outline in a larger area (at most 2048 per side).
    const double smallest = std::min(area.insideU1 - area.insideU0, area.insideV1 - area.insideV0);
    Raster raster = sampleSkin(scan, area, std::max(std::min(finest / 3.0, smallest / 8.0), longest / 2047.0));
    keepMainSheet(raster, area, options.maxSlope);

    std::size_t insideSamples = 0, insideKnown = 0;
    for (int j = 0; j < raster.nv; ++j)
        for (int i = 0; i < raster.nu; ++i) {
            const double u = area.u0 + i * raster.du, v = area.v0 + j * raster.dv;
            if (u < area.insideU0 || u > area.insideU1 || v < area.insideV0 || v > area.insideV1)
                continue;
            ++insideSamples;
            insideKnown += raster.known[raster.at(i, j)] != 0 ? 1U : 0U;
        }
    if (insideKnown < 16)
        throw SurfaceFitError("The scan is not under the outline (seen along the sketch normal)");
    // Before the gaps are filled: only scanned samples tell the noise. Samples about half
    // the finest pole spacing apart, so the five span two of them.
    const int noiseLag = std::max(1, static_cast<int>(std::lround(0.5 * finest / raster.du)));
    const double noise = estimateNoise(raster, area, noiseLag);
    std::vector<double> skin = fillGaps(raster);

    SkinFit result;
    result.raster = std::move(raster);
    result.skin = std::move(skin);
    result.report.bridgedFraction = 1.0 - static_cast<double>(insideKnown) / static_cast<double>(insideSamples);
    result.report.scanNoise = noise;

    // Refine the control net until the skin is followed within the tolerance.
    int pu = 0, pv = 0;
    for (double spacing = std::max(finest, longest / 16.0);; spacing = std::max(finest, spacing * 0.5)) {
        pu = std::clamp(static_cast<int>(std::ceil(wide / spacing)) + 3, 4, result.raster.nu);
        pv = std::clamp(static_cast<int>(std::ceil(high / spacing)) + 3, 4, result.raster.nv);
        const AxisFit fitU(result.raster.nu, pu), fitV(result.raster.nv, pv);
        result.poles = solvePoles(result.skin, result.raster, fitU, fitV, pu, pv);

        // Deviation on the skin under the outline.
        std::vector<float> deviations;
        deviations.reserve(insideKnown);
        std::vector<double> residual(result.raster.z.size(), std::numeric_limits<double>::quiet_NaN());
        double squares = 0.0, worst = 0.0;
        for (int j = 0; j < result.raster.nv; ++j) {
            const double v = area.v0 + j * result.raster.dv;
            if (v < area.insideV0 || v > area.insideV1)
                continue;
            const auto& wv = fitV.weights(j);
            const int fv = fitV.first(j);
            for (int i = 0; i < result.raster.nu; ++i) {
                const double u = area.u0 + i * result.raster.du;
                if (u < area.insideU0 || u > area.insideU1 || !result.raster.known[result.raster.at(i, j)])
                    continue;
                const auto& wu = fitU.weights(i);
                const int fu = fitU.first(i);
                double z = 0.0;
                for (int b = 0; b < 4; ++b)
                    for (int a = 0; a < 4; ++a)
                        z += wv[static_cast<std::size_t>(b)] * wu[static_cast<std::size_t>(a)] *
                             result.poles[static_cast<std::size_t>(fv + b) * pu + static_cast<std::size_t>(fu + a)];
                residual[result.raster.at(i, j)] = z - result.raster.z[result.raster.at(i, j)];
                const double d = std::abs(residual[result.raster.at(i, j)]);
                worst = std::max(worst, d);
                squares += d * d;
                deviations.push_back(static_cast<float>(d));
            }
        }
        auto nth = deviations.begin() + static_cast<std::ptrdiff_t>(0.99 * static_cast<double>(deviations.size()));
        std::nth_element(deviations.begin(), nth, deviations.end());
        result.report.maxDeviation = worst;
        result.report.p99Deviation = *nth;
        result.report.rmsDeviation = std::sqrt(squares / static_cast<double>(deviations.size()));
        // What is left is the scan's own noise when it is spread like noise: 99% within
        // 3 deviations (2.58 for normal noise), the rms not above it, and no pattern in it
        // (fine shape the noise estimate took for noise). Finer control points would only
        // follow the noise, making the surface bumpy, not closer to the shape.
        result.report.withinScanNoise = result.report.p99Deviation > options.tolerance &&
                                        result.report.p99Deviation <= 3.0 * noise &&
                                        result.report.rmsDeviation <= 1.2 * noise &&
                                        residualLooksLikeNoise(residual, result.raster.nu, result.raster.nv, noiseLag);
        // Single spikes (a loose triangle, a wall) do not drive the refinement.
        if (result.report.p99Deviation <= options.tolerance || result.report.withinScanNoise || spacing <= finest)
            break;
    }
    result.pu = pu;
    result.pv = pv;
    result.report.uPoles = pu;
    result.report.vPoles = pv;
    return result;
}

} // namespace

DrapedPart partOnScan(const core::MeshProjector& scan, const DrapeArea& area, const std::vector<Shape>& outlines,
                      double thickness, ThickenSide side, const DrapeFitOptions& options, const DrapeShaping& shaping)
{
    const auto started = std::chrono::steady_clock::now();
    if (!(thickness > 0.0))
        throw SurfaceFitError("Thickness must be positive");
    SkinFit fit = fitSkin(scan, area, options);
    const Raster& raster = fit.raster;
    const std::vector<double>& skin = fit.skin;
    const std::vector<double>& skinPoles = fit.poles;
    const int pu = fit.pu, pv = fit.pv;
    DrapedPart result;
    result.report = fit.report;

    const AxisFit fitU(raster.nu, pu), fitV(raster.nv, pv);
    std::vector<double> lower, upper;
    switch (side) {
    case ThickenSide::Outside:
        lower = skinPoles;
        upper = solvePoles(rollBall(skin, raster, thickness), raster, fitU, fitV, pu, pv);
        break;
    case ThickenSide::Inside:
        lower = solvePoles(rollBall(skin, raster, -thickness), raster, fitU, fitV, pu, pv);
        upper = skinPoles;
        break;
    case ThickenSide::Both:
        lower = solvePoles(rollBall(skin, raster, -0.5 * thickness), raster, fitU, fitV, pu, pv);
        upper = solvePoles(rollBall(skin, raster, 0.5 * thickness), raster, fitU, fitV, pu, pv);
        break;
    }
    int upperU = pu, upperV = pv, lowerU = pu, lowerV = pv;
    if (shaping.outerLift || shaping.thicknessDelta) {
        // The shaped surfaces are solved again from the skin plus the shaping, on a control net fine
        // enough for it (the net of the skin alone is as coarse as the skin allows).
        const double wide = area.u1 - area.u0, high = area.v1 - area.v0;
        // The shaping is sampled where the skin is, so a feature has to span a few samples to be there at all.
        const double sample = std::max(raster.du, raster.dv);
        if (shaping.featureSize > 0.0 && shaping.featureSize < 2.0 * sample)
            throw SurfaceFitError("The ridge or its fade is too narrow for this scan's sampling here: make the width and "
                                  "the fade lengths at least " + std::to_string(std::ceil(2.0 * sample * 10.0) / 10.0) +
                                  " mm");
        const double spacing = std::max({2.0 * raster.du, 2.0 * raster.dv, shaping.featureSize / 5.0,
                                         std::max(wide, high) / 200.0});
        const int su = std::clamp(static_cast<int>(std::ceil(wide / spacing)) + 3, pu, raster.nu);
        const int sv = std::clamp(static_cast<int>(std::ceil(high / spacing)) + 3, pv, raster.nv);
        const AxisFit shapeU(raster.nu, su), shapeV(raster.nv, sv);
        // The fitted skin at the samples: what the shaped surfaces are built on, so that they stay in step
        // with the surface lying on the scan (the raw samples carry the scan's noise).
        std::vector<double> smooth(skin.size());
        for (int j = 0; j < raster.nv; ++j) {
            const auto& wv = fitV.weights(j);
            const int fv = fitV.first(j);
            for (int i = 0; i < raster.nu; ++i) {
                const auto& wu = fitU.weights(i);
                const int fu = fitU.first(i);
                double z = 0.0;
                for (int b = 0; b < 4; ++b)
                    for (int a = 0; a < 4; ++a)
                        z += wv[static_cast<std::size_t>(b)] * wu[static_cast<std::size_t>(a)] *
                             skinPoles[static_cast<std::size_t>(fv + b) * pu + static_cast<std::size_t>(fu + a)];
                smooth[raster.at(i, j)] = z;
            }
        }
        // A change of thickness is along the surface's normal, a height over the sketch plane is steeper by
        // sqrt(1 + slope^2) where the skin is inclined to it.
        std::vector<double> steep(skin.size(), 1.0);
        for (int j = 0; j < raster.nv; ++j)
            for (int i = 0; i < raster.nu; ++i) {
                const int i0 = std::max(i - 1, 0), i1 = std::min(i + 1, raster.nu - 1);
                const int j0 = std::max(j - 1, 0), j1 = std::min(j + 1, raster.nv - 1);
                const double gx = (smooth[raster.at(i1, j)] - smooth[raster.at(i0, j)]) / ((i1 - i0) * raster.du);
                const double gy = (smooth[raster.at(i, j1)] - smooth[raster.at(i, j0)]) / ((j1 - j0) * raster.dv);
                steep[raster.at(i, j)] = std::sqrt(1.0 + gx * gx + gy * gy);
            }
        // f(u, v, steepness).
        auto shaped = [&](const std::vector<double>& base, const std::function<double(double, double, double)>& f) {
            std::vector<double> h = base;
            for (int j = 0; j < raster.nv; ++j)
                for (int i = 0; i < raster.nu; ++i)
                    h[raster.at(i, j)] += f(area.u0 + i * raster.du, area.v0 + j * raster.dv, steep[raster.at(i, j)]);
            return solvePoles(h, raster, shapeU, shapeV, su, sv);
        };
        // The taper goes to the surface away from the scan; both share it when the part is on both sides.
        const double upperShare = side == ThickenSide::Outside ? 1.0 : side == ThickenSide::Both ? 0.5 : 0.0;
        const double lowerShare = side == ThickenSide::Inside ? 1.0 : side == ThickenSide::Both ? 0.5 : 0.0;
        const auto delta = [&](double u, double v, double share) {
            return shaping.thicknessDelta ? share * shaping.thicknessDelta(u, v) : 0.0;
        };
        const auto lift = [&](double u, double v) { return shaping.outerLift ? shaping.outerLift(u, v) : 0.0; };
        // Only the shaped surfaces are solved again, from the unshaped height field plus the shaping.
        // The one lying on the scan keeps the skin fit.
        if (shaping.outerLift || (shaping.thicknessDelta && upperShare > 0.0)) {
            const std::vector<double> flat = side == ThickenSide::Inside    ? smooth
                                             : side == ThickenSide::Outside ? rollBall(smooth, raster, thickness)
                                                                            : rollBall(smooth, raster, 0.5 * thickness);
            upper = shaped(flat, [&](double u, double v, double k) { return lift(u, v) + k * delta(u, v, upperShare); });
            upperU = su;
            upperV = sv;
        }
        if (shaping.thicknessDelta && lowerShare > 0.0) {
            const std::vector<double> flat = side == ThickenSide::Inside ? rollBall(smooth, raster, -thickness)
                                                                         : rollBall(smooth, raster, -0.5 * thickness);
            lower = shaped(flat, [&](double u, double v, double k) { return -k * delta(u, v, lowerShare); });
            lowerU = su;
            lowerV = sv;
        }
    }
    result.report.milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    try {
        const auto bottom = makeSurface(area, lower, lowerU, lowerV);
        const auto top = makeSurface(area, upper, upperU, upperV);
        const gp_Pln plane(gp_Ax3(gp_Pnt(area.origin.x, area.origin.y, area.origin.z),
                                  gp_Dir(area.normal.x, area.normal.y, area.normal.z),
                                  gp_Dir(area.x.x, area.x.y, area.x.z)));
        std::vector<TopoDS_Shape> solids;
        for (const auto& o : outlines) {
            if (o.isNull())
                continue;
            for (TopExp_Explorer f(o.impl()->shape, TopAbs_FACE); f.More(); f.Next())
                solids.push_back(buildSolid(f.Current(), plane, bottom, top, result.skinArea));
        }
        if (solids.empty())
            throw SurfaceFitError("No closed outline");
        if (solids.size() == 1) {
            result.solid = wrapShape(solids.front());
        } else {
            TopoDS_Compound c;
            BRep_Builder b;
            b.MakeCompound(c);
            for (const auto& solid : solids)
                b.Add(c, solid);
            result.solid = wrapShape(c);
        }
        if (!BRepCheck_Analyzer(result.solid.impl()->shape).IsValid())
            throw SurfaceFitError("The part came out invalid; try a simpler outline");
    } catch (const Standard_Failure& e) {
        throw SurfaceFitError(std::string("Could not build the part: ") + e.GetMessageString());
    }
    return result;
}

Shape skinOnScan(const core::MeshProjector& scan, const DrapeArea& area, DrapeFitReport* report,
                 const DrapeFitOptions& options)
{
    const auto started = std::chrono::steady_clock::now();
    const SkinFit fit = fitSkin(scan, area, options);
    try {
        BRepBuilderAPI_MakeFace face(makeSurface(area, fit.poles, fit.pu, fit.pv), Precision::Confusion());
        if (!face.IsDone())
            throw SurfaceFitError("Could not build the skin surface");
        if (report != nullptr) {
            *report = fit.report;
            report->milliseconds =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        }
        return wrapShape(face.Face());
    } catch (const Standard_Failure& e) {
        throw SurfaceFitError(std::string("Could not build the skin surface: ") + e.GetMessageString());
    }
}

} // namespace aerocad::geometry
