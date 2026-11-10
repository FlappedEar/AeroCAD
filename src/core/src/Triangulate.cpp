// SPDX-License-Identifier: GPL-3.0-or-later
// Ear clipping with hole bridging, after the earcut algorithm (mapbox/earcut, ISC licence)
// without its z-order acceleration: section loops are simplified first, so they have a few
// thousand points at most.
#include "aerocad/core/Triangulate.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aerocad::core {

namespace {

struct Node {
    std::uint32_t i;
    double x;
    double y;
    int prev = -1;
    int next = -1;
    bool steiner = false;
};

class Earcut {
public:
    std::vector<Node> nodes;
    std::vector<std::uint32_t> out;

    std::vector<std::uint32_t> run(const std::vector<std::vector<Point2>>& rings)
    {
        std::size_t total = 0;
        for (const auto& r : rings)
            total += r.size();
        nodes.reserve(total * 2 + 16);
        std::vector<Point2> data;
        data.reserve(total);
        std::vector<std::size_t> starts;
        for (const auto& r : rings) {
            starts.push_back(data.size());
            data.insert(data.end(), r.begin(), r.end());
        }
        starts.push_back(data.size());
        if (rings.empty() || rings[0].size() < 3)
            return {};

        int outer = linkedList(data, starts[0], starts[1], true);
        if (outer < 0 || nodes[static_cast<std::size_t>(outer)].next == nodes[static_cast<std::size_t>(outer)].prev)
            return {};
        if (rings.size() > 1)
            outer = eliminateHoles(data, starts, outer);
        earcutLinked(outer, 0);
        return std::move(out);
    }

private:
    Node& n(int i) { return nodes[static_cast<std::size_t>(i)]; }

    static double signedArea(const std::vector<Point2>& d, std::size_t start, std::size_t end)
    {
        double sum = 0.0;
        for (std::size_t i = start, j = end - 1; i < end; j = i, ++i)
            sum += (d[j][0] - d[i][0]) * (d[i][1] + d[j][1]);
        return sum;
    }

    int insertNode(std::uint32_t i, double x, double y, int last)
    {
        const int p = static_cast<int>(nodes.size());
        nodes.push_back(Node{i, x, y});
        if (last < 0) {
            n(p).prev = p;
            n(p).next = p;
        } else {
            n(p).next = n(last).next;
            n(p).prev = last;
            n(n(last).next).prev = p;
            n(last).next = p;
        }
        return p;
    }

    void removeNode(int p)
    {
        n(n(p).next).prev = n(p).prev;
        n(n(p).prev).next = n(p).next;
    }

    int linkedList(const std::vector<Point2>& d, std::size_t start, std::size_t end, bool clockwise)
    {
        int last = -1;
        if (clockwise == (signedArea(d, start, end) > 0)) {
            for (std::size_t i = start; i < end; ++i)
                last = insertNode(static_cast<std::uint32_t>(i), d[i][0], d[i][1], last);
        } else {
            for (std::size_t i = end; i-- > start;)
                last = insertNode(static_cast<std::uint32_t>(i), d[i][0], d[i][1], last);
        }
        if (last >= 0 && equals(last, n(last).next)) {
            const int next = n(last).next;
            removeNode(last);
            last = next;
        }
        return last;
    }

    int filterPoints(int start, int end = -1)
    {
        if (start < 0)
            return start;
        if (end < 0)
            end = start;
        int p = start;
        bool again;
        do {
            again = false;
            if (!n(p).steiner && (equals(p, n(p).next) || area(n(p).prev, p, n(p).next) == 0.0)) {
                removeNode(p);
                p = end = n(p).prev;
                if (p == n(p).next)
                    break;
                again = true;
            } else {
                p = n(p).next;
            }
        } while (again || p != end);
        return end;
    }

    void earcutLinked(int ear, int pass)
    {
        if (ear < 0)
            return;
        int stop = ear;
        while (n(ear).prev != n(ear).next) {
            const int prev = n(ear).prev;
            const int next = n(ear).next;
            if (isEar(ear)) {
                out.push_back(n(prev).i);
                out.push_back(n(ear).i);
                out.push_back(n(next).i);
                removeNode(ear);
                ear = n(next).next;
                stop = n(next).next;
                continue;
            }
            ear = next;
            if (ear == stop) {
                if (pass == 0) {
                    earcutLinked(filterPoints(ear), 1);
                } else if (pass == 1) {
                    ear = cureLocalIntersections(filterPoints(ear));
                    earcutLinked(ear, 2);
                } else if (pass == 2) {
                    splitEarcut(ear);
                }
                break;
            }
        }
    }

    bool isEar(int ear)
    {
        const int a = n(ear).prev;
        const int c = n(ear).next;
        if (area(a, ear, c) >= 0.0)
            return false; // reflex
        const double ax = n(a).x, ay = n(a).y, bx = n(ear).x, by = n(ear).y, cx = n(c).x, cy = n(c).y;
        const double x0 = std::min({ax, bx, cx}), y0 = std::min({ay, by, cy});
        const double x1 = std::max({ax, bx, cx}), y1 = std::max({ay, by, cy});
        int p = n(c).next;
        while (p != a) {
            const Node& q = n(p);
            if (q.x >= x0 && q.x <= x1 && q.y >= y0 && q.y <= y1 &&
                pointInTriangle(ax, ay, bx, by, cx, cy, q.x, q.y) && area(q.prev, p, q.next) >= 0.0)
                return false;
            p = q.next;
        }
        return true;
    }

    int cureLocalIntersections(int start)
    {
        int p = start;
        do {
            const int a = n(p).prev;
            const int b = n(n(n(p).next).next).next == -1 ? -1 : n(n(p).next).next;
            if (a != b && !equals(a, b) && intersects(a, p, n(p).next, b) && locallyInside(a, b) &&
                locallyInside(b, a)) {
                out.push_back(n(a).i);
                out.push_back(n(p).i);
                out.push_back(n(b).i);
                removeNode(p);
                removeNode(n(p).next);
                p = start = b;
            }
            p = n(p).next;
        } while (p != start);
        return filterPoints(p);
    }

    void splitEarcut(int start)
    {
        int a = start;
        do {
            int b = n(n(a).next).next;
            while (b != n(a).prev) {
                if (n(a).i != n(b).i && isValidDiagonal(a, b)) {
                    int c = splitPolygon(a, b);
                    a = filterPoints(a, n(a).next);
                    c = filterPoints(c, n(c).next);
                    earcutLinked(a, 0);
                    earcutLinked(c, 0);
                    return;
                }
                b = n(b).next;
            }
            a = n(a).next;
        } while (a != start);
    }

    int eliminateHoles(const std::vector<Point2>& d, const std::vector<std::size_t>& starts, int outer)
    {
        std::vector<int> queue;
        for (std::size_t h = 1; h + 1 < starts.size(); ++h) {
            if (starts[h + 1] - starts[h] < 3)
                continue;
            const int list = linkedList(d, starts[h], starts[h + 1], false);
            if (list < 0)
                continue;
            if (list == n(list).next)
                n(list).steiner = true;
            queue.push_back(getLeftmost(list));
        }
        std::sort(queue.begin(), queue.end(), [this](int a, int b) { return n(a).x < n(b).x; });
        for (const int hole : queue) {
            outer = eliminateHole(hole, outer);
            outer = filterPoints(outer, n(outer).next);
        }
        return outer;
    }

    int eliminateHole(int hole, int outer)
    {
        const int bridge = findHoleBridge(hole, outer);
        if (bridge < 0)
            return outer;
        const int reverse = splitPolygon(bridge, hole);
        filterPoints(reverse, n(reverse).next);
        return filterPoints(bridge, n(bridge).next);
    }

    int findHoleBridge(int hole, int outer)
    {
        int p = outer;
        const double hx = n(hole).x, hy = n(hole).y;
        double qx = -std::numeric_limits<double>::infinity();
        int m = -1;
        do {
            if (equals(hole, p))
                return p;
            const int nx = n(p).next;
            if (hy <= n(p).y && hy >= n(nx).y && n(nx).y != n(p).y) {
                const double x = n(p).x + (hy - n(p).y) * (n(nx).x - n(p).x) / (n(nx).y - n(p).y);
                if (x <= hx && x > qx) {
                    qx = x;
                    m = n(p).x < n(nx).x ? p : nx;
                    if (x == hx)
                        return m;
                }
            }
            p = nx;
        } while (p != outer);
        if (m < 0)
            return -1;
        const int stop = m;
        const double mx = n(m).x, my = n(m).y;
        double tanMin = std::numeric_limits<double>::infinity();
        p = m;
        do {
            if (hx >= n(p).x && n(p).x >= mx && hx != n(p).x &&
                pointInTriangle(hy < my ? hx : qx, hy, mx, my, hy < my ? qx : hx, hy, n(p).x, n(p).y)) {
                const double t = std::abs(hy - n(p).y) / (hx - n(p).x);
                if (locallyInside(p, hole) &&
                    (t < tanMin ||
                     (t == tanMin && (n(p).x > n(m).x || (n(p).x == n(m).x && sectorContainsSector(m, p)))))) {
                    m = p;
                    tanMin = t;
                }
            }
            p = n(p).next;
        } while (p != stop);
        return m;
    }

    bool sectorContainsSector(int m, int p) { return area(n(m).prev, m, n(p).prev) < 0 && area(n(p).next, m, n(m).next) < 0; }

    int getLeftmost(int start)
    {
        int p = start;
        int leftmost = start;
        do {
            if (n(p).x < n(leftmost).x || (n(p).x == n(leftmost).x && n(p).y < n(leftmost).y))
                leftmost = p;
            p = n(p).next;
        } while (p != start);
        return leftmost;
    }

    static bool pointInTriangle(double ax, double ay, double bx, double by, double cx, double cy, double px, double py)
    {
        return (cx - px) * (ay - py) >= (ax - px) * (cy - py) && (ax - px) * (by - py) >= (bx - px) * (ay - py) &&
               (bx - px) * (cy - py) >= (cx - px) * (by - py);
    }

    bool isValidDiagonal(int a, int b)
    {
        return n(n(a).next).i != n(b).i && n(n(a).prev).i != n(b).i && !intersectsPolygon(a, b) &&
               ((locallyInside(a, b) && locallyInside(b, a) && middleInside(a, b) &&
                 (area(n(a).prev, a, n(b).prev) != 0.0 || area(a, n(b).prev, b) != 0.0)) ||
                (equals(a, b) && area(n(a).prev, a, n(a).next) > 0 && area(n(b).prev, b, n(b).next) > 0));
    }

    double area(int p, int q, int r)
    {
        return (n(q).y - n(p).y) * (n(r).x - n(q).x) - (n(q).x - n(p).x) * (n(r).y - n(q).y);
    }

    bool equals(int a, int b) { return n(a).x == n(b).x && n(a).y == n(b).y; }

    static int sign(double v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }

    bool intersects(int p1, int q1, int p2, int q2)
    {
        const int o1 = sign(area(p1, q1, p2));
        const int o2 = sign(area(p1, q1, q2));
        const int o3 = sign(area(p2, q2, p1));
        const int o4 = sign(area(p2, q2, q1));
        if (o1 != o2 && o3 != o4)
            return true;
        if (o1 == 0 && onSegment(p1, p2, q1))
            return true;
        if (o2 == 0 && onSegment(p1, q2, q1))
            return true;
        if (o3 == 0 && onSegment(p2, p1, q2))
            return true;
        if (o4 == 0 && onSegment(p2, q1, q2))
            return true;
        return false;
    }

    bool onSegment(int p, int q, int r)
    {
        return n(q).x <= std::max(n(p).x, n(r).x) && n(q).x >= std::min(n(p).x, n(r).x) &&
               n(q).y <= std::max(n(p).y, n(r).y) && n(q).y >= std::min(n(p).y, n(r).y);
    }

    bool intersectsPolygon(int a, int b)
    {
        int p = a;
        do {
            const int nx = n(p).next;
            if (n(p).i != n(a).i && n(nx).i != n(a).i && n(p).i != n(b).i && n(nx).i != n(b).i && intersects(p, nx, a, b))
                return true;
            p = nx;
        } while (p != a);
        return false;
    }

    bool locallyInside(int a, int b)
    {
        return area(n(a).prev, a, n(a).next) < 0 ? area(a, b, n(a).next) >= 0 && area(a, n(a).prev, b) >= 0
                                                  : area(a, b, n(a).prev) < 0 || area(a, n(a).next, b) < 0;
    }

    bool middleInside(int a, int b)
    {
        int p = a;
        bool inside = false;
        const double px = (n(a).x + n(b).x) / 2;
        const double py = (n(a).y + n(b).y) / 2;
        do {
            const int nx = n(p).next;
            if (((n(p).y > py) != (n(nx).y > py)) && n(nx).y != n(p).y &&
                (px < (n(nx).x - n(p).x) * (py - n(p).y) / (n(nx).y - n(p).y) + n(p).x))
                inside = !inside;
            p = nx;
        } while (p != a);
        return inside;
    }

    int splitPolygon(int a, int b)
    {
        const int a2 = static_cast<int>(nodes.size());
        nodes.push_back(Node{n(a).i, n(a).x, n(a).y});
        const int b2 = static_cast<int>(nodes.size());
        nodes.push_back(Node{n(b).i, n(b).x, n(b).y});
        const int an = n(a).next;
        const int bp = n(b).prev;
        n(a).next = b;
        n(b).prev = a;
        n(a2).next = an;
        n(an).prev = a2;
        n(b2).next = a2;
        n(a2).prev = b2;
        n(bp).next = b2;
        n(b2).prev = bp;
        return b2;
    }
};

double pointSegmentDistance(const Point2& p, const Point2& a, const Point2& b)
{
    const double dx = b[0] - a[0];
    const double dy = b[1] - a[1];
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 0.0 ? ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double ex = a[0] + t * dx - p[0];
    const double ey = a[1] + t * dy - p[1];
    return std::sqrt(ex * ex + ey * ey);
}

} // namespace

std::vector<std::uint32_t> triangulatePolygon(const std::vector<std::vector<Point2>>& rings)
{
    return Earcut{}.run(rings);
}

std::vector<Point2> simplifyRing(const std::vector<Point2>& ring, double tolerance)
{
    if (ring.size() <= 3 || tolerance <= 0.0)
        return ring;
    // Anchor the two ends of the ring at the farthest pair found from point 0, then simplify
    // both halves as open chains.
    std::size_t far = 0;
    double best = -1.0;
    for (std::size_t i = 1; i < ring.size(); ++i) {
        const double dx = ring[i][0] - ring[0][0];
        const double dy = ring[i][1] - ring[0][1];
        const double d = dx * dx + dy * dy;
        if (d > best) {
            best = d;
            far = i;
        }
    }
    std::vector<bool> keep(ring.size(), false);
    keep[0] = true;
    keep[far] = true;
    auto chain = [&](std::size_t from, std::size_t to) {
        // `to` may be ring.size() (the closing point, index 0).
        std::vector<std::pair<std::size_t, std::size_t>> stack{{from, to}};
        while (!stack.empty()) {
            const auto [a, b] = stack.back();
            stack.pop_back();
            if (b <= a + 1)
                continue;
            const Point2& pa = ring[a % ring.size()];
            const Point2& pb = ring[b % ring.size()];
            double worst = tolerance;
            std::size_t at = 0;
            for (std::size_t i = a + 1; i < b; ++i) {
                const double d = pointSegmentDistance(ring[i], pa, pb);
                if (d > worst) {
                    worst = d;
                    at = i;
                }
            }
            if (at != 0) {
                keep[at] = true;
                stack.emplace_back(a, at);
                stack.emplace_back(at, b);
            }
        }
    };
    chain(0, far);
    chain(far, ring.size());
    std::vector<Point2> out;
    for (std::size_t i = 0; i < ring.size(); ++i)
        if (keep[i])
            out.push_back(ring[i]);
    return out.size() >= 3 ? out : ring;
}

} // namespace aerocad::core
