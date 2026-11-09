// SPDX-License-Identifier: GPL-3.0-or-later
// Writes a dense binary STL for the performance tests: a wavy 1000 x 600 mm sheet with a
// bump, like a car body scan, of about <triangles> triangles (default 1.5 million).
//   make_dense_mesh <out.stl> [triangles]
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct P {
    float x, y, z;
};

P surface(double u, double v)
{
    const double x = 1000.0 * u;
    const double y = 600.0 * v;
    const double bump = 150.0 * std::exp(-((u - 0.5) * (u - 0.5) + (v - 0.5) * (v - 0.5)) * 12.0);
    const double z = bump + 4.0 * std::sin(x * 0.05) * std::cos(y * 0.04);
    return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
}

void put(std::vector<unsigned char>& out, const void* data, std::size_t n)
{
    const auto* b = static_cast<const unsigned char*>(data);
    out.insert(out.end(), b, b + n);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: make_dense_mesh <out.stl> [triangles]\n");
        return 2;
    }
    const double wanted = argc > 2 ? std::atof(argv[2]) : 1.5e6;
    const int nu = std::max(2, static_cast<int>(std::sqrt(wanted / 2.0 * 1000.0 / 600.0)));
    const int nv = std::max(2, static_cast<int>(wanted / 2.0 / nu));
    const std::uint32_t count = static_cast<std::uint32_t>(2) * static_cast<std::uint32_t>(nu * nv);

    std::vector<unsigned char> out;
    out.reserve(84 + static_cast<std::size_t>(count) * 50);
    char header[80] = {};
    std::strncpy(header, "AeroCAD dense performance test mesh", sizeof header - 1);
    put(out, header, 80);
    put(out, &count, 4);
    const float normal[3] = {0, 0, 0};
    const std::uint16_t attr = 0;
    auto tri = [&](const P& a, const P& b, const P& c) {
        put(out, normal, 12);
        put(out, &a, 12);
        put(out, &b, 12);
        put(out, &c, 12);
        put(out, &attr, 2);
    };
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) {
            const P a = surface(double(i) / nu, double(j) / nv);
            const P b = surface(double(i + 1) / nu, double(j) / nv);
            const P c = surface(double(i + 1) / nu, double(j + 1) / nv);
            const P d = surface(double(i) / nu, double(j + 1) / nv);
            tri(a, b, c);
            tri(a, c, d);
        }
    std::FILE* f = std::fopen(argv[1], "wb");
    if (f == nullptr || std::fwrite(out.data(), 1, out.size(), f) != out.size()) {
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    std::fclose(f);
    std::printf("%u triangles\n", count);
    return 0;
}
