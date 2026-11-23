#pragma once

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "ProgramSearch.h"

namespace infocell {
namespace arc {
namespace search {

inline int background(const G& g)
{
    std::map<int, int> hist;
    for (int c : g.p) {
        ++hist[c];
    }
    return std::max_element(hist.begin(), hist.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
}

inline std::map<int, int> histogram(const G& g)
{
    std::map<int, int> hist;
    for (int c : g.p) {
        ++hist[c];
    }
    return hist;
}

inline G dihedral(const G& in, int k)
{
    const bool swap = (k == 1 || k == 3 || k == 6 || k == 7);
    G out(swap ? in.h : in.w, swap ? in.w : in.h);
    for (int y = 0; y < out.h; ++y) {
        for (int x = 0; x < out.w; ++x) {
            int sx = x, sy = y;
            switch (k) {
            case 1: sx = y;            sy = in.h - 1 - x; break;
            case 2: sx = in.w - 1 - x; sy = in.h - 1 - y; break;
            case 3: sx = in.w - 1 - y; sy = x;            break;
            case 4: sx = in.w - 1 - x; sy = y;            break;
            case 5: sx = x;            sy = in.h - 1 - y; break;
            case 6: sx = y;            sy = x;            break;
            case 7: sx = in.w - 1 - y; sy = in.h - 1 - x; break;
            default: break;
            }
            out.at(x, y) = in.at(sx, sy);
        }
    }
    return out;
}

inline const char* dihedralName(int k)
{
    static const char* names[] = { "identity", "rotate90", "rotate180", "rotate270", "mirrorH", "mirrorV", "transpose", "antiTranspose" };
    return names[k];
}

struct Box
{
    int x0, y0, x1, y1;
    int w() const { return x1 - x0 + 1; }
    int h() const { return y1 - y0 + 1; }
};

struct Object
{
    std::vector<std::pair<int, int>> pixels;
    std::set<int> colors;
    Box box { 1 << 20, 1 << 20, -1, -1 };
};

inline std::vector<Object> objects(const G& g, int bg, bool sameColor, bool diagonal)
{
    std::vector<Object> ret;
    std::vector<bool> seen(g.p.size(), false);
    for (int y = 0; y < g.h; ++y) {
        for (int x = 0; x < g.w; ++x) {
            if (seen[y * g.w + x] || g.at(x, y) == bg) {
                continue;
            }
            Object obj;
            std::vector<std::pair<int, int>> stack { { x, y } };
            seen[y * g.w + x] = true;
            while (!stack.empty()) {
                auto [cx, cy] = stack.back();
                stack.pop_back();
                obj.pixels.push_back({ cx, cy });
                obj.colors.insert(g.at(cx, cy));
                obj.box = { std::min(obj.box.x0, cx), std::min(obj.box.y0, cy), std::max(obj.box.x1, cx), std::max(obj.box.y1, cy) };
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if ((dx == 0 && dy == 0) || (!diagonal && dx != 0 && dy != 0)) {
                            continue;
                        }
                        const int nx = cx + dx, ny = cy + dy;
                        if (!g.inRange(nx, ny) || seen[ny * g.w + nx] || g.at(nx, ny) == bg) {
                            continue;
                        }
                        if (sameColor && g.at(nx, ny) != g.at(cx, cy)) {
                            continue;
                        }
                        seen[ny * g.w + nx] = true;
                        stack.push_back({ nx, ny });
                    }
                }
            }
            ret.push_back(std::move(obj));
        }
    }
    return ret;
}

inline G crop(const G& g, const Box& b)
{
    G out(b.w(), b.h());
    for (int y = 0; y < out.h; ++y) {
        for (int x = 0; x < out.w; ++x) {
            out.at(x, y) = g.at(b.x0 + x, b.y0 + y);
        }
    }
    return out;
}

inline std::vector<int> enclosedMask(const G& g, int bg)
{
    std::vector<int> outside(g.p.size(), 0);
    std::vector<std::pair<int, int>> stack;
    for (int y = 0; y < g.h; ++y) {
        for (int x = 0; x < g.w; ++x) {
            if ((x == 0 || y == 0 || x == g.w - 1 || y == g.h - 1) && g.at(x, y) == bg) {
                outside[y * g.w + x] = 1;
                stack.push_back({ x, y });
            }
        }
    }
    while (!stack.empty()) {
        auto [x, y] = stack.back();
        stack.pop_back();
        const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (auto& dd : d) {
            const int nx = x + dd[0], ny = y + dd[1];
            if (g.inRange(nx, ny) && !outside[ny * g.w + nx] && g.at(nx, ny) == bg) {
                outside[ny * g.w + nx] = 1;
                stack.push_back({ nx, ny });
            }
        }
    }
    std::vector<int> ret(g.p.size(), 0);
    for (size_t i = 0; i < g.p.size(); ++i) {
        ret[i] = (g.p[i] == bg && !outside[i]) ? 1 : 0;
    }
    return ret;
}

inline int ray(const G& g, int bg, int x, int y, int dx, int dy)
{
    for (x += dx, y += dy; g.inRange(x, y); x += dx, y += dy) {
        if (g.at(x, y) != bg) {
            return g.at(x, y);
        }
    }
    return -1;
}

inline int rayDist(const G& g, int bg, int x, int y, int dx, int dy)
{
    int d = 0;
    for (x += dx, y += dy; g.inRange(x, y); x += dx, y += dy) {
        ++d;
        if (g.at(x, y) != bg) {
            return d;
        }
    }
    return -1;
}

inline bool sameSizes(const std::vector<Pair>& train)
{
    return std::all_of(train.begin(), train.end(), [](const Pair& p) { return p.in.w == p.out.w && p.in.h == p.out.h; });
}

} // namespace search
} // namespace arc
} // namespace infocell
