// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshIntersect.hpp"

#include "aerocad/core/MeshCheck.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <vector>

namespace aerocad::core {

double MeshCrossing::length() const
{
    const Vec3 d = to - from;
    return std::sqrt(d.dot(d));
}

namespace {

using LD = long double;

struct LV {
    LD x = 0, y = 0, z = 0;
    LV() = default;
    LV(LD x_, LD y_, LD z_) : x(x_), y(y_), z(z_) {}
    explicit LV(const Vec3& v) : x(v.x), y(v.y), z(v.z) {}
    LV operator-(const LV& o) const { return {x - o.x, y - o.y, z - o.z}; }
    LV operator+(const LV& o) const { return {x + o.x, y + o.y, z + o.z}; }
    LV operator*(LD s) const { return {x * s, y * s, z * s}; }
    [[nodiscard]] LD dot(const LV& o) const { return x * o.x + y * o.y + z * o.z; }
    [[nodiscard]] LV cross(const LV& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    [[nodiscard]] Vec3 toVec3() const { return {static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)}; }
};

using Tri = std::array<LV, 3>;

// Exact signs of the determinants the tests are made of (CAD-95 review: long double alone
// rounds to the wrong sign for coplanar input). A floating-point filter decides the clear
// cases; the rest is summed as non-overlapping expansions of doubles (Shewchuk), which is
// exact. Inputs are always mesh vertices, i.e. doubles, so the conversions are exact too.
namespace exact {

using Expansion = std::vector<double>; ///< non-overlapping, increasing magnitude, no zeros

inline void twoSum(double a, double b, double& x, double& y)
{
    x = a + b;
    const double bv = x - a, av = x - bv;
    y = (a - av) + (b - bv);
}

inline Expansion grow(const Expansion& e, double b)
{
    Expansion h;
    h.reserve(e.size() + 1);
    double q = b;
    for (const double ei : e) {
        double next, low;
        twoSum(q, ei, next, low);
        q = next;
        if (low != 0.0)
            h.push_back(low);
    }
    if (q != 0.0)
        h.push_back(q);
    return h;
}

inline Expansion add(Expansion a, const Expansion& b)
{
    for (const double bi : b)
        a = grow(a, bi);
    return a;
}

inline Expansion negate(Expansion a)
{
    for (auto& v : a)
        v = -v;
    return a;
}

inline Expansion scale(const Expansion& e, double b)
{
    Expansion out;
    for (const double ei : e) {
        const double hi = ei * b, lo = std::fma(ei, b, -hi);
        if (lo != 0.0)
            out = grow(out, lo);
        if (hi != 0.0)
            out = grow(out, hi);
    }
    return out;
}

inline Expansion mul(const Expansion& a, const Expansion& b)
{
    Expansion out;
    for (const double ai : a)
        out = add(std::move(out), scale(b, ai));
    return out;
}

inline Expansion diff(double a, double b)
{
    double x, y;
    twoSum(a, -b, x, y);
    Expansion e;
    if (y != 0.0)
        e.push_back(y);
    if (x != 0.0)
        e.push_back(x);
    return e;
}

inline int sign(const Expansion& e)
{
    return e.empty() ? 0 : (e.back() > 0 ? 1 : -1);
}

constexpr double kEps = 1.1102230246251565e-16; // 2^-53

/// Sign of (a-d) . ((b-d) x (c-d)).
inline int orient3(const double* a, const double* b, const double* c, const double* d)
{
    const double adx = a[0] - d[0], bdx = b[0] - d[0], cdx = c[0] - d[0];
    const double ady = a[1] - d[1], bdy = b[1] - d[1], cdy = c[1] - d[1];
    const double adz = a[2] - d[2], bdz = b[2] - d[2], cdz = c[2] - d[2];
    const double det = adx * (bdy * cdz - bdz * cdy) + ady * (bdz * cdx - bdx * cdz) + adz * (bdx * cdy - bdy * cdx);
    const double permanent = (std::abs(bdy * cdz) + std::abs(bdz * cdy)) * std::abs(adx) +
                             (std::abs(bdz * cdx) + std::abs(bdx * cdz)) * std::abs(ady) +
                             (std::abs(bdx * cdy) + std::abs(bdy * cdx)) * std::abs(adz);
    const double bound = (7.0 + 56.0 * kEps) * kEps * permanent;
    if (det > bound)
        return 1;
    if (-det > bound)
        return -1;
    const Expansion ex[3] = {diff(a[0], d[0]), diff(a[1], d[1]), diff(a[2], d[2])};
    const Expansion bx[3] = {diff(b[0], d[0]), diff(b[1], d[1]), diff(b[2], d[2])};
    const Expansion cx[3] = {diff(c[0], d[0]), diff(c[1], d[1]), diff(c[2], d[2])};
    auto minor = [&](int i, int j) { return add(mul(bx[i], cx[j]), negate(mul(bx[j], cx[i]))); };
    Expansion total = mul(ex[0], minor(1, 2));
    total = add(std::move(total), mul(ex[1], minor(2, 0)));
    total = add(std::move(total), mul(ex[2], minor(0, 1)));
    return sign(total);
}

/// The sign of orient3() when the floating-point filter decides it, 0 when it does not
/// (which can also mean the points are coplanar).
inline int orient3Filtered(const double* a, const double* b, const double* c, const double* d)
{
    const double adx = a[0] - d[0], bdx = b[0] - d[0], cdx = c[0] - d[0];
    const double ady = a[1] - d[1], bdy = b[1] - d[1], cdy = c[1] - d[1];
    const double adz = a[2] - d[2], bdz = b[2] - d[2], cdz = c[2] - d[2];
    const double det = adx * (bdy * cdz - bdz * cdy) + ady * (bdz * cdx - bdx * cdz) + adz * (bdx * cdy - bdy * cdx);
    const double permanent = (std::abs(bdy * cdz) + std::abs(bdz * cdy)) * std::abs(adx) +
                             (std::abs(bdz * cdx) + std::abs(bdx * cdz)) * std::abs(ady) +
                             (std::abs(bdx * cdy) + std::abs(bdy * cdx)) * std::abs(adz);
    const double bound = (7.0 + 56.0 * kEps) * kEps * permanent;
    return det > bound ? 1 : (-det > bound ? -1 : 0);
}

/// Sign of (b-a) x (c-a) in the plane.
inline int orient2(double au, double av, double bu, double bv, double cu, double cv)
{
    const double left = (bu - au) * (cv - av), right = (bv - av) * (cu - au);
    const double det = left - right;
    const double bound = (3.0 + 16.0 * kEps) * kEps * (std::abs(left) + std::abs(right));
    if (det > bound)
        return 1;
    if (-det > bound)
        return -1;
    const Expansion l = mul(diff(bu, au), diff(cv, av)), r = mul(diff(bv, av), diff(cu, au));
    return sign(add(l, negate(r)));
}

} // namespace exact

/// Sign-carrying volume of the tetrahedron abcd: positive when d lies below the plane of
/// abc seen from the side its normal points to. The sign is exact (the value is not: it is
/// only used to place the crossing point, and is made to agree with the sign).
LD orient(const LV& a, const LV& b, const LV& c, const LV& d)
{
    const double pa[3] = {static_cast<double>(a.x), static_cast<double>(a.y), static_cast<double>(a.z)};
    const double pb[3] = {static_cast<double>(b.x), static_cast<double>(b.y), static_cast<double>(b.z)};
    const double pc[3] = {static_cast<double>(c.x), static_cast<double>(c.y), static_cast<double>(c.z)};
    const double pd[3] = {static_cast<double>(d.x), static_cast<double>(d.y), static_cast<double>(d.z)};
    const int sign = exact::orient3(pa, pb, pc, pd);
    LD value = (a - d).dot((b - d).cross(c - d));
    if (sign == 0)
        return 0;
    if ((value > 0) != (sign > 0) || value == 0)
        value = sign * std::numeric_limits<LD>::min();
    return value;
}

/// Where the segment PQ cuts the triangle. `strict`: only through its inside (not its
/// border, not with an end on its plane).
bool pierce(const LV& p, const LV& q, const Tri& t, bool strict, LV& at)
{
    const LD d1 = orient(t[0], t[1], t[2], p), d2 = orient(t[0], t[1], t[2], q);
    if ((d1 > 0 && d2 > 0) || (d1 < 0 && d2 < 0) || (d1 == 0 && d2 == 0))
        return false;
    if (strict && (d1 == 0 || d2 == 0))
        return false;
    const LD s1 = orient(p, q, t[0], t[1]), s2 = orient(p, q, t[1], t[2]), s3 = orient(p, q, t[2], t[0]);
    if (strict) {
        if (!((s1 > 0 && s2 > 0 && s3 > 0) || (s1 < 0 && s2 < 0 && s3 < 0)))
            return false;
    } else {
        const bool pos = s1 > 0 || s2 > 0 || s3 > 0, neg = s1 < 0 || s2 < 0 || s3 < 0;
        if (pos && neg)
            return false;
    }
    at = p + (q - p) * (d1 / (d1 - d2));
    return true;
}

int dominantAxis(const LV& n)
{
    const LD ax = std::abs(n.x), ay = std::abs(n.y), az = std::abs(n.z);
    return ax >= ay && ax >= az ? 0 : (ay >= az ? 1 : 2);
}

struct P2 {
    LD u, v;
};

P2 project(const LV& p, int axis)
{
    return axis == 0 ? P2{p.y, p.z} : (axis == 1 ? P2{p.z, p.x} : P2{p.x, p.y});
}

LD orient2(const P2& a, const P2& b, const P2& c)
{
    const int sign = exact::orient2(static_cast<double>(a.u), static_cast<double>(a.v), static_cast<double>(b.u),
                                    static_cast<double>(b.v), static_cast<double>(c.u), static_cast<double>(c.v));
    return static_cast<LD>(sign);
}

bool strictlyInside(const P2& p, const std::array<P2, 3>& t)
{
    const LD s1 = orient2(t[0], t[1], p), s2 = orient2(t[1], t[2], p), s3 = orient2(t[2], t[0], p);
    return (s1 > 0 && s2 > 0 && s3 > 0) || (s1 < 0 && s2 < 0 && s3 < 0);
}

/// Two triangles in one plane share some area.
bool coplanarOverlap(const Tri& a, const Tri& b)
{
    const int axis = dominantAxis((a[1] - a[0]).cross(a[2] - a[0]));
    std::array<P2, 3> pa, pb;
    for (int i = 0; i < 3; ++i) {
        pa[static_cast<std::size_t>(i)] = project(a[static_cast<std::size_t>(i)], axis);
        pb[static_cast<std::size_t>(i)] = project(b[static_cast<std::size_t>(i)], axis);
    }
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) {
            const P2 &a0 = pa[i], &a1 = pa[(i + 1) % 3], &b0 = pb[j], &b1 = pb[(j + 1) % 3];
            const LD o1 = orient2(a0, a1, b0), o2 = orient2(a0, a1, b1), o3 = orient2(b0, b1, a0),
                     o4 = orient2(b0, b1, a1);
            if (((o1 > 0 && o2 < 0) || (o1 < 0 && o2 > 0)) && ((o3 > 0 && o4 < 0) || (o3 < 0 && o4 > 0)))
                return true;
        }
    for (const auto& p : pa)
        if (strictlyInside(p, pb))
            return true;
    for (const auto& p : pb)
        if (strictlyInside(p, pa))
            return true;
    // Equal triangles, or one corner of each lying on the other's border with the rest inside.
    const P2 ca{(pa[0].u + pa[1].u + pa[2].u) / 3, (pa[0].v + pa[1].v + pa[2].v) / 3};
    const P2 cb{(pb[0].u + pb[1].u + pb[2].u) / 3, (pb[0].v + pb[1].v + pb[2].v) / 3};
    return strictlyInside(ca, pb) || strictlyInside(cb, pa);
}

struct Box {
    double lo[3], hi[3];
};

struct Node {
    Box box;
    std::uint32_t left = 0, right = 0; ///< children, or [first, first + count) of `order` for a leaf
    std::uint32_t count = 0;           ///< 0 for an inner node
};

class Bvh {
public:
    explicit Bvh(const std::vector<Box>& boxes) : m_boxes(boxes), m_order(boxes.size())
    {
        std::iota(m_order.begin(), m_order.end(), 0U);
        m_nodes.reserve(2 * boxes.size() / 4 + 2);
        if (!boxes.empty())
            build(0, static_cast<std::uint32_t>(boxes.size()));
    }

    template <typename Visit>
    void query(const Box& box, Visit&& visit) const
    {
        if (m_nodes.empty())
            return;
        // A median split halves the triangle count at every level, so the depth is at most
        // log2(count / 4) + 1 < 32 for any mesh that fits 32-bit indices.
        std::array<std::uint32_t, 128> stack{};
        std::size_t top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const Node& n = m_nodes[stack[--top]];
            if (!overlap(n.box, box))
                continue;
            if (n.count > 0) {
                for (std::uint32_t i = 0; i < n.count; ++i) {
                    const std::uint32_t t = m_order[n.left + i];
                    if (overlap(m_boxes[t], box))
                        visit(t);
                }
            } else if (top + 2 <= stack.size()) {
                stack[top++] = n.left;
                stack[top++] = n.right;
            }
        }
    }

private:
    static bool overlap(const Box& a, const Box& b)
    {
        for (int k = 0; k < 3; ++k)
            if (a.hi[k] < b.lo[k] || b.hi[k] < a.lo[k])
                return false;
        return true;
    }

    std::uint32_t build(std::uint32_t first, std::uint32_t count)
    {
        const auto index = static_cast<std::uint32_t>(m_nodes.size());
        m_nodes.emplace_back();
        Box box = m_boxes[m_order[first]];
        for (std::uint32_t i = 1; i < count; ++i) {
            const Box& b = m_boxes[m_order[first + i]];
            for (int k = 0; k < 3; ++k) {
                box.lo[k] = std::min(box.lo[k], b.lo[k]);
                box.hi[k] = std::max(box.hi[k], b.hi[k]);
            }
        }
        m_nodes[index].box = box;
        if (count <= 4) {
            m_nodes[index].left = first;
            m_nodes[index].count = count;
            return index;
        }
        int axis = 0;
        for (int k = 1; k < 3; ++k)
            if (box.hi[k] - box.lo[k] > box.hi[axis] - box.lo[axis])
                axis = k;
        const std::uint32_t mid = count / 2;
        std::nth_element(m_order.begin() + first, m_order.begin() + first + mid, m_order.begin() + first + count,
                         [&](std::uint32_t a, std::uint32_t b) {
                             return m_boxes[a].lo[axis] + m_boxes[a].hi[axis] < m_boxes[b].lo[axis] + m_boxes[b].hi[axis];
                         });
        const std::uint32_t l = build(first, mid);
        const std::uint32_t r = build(first + mid, count - mid);
        m_nodes[index].left = l;
        m_nodes[index].right = r;
        return index;
    }

    const std::vector<Box>& m_boxes;
    std::vector<std::uint32_t> m_order;
    std::vector<Node> m_nodes;
};

} // namespace


namespace {

struct Hit {
    LV from, to;
    bool coplanar = false;
};

using Tris = std::vector<TriangleMesh::Triangle>;

/// Do triangles i and j cross? (Pairs that share an edge or corner: see the header.)
std::optional<Hit> testPair(const Tris& tris, const std::vector<LV>& lv, std::uint32_t i, std::uint32_t j,
                            double minLength)
{
    const auto& ta = tris[i];
    const auto& tb = tris[j];
    const Tri A{lv[ta[0]], lv[ta[1]], lv[ta[2]]};
    const Tri B{lv[tb[0]], lv[tb[1]], lv[tb[2]]};
    int sharedA[3], sharedB[3], shared = 0;
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            if (ta[static_cast<std::size_t>(a)] == tb[static_cast<std::size_t>(b)]) {
                sharedA[shared] = a;
                sharedB[shared] = b;
                ++shared;
            }
    if (shared <= 1) {
        // Nearly every pair of neighbouring boxes is far apart in one of the planes: say so
        // with plain doubles before any long double work. (Corners other than the shared one
        // strictly on one side of the other plane can only touch it at the shared corner.)
        auto point = [&](std::uint32_t v, double (&out)[3]) {
            out[0] = static_cast<double>(lv[v].x);
            out[1] = static_cast<double>(lv[v].y);
            out[2] = static_cast<double>(lv[v].z);
        };
        auto oneSide = [&](const std::array<std::uint32_t, 3>& plane, const std::array<std::uint32_t, 3>& pts,
                           int skip) {
            double a[3], b[3], c[3], d[3];
            point(plane[0], a);
            point(plane[1], b);
            point(plane[2], c);
            int first = 0;
            for (int k = 0; k < 3; ++k) {
                if (k == skip)
                    continue;
                point(pts[static_cast<std::size_t>(k)], d);
                const int side = exact::orient3Filtered(a, b, c, d);
                if (side == 0 || (first != 0 && side != first))
                    return false;
                first = side;
            }
            return true;
        };
        if (oneSide(tb, ta, shared == 1 ? sharedA[0] : -1) || oneSide(ta, tb, shared == 1 ? sharedB[0] : -1))
            return std::nullopt;
    }
    if (shared == 3)
        return Hit{A[0], A[0], true};
    if (shared == 2) {
        // Folded back onto each other: the free corners on one side of the shared edge in
        // one plane.
        int fa = 0, fb = 0;
        for (int k = 0; k < 3; ++k) {
            if (k != sharedA[0] && k != sharedA[1])
                fa = k;
            if (k != sharedB[0] && k != sharedB[1])
                fb = k;
        }
        const LV& e0 = A[static_cast<std::size_t>(sharedA[0])];
        const LV& e1 = A[static_cast<std::size_t>(sharedA[1])];
        const LV& pa = A[static_cast<std::size_t>(fa)];
        const LV& pb = B[static_cast<std::size_t>(fb)];
        if (orient(e0, e1, pa, pb) == 0) {
            const int axis = dominantAxis((e1 - e0).cross(pa - e0));
            const LD sa = orient2(project(e0, axis), project(e1, axis), project(pa, axis));
            const LD sb = orient2(project(e0, axis), project(e1, axis), project(pb, axis));
            if (sa != 0 && sa == sb)
                return Hit{e0, e1, true};
        }
        return std::nullopt;
    }
    if (shared == 1) {
        // The triangles meet at the shared corner S, so they cross when they have any other
        // point in common: where an edge of one goes through the other, border included.
        const auto sa = static_cast<std::size_t>(sharedA[0]);
        const LV& corner = A[sa];
        LD best = 0;
        LV farthest = corner;
        for (std::size_t e = 0; e < 3; ++e) {
            LV at;
            for (int which = 0; which < 2; ++which) {
                const bool found = which == 0 ? pierce(A[e], A[(e + 1) % 3], B, false, at)
                                              : pierce(B[e], B[(e + 1) % 3], A, false, at);
                if (!found)
                    continue;
                const LV d = at - corner;
                const LD len = std::sqrt(d.dot(d));
                if (len > best) {
                    best = len;
                    farthest = at;
                }
            }
        }
        if (best > static_cast<LD>(minLength) && best > 0)
            return Hit{corner, farthest, false};
        if (orient(B[0], B[1], B[2], A[(sa + 1) % 3]) == 0 && orient(B[0], B[1], B[2], A[(sa + 2) % 3]) == 0 &&
            coplanarOverlap(A, B))
            return Hit{corner, corner, true};
        return std::nullopt;
    }
    std::array<LV, 6> pts;
    std::size_t n = 0;
    for (std::size_t e = 0; e < 3; ++e) {
        LV at;
        if (pierce(A[e], A[(e + 1) % 3], B, false, at))
            pts[n++] = at;
        if (pierce(B[e], B[(e + 1) % 3], A, false, at))
            pts[n++] = at;
    }
    if (n == 0) {
        if (orient(B[0], B[1], B[2], A[0]) == 0 && orient(B[0], B[1], B[2], A[1]) == 0 &&
            orient(B[0], B[1], B[2], A[2]) == 0 && coplanarOverlap(A, B))
            return Hit{A[0], A[0], true};
        return std::nullopt;
    }
    LD best = 0;
    std::size_t bi = 0, bj = 0;
    for (std::size_t p = 0; p < n; ++p)
        for (std::size_t q = p + 1; q < n; ++q) {
            const LV d = pts[p] - pts[q];
            const LD len = std::sqrt(d.dot(d));
            if (len > best) {
                best = len;
                bi = p;
                bj = q;
            }
        }
    if (best > static_cast<LD>(minLength))
        return Hit{pts[bi], pts[bj], false};
    return std::nullopt;
}

Box boxOf(const Tris& tris, const std::vector<LV>& lv, std::size_t i, double inflate)
{
    Box b{};
    for (int k = 0; k < 3; ++k) {
        b.lo[k] = std::numeric_limits<double>::max();
        b.hi[k] = std::numeric_limits<double>::lowest();
    }
    for (std::size_t c = 0; c < 3; ++c) {
        const LV& v = lv[tris[i][c]];
        const double p[3] = {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
        for (int k = 0; k < 3; ++k) {
            b.lo[k] = std::min(b.lo[k], p[k] - inflate);
            b.hi[k] = std::max(b.hi[k], p[k] + inflate);
        }
    }
    return b;
}

std::vector<LV> toLong(const std::vector<Vec3>& v)
{
    std::vector<LV> lv;
    lv.reserve(v.size());
    for (const auto& p : v)
        lv.emplace_back(p);
    return lv;
}

/// findSelfIntersections() for a mesh whose coinciding vertices are already merged.
SelfIntersections findInWelded(const TriangleMesh& mesh, double minLength, std::size_t maxExamples,
                               const CancelCheck& cancelled = {})
{
    SelfIntersections result;
    const Tris& tris = mesh.triangles();
    if (tris.size() < 2)
        return result;
    const std::vector<LV> lv = toLong(mesh.vertices());
    std::vector<Box> boxes(tris.size());
    for (std::size_t i = 0; i < tris.size(); ++i)
        boxes[i] = boxOf(tris, lv, i, 0.0);
    const Bvh bvh(boxes);
    for (std::size_t i = 0; i < tris.size(); ++i) {
        if ((i & 1023) == 0)
            throwIfCancelled(cancelled);
        bvh.query(boxes[i], [&](std::uint32_t j) {
            if (j <= i)
                return;
            const auto hit = testPair(tris, lv, static_cast<std::uint32_t>(i), j, minLength);
            if (!hit)
                return;
            MeshCrossing c;
            c.a = static_cast<std::uint32_t>(i);
            c.b = j;
            c.from = hit->from.toVec3();
            c.to = hit->to.toVec3();
            c.coplanar = hit->coplanar;
            ++result.pairs;
            result.longest = std::max(result.longest, c.length());
            if (result.examples.size() < maxExamples)
                result.examples.push_back(c);
        });
    }
    return result;
}

} // namespace

SelfIntersections findSelfIntersections(const TriangleMesh& input, double minLength, std::size_t maxExamples,
                                        const CancelCheck& cancelled)
{
    return findInWelded(weldVertices(input), minLength, maxExamples, cancelled);
}

namespace {

Vec3 roundedToFloat(const Vec3& v)
{
    return {static_cast<double>(static_cast<float>(v.x)), static_cast<double>(static_cast<float>(v.y)),
            static_cast<double>(static_cast<float>(v.z))};
}

/// A way to lift one crossing: the end of an edge of one triangle that cuts through the
/// other triangle, moved along the other triangle's normal to the side of the edge's other
/// end.
struct Nudge {
    std::uint32_t vertex = 0;
    Vec3 to{};
    double cost = 0.0;
};

void nudgesFor(const Tris& tris, const std::vector<LV>& lv, std::uint32_t ia, std::uint32_t ib, double margin,
               std::vector<Nudge>& out)
{
    const auto& ta = tris[ia];
    const auto& tb = tris[ib];
    const Tri T{lv[tb[0]], lv[tb[1]], lv[tb[2]]};
    LV n = (T[1] - T[0]).cross(T[2] - T[0]);
    const LD len = std::sqrt(n.dot(n));
    if (len == 0)
        return;
    n = n * (1 / len);
    for (std::size_t k = 0; k < 3; ++k) {
        const std::uint32_t p = ta[k], q = ta[(k + 1) % 3];
        if (p == tb[0] || p == tb[1] || p == tb[2] || q == tb[0] || q == tb[1] || q == tb[2])
            continue;
        LV at;
        if (!pierce(lv[p], lv[q], T, false, at))
            continue;
        for (int end = 0; end < 2; ++end) {
            const std::uint32_t e = end == 0 ? p : q, o = end == 0 ? q : p;
            const LD de = n.dot(lv[e] - T[0]), dOther = n.dot(lv[o] - T[0]);
            if (dOther == 0)
                continue;
            const LD target = (dOther > 0 ? 1 : -1) * static_cast<LD>(margin);
            const LD move = de - target;
            out.push_back({e, (lv[e] - n * move).toVec3(), static_cast<double>(std::abs(move))});
        }
    }
}

} // namespace

IntersectionRepair untangleSelfIntersections(const TriangleMesh& input, double maxMove, bool roundToFloat,
                                             const CancelCheck& cancelled)
{
    IntersectionRepair out;
    out.mesh = input;
    if (!input.isValid() || input.isEmpty())
        return out;
    // Round first, then merge: vertices that fall together as float are one in the file.
    std::vector<Vec3> rounded = input.vertices();
    if (roundToFloat)
        for (auto& v : rounded)
            v = roundedToFloat(v);
    TriangleMesh welded = weldVertices(TriangleMesh(std::move(rounded), input.triangles()));
    if (welded.isEmpty())
        return out;
    std::vector<Vec3> pos = welded.vertices();
    const std::vector<Vec3> original = pos;
    const Tris tris = welded.triangles();
    const double extent = welded.bounds().diagonal();
    // Far enough to keep the side after the vertex is rounded to float again.
    const double margin = std::max(2e-6 * extent, 1e-6);

    std::vector<LV> lv = toLong(pos);
    std::vector<Box> boxes(tris.size());
    // A moved triangle stays inside its original box grown by the largest move.
    const double reach = maxMove + 2.0 * margin;
    for (std::size_t i = 0; i < tris.size(); ++i)
        boxes[i] = boxOf(tris, lv, i, reach);
    const Bvh bvh(boxes);
    std::vector<std::vector<std::uint32_t>> around(pos.size());
    for (std::size_t i = 0; i < tris.size(); ++i)
        for (const auto v : tris[i])
            around[v].push_back(static_cast<std::uint32_t>(i));

    // Crossings of the triangles around vertex v, with everything.
    auto crossingsAround = [&](std::uint32_t v) {
        std::size_t count = 0;
        for (const std::uint32_t t : around[v]) {
            const Box box = boxOf(tris, lv, t, 0.0);
            bvh.query(box, [&](std::uint32_t j) {
                if (j != t && testPair(tris, lv, t, j, 0.0))
                    ++count;
            });
        }
        return count;
    };

    auto current = [&] {
        out.mesh = TriangleMesh(pos, tris);
        out.remaining = findInWelded(out.mesh, 0.0, std::numeric_limits<std::size_t>::max(), cancelled);
    };
    for (int pass = 0; pass < 40; ++pass) {
        throwIfCancelled(cancelled);
        current();
        if (!out.remaining.any())
            break;
        bool progress = false;
        for (const auto& c : out.remaining.examples) {
            throwIfCancelled(cancelled);
            if (c.coplanar || !testPair(tris, lv, c.a, c.b, 0.0))
                continue; // not a crossing any more after an earlier move of this pass
            std::vector<Nudge> options;
            nudgesFor(tris, lv, c.a, c.b, margin, options);
            nudgesFor(tris, lv, c.b, c.a, margin, options);
            std::sort(options.begin(), options.end(), [](const Nudge& x, const Nudge& y) { return x.cost < y.cost; });
            for (const Nudge& nudge : options) {
                const Vec3 to = roundToFloat ? roundedToFloat(nudge.to) : nudge.to;
                const Vec3 d = to - original[nudge.vertex];
                if (std::sqrt(d.dot(d)) > maxMove)
                    continue;
                // Keep a move only when fewer triangles cross afterwards.
                const std::size_t before = crossingsAround(nudge.vertex);
                const LV oldLv = lv[nudge.vertex];
                lv[nudge.vertex] = LV(to);
                if (crossingsAround(nudge.vertex) < before) {
                    pos[nudge.vertex] = to;
                    progress = true;
                    break;
                }
                lv[nudge.vertex] = oldLv;
            }
        }
        if (!progress)
            break;
    }
    current();
    out.moved = 0;
    for (std::size_t i = 0; i < pos.size(); ++i)
        if (pos[i] != original[i]) {
            ++out.moved;
            const Vec3 d = pos[i] - original[i];
            out.largestMove = std::max(out.largestMove, std::sqrt(d.dot(d)));
        }
    return out;
}

} // namespace aerocad::core
