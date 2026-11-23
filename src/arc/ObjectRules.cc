#include "ObjectRules.h"

#include <array>
#include <cmath>
#include <bit>
#include <tuple>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "Perception.h"

namespace infocell {
namespace arc {
namespace search {

namespace {

struct Obj
{
    std::vector<std::pair<int, int>> px;
    std::vector<int> col;
    Box box;
    int main = 0;
};

struct ParseDef
{
    const char* name;
    bool sameColor;
    bool diagonal;
    int kind = 0;
};

const ParseDef kParses[] = {
    { "single color", true, false },
    { "single color, diagonal", true, true },
    { "multi color", false, true },
    { "by color", true, true, 1 },
    { "background regions", true, false, 2 },
    { "rows", false, false, 3 },
    { "columns", false, false, 4 },
};

std::vector<Object> backgroundRegions(const G& g, int bg)
{
    std::vector<Object> ret;
    std::vector<char> seen(g.p.size(), 0);
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x) {
            if (seen[y * g.w + x] || g.at(x, y) != bg) continue;
            Object o;
            std::vector<std::pair<int, int>> stack { { x, y } };
            seen[y * g.w + x] = 1;
            while (!stack.empty()) {
                auto [cx, cy] = stack.back();
                stack.pop_back();
                o.pixels.push_back({ cx, cy });
                o.colors.insert(bg);
                o.box = { std::min(o.box.x0, cx), std::min(o.box.y0, cy), std::max(o.box.x1, cx), std::max(o.box.y1, cy) };
                const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                for (auto& dd : d) {
                    const int nx = cx + dd[0], ny = cy + dd[1];
                    if (g.inRange(nx, ny) && !seen[ny * g.w + nx] && g.at(nx, ny) == bg) {
                        seen[ny * g.w + nx] = 1;
                        stack.push_back({ nx, ny });
                    }
                }
            }
            ret.push_back(std::move(o));
        }
    return ret;
}

std::vector<Object> lines(const G& g, bool columns)
{
    std::vector<Object> ret;
    const int n = columns ? g.w : g.h, len = columns ? g.h : g.w;
    for (int i = 0; i < n; ++i) {
        Object o;
        for (int j = 0; j < len; ++j) {
            const int x = columns ? i : j, y = columns ? j : i;
            o.pixels.push_back({ x, y });
            o.colors.insert(g.at(x, y));
        }
        o.box = columns ? Box { i, 0, i, g.h - 1 } : Box { 0, i, g.w - 1, i };
        ret.push_back(std::move(o));
    }
    return ret;
}

std::vector<Object> colorGroups(const G& g, int bg)
{
    std::map<int, Object> groups;
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x) {
            if (g.at(x, y) == bg) continue;
            Object& o = groups[g.at(x, y)];
            o.pixels.push_back({ x, y });
            o.colors.insert(g.at(x, y));
            o.box = { std::min(o.box.x0, x), std::min(o.box.y0, y), std::max(o.box.x1, x), std::max(o.box.y1, y) };
        }
    std::vector<Object> ret;
    for (auto& [c, o] : groups) ret.push_back(std::move(o));
    return ret;
}

std::vector<Obj> parseObjects(const G& g, int bg, const ParseDef& parse)
{
    std::vector<Obj> ret;
    std::vector<Object> found;
    switch (parse.kind) {
    case 1: found = colorGroups(g, bg); break;
    case 2: found = backgroundRegions(g, bg); break;
    case 3: found = lines(g, false); break;
    case 4: found = lines(g, true); break;
    default: found = objects(g, bg, parse.sameColor, parse.diagonal); break;
    }
    for (auto& o : found) {
        Obj r;
        r.px  = o.pixels;
        r.box = o.box;
        std::map<int, int> h;
        for (auto [x, y] : o.pixels) {
            r.col.push_back(g.at(x, y));
            ++h[g.at(x, y)];
        }
        r.main = std::max_element(h.begin(), h.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
        ret.push_back(std::move(r));
    }
    return ret;
}

std::vector<int> shapeOf(const Obj& o)
{
    std::vector<int> k { o.box.w(), o.box.h() };
    std::vector<int> mask(o.box.w() * o.box.h(), 0);
    for (auto [x, y] : o.px) {
        mask[(y - o.box.y0) * o.box.w() + (x - o.box.x0)] = 1;
    }
    k.insert(k.end(), mask.begin(), mask.end());
    return k;
}

int shapeHash(const std::vector<int>& shape)
{
    uint32_t h = 2166136261u;
    for (int v : shape) h = (h ^ static_cast<uint32_t>(v + 7)) * 16777619u;
    return static_cast<int>(h & 0x3FFFFFFF);
}

enum Atom
{
    AColor,
    ASize,
    AWidth,
    AHeight,
    ALeft,
    ATop,
    ARight,
    ABottom,
    AHoles,
    AColors,
    ABorder,
    ARect,
    ASymmetric,
    AShape,
    AtomCount
};
const char* kAtomName[AtomCount] = { "color", "size", "width", "height", "left", "top", "right", "bottom", "holes", "colors", "touches border", "is rectangle", "is symmetric", "shape" };

enum Rel
{
    RNearest,
    RLargest,
    RSmallest,
    RTouching,
    RContainer,
    RelCount
};
const char* kRelName[RelCount] = { "nearest", "largest", "smallest", "touching", "container" };

enum Form
{
    FSelf,
    FRel,
    FEqual,
    FRankDown,
    FRankUp,
    FSameCount,
    FObjects,
    FColors,
    FormCount
};

struct Formula
{
    int form;
    int atom;
    int rel;
    std::string name;
    double bits;
};

const std::vector<Formula>& grammar()
{
    static const std::vector<Formula> g = [] {
        std::vector<Formula> r;
        const double form = std::log2(double(FormCount)), atom = std::log2(double(AtomCount)), rel = std::log2(double(RelCount));
        for (int a = 0; a < AtomCount; ++a) r.push_back({ FSelf, a, -1, kAtomName[a], form + atom });
        for (int a = 0; a < AtomCount; ++a) r.push_back({ FRankDown, a, -1, std::string("rank of ") + kAtomName[a] + " (largest first)", form + atom });
        for (int a = 0; a < AtomCount; ++a) r.push_back({ FRankUp, a, -1, std::string("rank of ") + kAtomName[a] + " (smallest first)", form + atom });
        for (int a = 0; a < AtomCount; ++a) r.push_back({ FSameCount, a, -1, std::string("objects with the same ") + kAtomName[a], form + atom });
        r.push_back({ FObjects, -1, -1, "number of objects", form });
        r.push_back({ FColors, -1, -1, "number of colors", form });
        for (int a = 0; a < AtomCount; ++a)
            for (int q = 0; q < RelCount; ++q) r.push_back({ FRel, a, q, std::string(kAtomName[a]) + "(" + kRelName[q] + ")", form + atom + rel });
        for (int a = 0; a < AtomCount; ++a)
            for (int q = 0; q < RelCount; ++q) r.push_back({ FEqual, a, q, std::string(kAtomName[a]) + " == " + kAtomName[a] + "(" + kRelName[q] + ")", form + atom + rel });
        std::stable_sort(r.begin(), r.end(), [](const Formula& x, const Formula& y) { return x.bits < y.bits; });
        return r;
    }();
    return g;
}

const std::string& featureName(int f) { return grammar()[f].name; }

std::vector<double>& formulaBits()
{
    static std::vector<double> bits = [] {
        std::vector<double> b;
        for (const Formula& f : grammar()) b.push_back(f.bits);
        return b;
    }();
    return bits;
}

std::vector<int>& formulaOrderStorage()
{
    static std::vector<int> order = [] {
        std::vector<int> o(grammar().size());
        for (size_t i = 0; i < o.size(); ++i) o[i] = static_cast<int>(i);
        return o;
    }();
    return order;
}

const std::vector<int>& formulaOrder() { return formulaOrderStorage(); }

std::array<int, AtomCount> atomsOf(const G& g, int bg, const Obj& o)
{
    std::array<int, AtomCount> a {};
    G local(o.box.w(), o.box.h(), 0);
    for (auto [x, y] : o.px) local.at(x - o.box.x0, y - o.box.y0) = 1;
    const auto holes = enclosedMask(local, 0);
    int holeCount    = 0;
    {
        std::vector<int> seen(holes.size(), 0);
        for (size_t start = 0; start < holes.size(); ++start) {
            if (!holes[start] || seen[start]) continue;
            ++holeCount;
            std::vector<size_t> stack { start };
            seen[start] = 1;
            while (!stack.empty()) {
                const size_t j = stack.back();
                stack.pop_back();
                const int jx = static_cast<int>(j) % local.w, jy = static_cast<int>(j) / local.w;
                const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                for (auto& dd : d) {
                    const int nx = jx + dd[0], ny = jy + dd[1];
                    if (!local.inRange(nx, ny)) continue;
                    const size_t k = static_cast<size_t>(ny * local.w + nx);
                    if (holes[k] && !seen[k]) {
                        seen[k] = 1;
                        stack.push_back(k);
                    }
                }
            }
        }
    }
    G box(o.box.w(), o.box.h(), -1);
    for (size_t i = 0; i < o.px.size(); ++i) box.at(o.px[i].first - o.box.x0, o.px[i].second - o.box.y0) = o.col[i];
    a[AColor]     = o.main;
    a[ASize]      = static_cast<int>(o.px.size());
    a[AWidth]     = o.box.w();
    a[AHeight]    = o.box.h();
    a[ALeft]      = o.box.x0;
    a[ATop]       = o.box.y0;
    a[ARight]     = g.w - 1 - o.box.x1;
    a[ABottom]    = g.h - 1 - o.box.y1;
    a[AHoles]     = std::min(holeCount, 5);
    a[AColors]    = static_cast<int>(std::set<int>(o.col.begin(), o.col.end()).size());
    a[ABorder]    = o.box.x0 == 0 || o.box.y0 == 0 || o.box.x1 == g.w - 1 || o.box.y1 == g.h - 1;
    a[ARect]      = static_cast<int>(o.px.size()) == o.box.w() * o.box.h();
    a[ASymmetric] = dihedral(box, 4) == box || dihedral(box, 5) == box;
    a[AShape]     = shapeHash(shapeOf(o));
    (void)bg;
    return a;
}

std::vector<std::array<int, RelCount>> relationsOf(const G& g, int bg, const std::vector<Obj>& objs)
{
    const int n = static_cast<int>(objs.size());
    std::vector<std::array<int, RelCount>> rel(n);
    int largest = -1, smallest = -1;
    {
        size_t maxS = 0, minS = 1 << 20;
        int maxN = 0, minN = 0;
        for (int i = 0; i < n; ++i) {
            const size_t s = objs[i].px.size();
            if (s > maxS) { maxS = s; largest = i; maxN = 1; } else if (s == maxS) ++maxN;
            if (s < minS) { minS = s; smallest = i; minN = 1; } else if (s == minS) ++minN;
        }
        if (maxN != 1) largest = -1;
        if (minN != 1) smallest = -1;
    }
    std::vector<int> owner(g.p.size(), -1);
    for (int i = 0; i < n; ++i)
        for (auto [x, y] : objs[i].px) owner[y * g.w + x] = i;
    for (int i = 0; i < n; ++i) {
        const Box& a = objs[i].box;
        int nearest = -1, bestGap = 1 << 20;
        bool tie    = false;
        int container = -1, containerArea = 1 << 20;
        for (int j = 0; j < n; ++j) {
            if (j == i) continue;
            const Box& b  = objs[j].box;
            const int gap = std::max(std::max({ 0, b.x0 - a.x1, a.x0 - b.x1 }), std::max({ 0, b.y0 - a.y1, a.y0 - b.y1 }));
            if (gap < bestGap) { bestGap = gap; nearest = j; tie = false; } else if (gap == bestGap) tie = true;
            if (b.x0 <= a.x0 && b.y0 <= a.y0 && b.x1 >= a.x1 && b.y1 >= a.y1 && b.w() * b.h() > a.w() * a.h() && b.w() * b.h() < containerArea) {
                containerArea = b.w() * b.h();
                container     = j;
            }
        }
        std::set<int> touching;
        for (auto [x, y] : objs[i].px)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (g.inRange(nx, ny) && owner[ny * g.w + nx] >= 0 && owner[ny * g.w + nx] != i) touching.insert(owner[ny * g.w + nx]);
                }
        rel[i][RNearest]   = tie ? -1 : nearest;
        rel[i][RLargest]   = largest;
        rel[i][RSmallest]  = smallest;
        rel[i][RTouching]  = touching.size() == 1 ? *touching.begin() : -1;
        rel[i][RContainer] = container;
    }
    (void)bg;
    return rel;
}

std::vector<std::vector<int>> objectFeatures(const G& g, int bg, const std::vector<Obj>& objs)
{
    const int n = static_cast<int>(objs.size());
    std::vector<std::array<int, AtomCount>> atoms;
    for (auto& o : objs) atoms.push_back(atomsOf(g, bg, o));
    const auto rel = relationsOf(g, bg, objs);
    std::array<std::map<int, int>, AtomCount> counts;
    std::array<std::vector<int>, AtomCount> sortedValues;
    for (int a = 0; a < AtomCount; ++a) {
        std::set<int> values;
        for (auto& at : atoms) {
            ++counts[a][at[a]];
            values.insert(at[a]);
        }
        sortedValues[a].assign(values.begin(), values.end());
    }
    std::set<int> colors;
    for (auto& o : objs) colors.insert(o.col.begin(), o.col.end());
    std::vector<std::vector<int>> ret(n);
    for (int i = 0; i < n; ++i) {
        for (const Formula& f : grammar()) {
            int v = -1;
            switch (f.form) {
            case FSelf: v = atoms[i][f.atom]; break;
            case FRel: v = rel[i][f.rel] >= 0 ? atoms[rel[i][f.rel]][f.atom] : -1; break;
            case FEqual: v = rel[i][f.rel] >= 0 ? atoms[rel[i][f.rel]][f.atom] == atoms[i][f.atom] : -1; break;
            case FRankUp:
            case FRankDown: {
                const auto& sv = sortedValues[f.atom];
                const int up   = static_cast<int>(std::lower_bound(sv.begin(), sv.end(), atoms[i][f.atom]) - sv.begin());
                v              = std::min(3, f.form == FRankUp ? up : static_cast<int>(sv.size()) - 1 - up);
                break;
            }
            case FSameCount: v = std::min(3, counts[f.atom][atoms[i][f.atom]]); break;
            case FObjects: v = std::min(n, 10); break;
            case FColors: v = static_cast<int>(colors.size()); break;
            }
            ret[i].push_back(v);
        }
    }
    return ret;
}

enum Comp
{
    Exists,
    Geometry,
    Position,
    Color,
    Addition,
    CompCount
};

const char* compName(int c)
{
    static const char* names[] = { "exists", "geometry", "position", "color", "addition" };
    return names[c];
}

struct V
{
    int t = 0;
    int a = 0;
    int b = 0;
    auto operator<=>(const V&) const = default;
};

using Fate = std::array<V, CompCount>;

const int kDir[8][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
const char* kDirName[8] = { "up", "down", "left", "right", "up-left", "up-right", "down-left", "down-right" };

std::string valueStr(int comp, const V& v)
{
    switch (comp) {
    case Exists: return v.t ? "delete" : "keep";
    case Geometry: return v.t ? dihedralName(v.t) : "keep";
    case Position:
        if (v.t == 0) return v.a || v.b ? "shift(" + std::to_string(v.a) + "," + std::to_string(v.b) + ")" : "stay";
        if (v.t == 1) return std::string("slide ") + kDirName[v.a];
        return "slide toward nearest object";
    case Color:
        if (v.t == 0) return "keep";
        if (v.t == 1) return "recolor " + std::to_string(v.a);
        if (v.t == 3) return "recolor interior " + std::to_string(v.a);
        return "color of nearest object";
    case Addition: {
        static const char* names[] = { "none", "ray", "outline", "outline8", "fill holes", "fill box" };
        if (v.t == 0) return "none";
        if (v.t == 6) return "connect to same color";
        if (v.t == 7) return std::string("corner ray ") + kDirName[v.a];
        if (v.t == 8) return std::string("stamp the ") + (v.a ? "largest" : "most colorful") + " object, aligned by my color";
        if (v.t == 9) return std::string("stamp the ") + (v.a ? "largest" : "most colorful") + " object, centred on me";
        if (v.t == 1) return std::string("ray ") + kDirName[v.a];
        return std::string(names[v.t]) + " " + std::to_string(v.a);
    }
    }
    return "?";
}

int cost(int comp, const V& v)
{
    switch (comp) {
    case Exists: return v.t;
    case Geometry: return v.t == 0 ? 0 : 1;
    case Position: return (v.t == 0 && !v.a && !v.b) ? 0 : (v.t == 0 ? 2 : 1);
    case Color: return v.t == 0 ? 0 : v.t;
    case Addition: return v.t == 0 ? 0 : 1;
    }
    return 0;
}

struct Paint
{
    int x, y, c;
};

struct Scene
{
    const G* g;
    int bg;
    std::vector<Obj> objs;
    std::vector<std::vector<int>> feats;
    std::vector<int> owner;
};

Scene perceive(const G& g, const ParseDef& parse)
{
    Scene s;
    s.g     = &g;
    s.bg    = std::count(g.p.begin(), g.p.end(), 0) ? 0 : background(g);
    s.objs  = parseObjects(g, s.bg, parse);
    s.feats = objectFeatures(g, s.bg, s.objs);
    s.owner.assign(g.p.size(), -1);
    for (size_t i = 0; i < s.objs.size(); ++i)
        for (auto [x, y] : s.objs[i].px) s.owner[y * g.w + x] = static_cast<int>(i);
    return s;
}

int nearestObject(const Scene& s, int i)
{
    const Box& a = s.objs[i].box;
    int best = -1, bestGap = 1 << 20;
    bool tie = false;
    for (size_t j = 0; j < s.objs.size(); ++j) {
        if (static_cast<int>(j) == i) continue;
        const Box& b  = s.objs[j].box;
        const int gx  = std::max({ 0, b.x0 - a.x1, a.x0 - b.x1 });
        const int gy  = std::max({ 0, b.y0 - a.y1, a.y0 - b.y1 });
        const int gap = std::max(gx, gy);
        if (gap < bestGap) {
            best    = static_cast<int>(j);
            bestGap = gap;
            tie     = false;
        } else if (gap == bestGap) {
            tie = true;
        }
    }
    return tie ? -1 : best;
}

std::optional<std::vector<Paint>> shaped(const Obj& o, int k)
{
    G box(o.box.w(), o.box.h(), -1);
    for (size_t i = 0; i < o.px.size(); ++i) box.at(o.px[i].first - o.box.x0, o.px[i].second - o.box.y0) = o.col[i];
    const G t    = dihedral(box, k);
    const int x0 = o.box.x0 + (o.box.w() - t.w) / 2, y0 = o.box.y0 + (o.box.h() - t.h) / 2;
    std::vector<Paint> ret;
    for (int y = 0; y < t.h; ++y)
        for (int x = 0; x < t.w; ++x)
            if (t.at(x, y) >= 0) ret.push_back({ x0 + x, y0 + y, t.at(x, y) });
    return ret;
}

int slideSteps(const Scene& s, int i, const std::vector<Paint>& px, int dir)
{
    const G& g = *s.g;
    auto free  = [&](int step) {
        for (const Paint& p : px) {
            const int x = p.x + kDir[dir][0] * step, y = p.y + kDir[dir][1] * step;
            if (!g.inRange(x, y)) return false;
            const int o = s.owner[y * g.w + x];
            if (o >= 0 && o != i) return false;
        }
        return true;
    };
    int step = 0;
    while (step < 30 && free(step + 1)) ++step;
    return step;
}

int towardDir(const Scene& s, int i)
{
    const int j = nearestObject(s, i);
    if (j < 0) return -1;
    const Box& a = s.objs[i].box;
    const Box& b = s.objs[j].box;
    const int dx = b.x0 > a.x1 ? 1 : (b.x1 < a.x0 ? -1 : 0);
    const int dy = b.y0 > a.y1 ? 1 : (b.y1 < a.y0 ? -1 : 0);
    for (int d = 0; d < 8; ++d)
        if (kDir[d][0] == dx && kDir[d][1] == dy) return d;
    return -1;
}

std::optional<std::pair<int, int>> offsetOf(const Scene& s, int i, const std::vector<Paint>& px, const V& pos)
{
    if (pos.t == 0) return std::make_pair(pos.a, pos.b);
    const int dir = pos.t == 1 ? pos.a : towardDir(s, i);
    if (dir < 0) return std::nullopt;
    const int step = slideSteps(s, i, px, dir);
    return std::make_pair(kDir[dir][0] * step, kDir[dir][1] * step);
}

std::optional<int> colorOf(const Scene& s, int i, const V& color, int original)
{
    if (color.t == 0) return original;
    if (color.t == 1) return color.a;
    const int j = nearestObject(s, i);
    if (j < 0) return std::nullopt;
    return s.objs[j].main;
}

std::optional<std::vector<Paint>> body(const Scene& s, int i, const Fate& f)
{
    std::vector<Paint> ret;
    if (f[Exists].t) return ret;
    auto px = shaped(s.objs[i], f[Geometry].t);
    if (!px) return std::nullopt;
    auto off = offsetOf(s, i, *px, f[Position]);
    if (!off) return std::nullopt;
    std::set<std::pair<int, int>> shape;
    for (const Paint& p : *px) shape.insert({ p.x, p.y });
    for (const Paint& p : *px) {
        const bool inner = shape.count({ p.x - 1, p.y }) && shape.count({ p.x + 1, p.y }) && shape.count({ p.x, p.y - 1 }) && shape.count({ p.x, p.y + 1 });
        auto c           = f[Color].t == 3 ? std::optional<int>(inner ? f[Color].a : p.c) : colorOf(s, i, f[Color], p.c);
        if (!c) return std::nullopt;
        const int x = p.x + off->first, y = p.y + off->second;
        if (!s.g->inRange(x, y)) return std::nullopt;
        ret.push_back({ x, y, *c });
    }
    return ret;
}

std::vector<Paint> addition(const Scene& s, int i, const V& v)
{
    std::vector<Paint> ret;
    const G& g   = *s.g;
    const Obj& o = s.objs[i];
    const int c  = v.a;
    switch (v.t) {
    case 1:
        for (size_t k = 0; k < o.px.size(); ++k) {
            int x = o.px[k].first + kDir[v.a][0], y = o.px[k].second + kDir[v.a][1];
            for (; g.inRange(x, y) && g.at(x, y) == s.bg; x += kDir[v.a][0], y += kDir[v.a][1]) ret.push_back({ x, y, o.col[k] });
        }
        break;
    case 6:
        for (int d = 0; d < 8; ++d)
            for (size_t k = 0; k < o.px.size(); ++k) {
                std::vector<Paint> line;
                int x = o.px[k].first + kDir[d][0], y = o.px[k].second + kDir[d][1];
                for (; g.inRange(x, y) && g.at(x, y) == s.bg; x += kDir[d][0], y += kDir[d][1]) line.push_back({ x, y, o.col[k] });
                if (!line.empty() && g.inRange(x, y) && s.owner[y * g.w + x] != i && g.at(x, y) == o.col[k]) ret.insert(ret.end(), line.begin(), line.end());
            }
        break;
    case 7: {
        int best = -1, bestScore = -(1 << 20), ties = 0;
        for (size_t k = 0; k < o.px.size(); ++k) {
            const int score = o.px[k].first * kDir[v.a][0] + o.px[k].second * kDir[v.a][1];
            if (score > bestScore) { bestScore = score; best = static_cast<int>(k); ties = 1; } else if (score == bestScore) ++ties;
        }
        if (ties != 1) break;
        int x = o.px[best].first + kDir[v.a][0], y = o.px[best].second + kDir[v.a][1];
        for (; g.inRange(x, y) && g.at(x, y) == s.bg; x += kDir[v.a][0], y += kDir[v.a][1]) ret.push_back({ x, y, o.col[best] });
        break;
    }
    case 8:
    case 9: {
        int tmpl = -1, best = -1, ties = 0;
        for (size_t j = 0; j < s.objs.size(); ++j) {
            const Obj& t    = s.objs[j];
            const int score = v.a == 0 ? static_cast<int>(std::set<int>(t.col.begin(), t.col.end()).size()) : static_cast<int>(t.px.size());
            if (score > best) { best = score; tmpl = static_cast<int>(j); ties = 1; } else if (score == best) ++ties;
        }
        if (ties != 1 || tmpl == i || (v.a == 0 && best < 2)) break;
        const Obj& t = s.objs[tmpl];
        int dx = 0, dy = 0;
        if (v.t == 8) {
            int found = 0;
            for (size_t k = 0; k < t.px.size(); ++k)
                if (t.col[k] == o.main) {
                    ++found;
                    dx = o.px[0].first - t.px[k].first;
                    dy = o.px[0].second - t.px[k].second;
                }
            if (found != 1 || o.px.size() != 1) break;
        } else {
            dx = (o.box.x0 + o.box.x1) / 2 - (t.box.x0 + t.box.x1) / 2;
            dy = (o.box.y0 + o.box.y1) / 2 - (t.box.y0 + t.box.y1) / 2;
        }
        for (size_t k = 0; k < t.px.size(); ++k) {
            const int x = t.px[k].first + dx, y = t.px[k].second + dy;
            if (g.inRange(x, y)) ret.push_back({ x, y, t.col[k] });
        }
        break;
    }
    case 2:
    case 3: {
        std::vector<char> done(g.p.size(), 0);
        for (auto [x, y] : o.px)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((!dx && !dy) || (v.t == 2 && dx && dy)) continue;
                    const int nx = x + dx, ny = y + dy;
                    if (g.inRange(nx, ny) && g.at(nx, ny) == s.bg && !done[ny * g.w + nx]) {
                        done[ny * g.w + nx] = 1;
                        ret.push_back({ nx, ny, c });
                    }
                }
        break;
    }
    case 4:
    case 5: {
        G local(o.box.w(), o.box.h(), 0);
        for (auto [x, y] : o.px) local.at(x - o.box.x0, y - o.box.y0) = 1;
        const auto holes = enclosedMask(local, 0);
        for (int y = 0; y < local.h; ++y)
            for (int x = 0; x < local.w; ++x) {
                const int gx = x + o.box.x0, gy = y + o.box.y0;
                if (!local.at(x, y) && g.at(gx, gy) == s.bg && (v.t == 5 || holes[y * local.w + x])) ret.push_back({ gx, gy, c });
            }
        break;
    }
    default:
        break;
    }
    return ret;
}

struct Options
{
    std::array<std::set<V>, CompCount> values;
    std::array<bool, CompCount> any {};
};

bool matches(const G& out, const std::vector<Paint>& px)
{
    return std::all_of(px.begin(), px.end(), [&](const Paint& p) { return out.inRange(p.x, p.y) && out.at(p.x, p.y) == p.c; });
}

Options observe(const Scene& s, int i, const G& out, bool allowShifts)
{
    Options opt;
    const Obj& o = s.objs[i];
    for (int k = 0; k < 8; ++k) {
        auto px = shaped(o, k);
        if (!px || px->empty()) continue;
        std::vector<V> positions { { 0, 0, 0 }, { 2 } };
        for (int d = 0; d < 4; ++d) positions.push_back({ 1, d });
        if (allowShifts) {
            const Paint& a = (*px)[0];
            for (int dy = -s.g->h + 1; dy < s.g->h; ++dy)
                for (int dx = -s.g->w + 1; dx < s.g->w; ++dx)
                    if ((dx || dy) && out.inRange(a.x + dx, a.y + dy) && out.at(a.x + dx, a.y + dy) != s.bg) positions.push_back({ 0, dx, dy });
        }
        for (const V& pos : positions) {
            auto off = offsetOf(s, i, *px, pos);
            if (!off) continue;
            const int ax = (*px)[0].x + off->first, ay = (*px)[0].y + off->second;
            if (!out.inRange(ax, ay)) continue;
            if (off->first || off->second) {
                int occupied = 0;
                for (const Paint& p : *px) {
                    const int x = p.x + off->first, y = p.y + off->second;
                    if (!s.g->inRange(x, y)) continue;
                    const int owner = s.owner[y * s.g->w + x];
                    occupied += owner >= 0 && owner != i;
                }
                if (occupied * 2 > static_cast<int>(px->size())) continue;
            }
            std::vector<V> colors { V { 0 }, V { 1, out.at(ax, ay) }, V { 2 } };
            std::set<int> landing;
            for (const Paint& p : *px) {
                const int x = p.x + off->first, y = p.y + off->second;
                if (out.inRange(x, y) && out.at(x, y) != p.c) landing.insert(out.at(x, y));
            }
            if (landing.size() == 1) colors.push_back({ 3, *landing.begin() });
            for (const V& color : colors) {
                if (color.t == 1 && color.a == s.bg) continue;
                Fate f {};
                f[Geometry] = { k };
                f[Position] = pos;
                f[Color]    = color;
                auto b      = body(s, i, f);
                if (b && matches(out, *b)) {
                    opt.values[Exists].insert({ 0 });
                    opt.values[Geometry].insert({ k });
                    opt.values[Position].insert(pos);
                    opt.values[Color].insert(color);
                }
            }
        }
    }
    bool changed = false;
    for (size_t k = 0; k < o.px.size(); ++k) changed = changed || out.at(o.px[k].first, o.px[k].second) != o.col[k];
    if (changed) {
        opt.values[Exists].insert({ 1 });
        if (opt.values[Geometry].empty()) {
            opt.any[Geometry] = opt.any[Position] = opt.any[Color] = true;
        }
    }
    opt.values[Addition].insert({ 0 });
    for (int d = 0; d < 8; ++d) {
        auto px = addition(s, i, { 1, d });
        if (!px.empty() && matches(out, px)) opt.values[Addition].insert({ 1, d });
    }
    {
        auto px = addition(s, i, { 6 });
        if (!px.empty() && matches(out, px)) opt.values[Addition].insert({ 6 });
    }
    for (int d = 0; d < 8; ++d) {
        auto px = addition(s, i, { 7, d });
        if (!px.empty() && matches(out, px)) opt.values[Addition].insert({ 7, d });
    }
    for (int t = 8; t <= 9; ++t)
        for (int a = 0; a < 2; ++a) {
            auto px = addition(s, i, { t, a });
            if (!px.empty() && matches(out, px)) opt.values[Addition].insert({ t, a });
        }
    for (int t = 2; t <= 5; ++t) {
        auto probe = addition(s, i, { t, 0 });
        if (probe.empty()) continue;
        const V v { t, out.at(probe[0].x, probe[0].y) };
        if (v.a == s.bg) continue;
        if (matches(out, addition(s, i, v))) opt.values[Addition].insert(v);
    }
    return opt;
}

using Key = std::vector<int>;

struct CompRule
{
    std::vector<int> chosen;
    std::map<Key, V> table;
    V fallback {};
};

using Rule = std::array<CompRule, CompCount>;

Key keyOf(const std::vector<int>& features, const std::vector<int>& chosen)
{
    Key k;
    for (int f : chosen) k.push_back(features[f]);
    return k;
}

std::optional<Fate> fateOf(const Rule& rule, const std::vector<int>& feats, bool backoff)
{
    Fate f;
    for (int c = 0; c < CompCount; ++c) {
        auto it = rule[c].table.find(keyOf(feats, rule[c].chosen));
        if (it == rule[c].table.end()) {
            if (!rule[c].table.empty() && !backoff) return std::nullopt;
            f[c] = rule[c].table.empty() ? V {} : rule[c].fallback;
        } else {
            f[c] = it->second;
        }
        if (c == Exists && f[Exists].t == 1) {
            auto add       = rule[Addition].table.find(keyOf(feats, rule[Addition].chosen));
            f[Addition]    = add != rule[Addition].table.end() ? add->second : V {};
            return f;
        }
    }
    return f;
}

std::optional<G> applyRule(const G& in, const ParseDef& parse, const Rule& rule, bool backoff = false)
{
    const Scene s = perceive(in, parse);
    if (s.objs.empty() || s.objs.size() > 60) return std::nullopt;
    G out = in;
    for (auto& o : s.objs)
        for (auto [x, y] : o.px) out.at(x, y) = s.bg;
    std::vector<std::vector<Paint>> bodies;
    for (size_t i = 0; i < s.objs.size(); ++i) {
        auto f = fateOf(rule, s.feats[i], backoff);
        if (!f) return std::nullopt;
        for (const Paint& p : addition(s, static_cast<int>(i), (*f)[Addition])) out.at(p.x, p.y) = p.c;
        auto b = body(s, static_cast<int>(i), *f);
        if (!b) return std::nullopt;
        bodies.push_back(std::move(*b));
    }
    for (auto& b : bodies)
        for (const Paint& p : b) out.at(p.x, p.y) = p.c;
    return out;
}

std::vector<CompRule> componentTables(int comp, const std::vector<Scene>& scenes, const std::vector<std::vector<Options>>& opts, bool preferNonDefault, size_t maxTables)
{
    std::vector<int> active;
    {
        std::set<std::vector<int>> partitions;
        for (int f : formulaOrder()) {
            std::map<int, int> relabel;
            std::vector<int> partition;
            for (auto& s : scenes)
                for (auto& feats : s.feats) partition.push_back(relabel.emplace(feats[f], static_cast<int>(relabel.size())).first->second);
            if (relabel.size() > 1 && partitions.insert(partition).second) active.push_back(f);
        }
    }
    std::vector<std::vector<int>> subsets { {} };
    for (int f : active) subsets.push_back({ f });
    const size_t pairLimit = std::min<size_t>(active.size(), 30);
    for (size_t i = 0; i < pairLimit; ++i)
        for (size_t j = i + 1; j < pairLimit; ++j) subsets.push_back({ active[i], active[j] });
    struct Scored
    {
        int residual;
        double bits;
        CompRule rule;
    };
    std::vector<Scored> found;
    for (const auto& chosen : subsets) {
        std::map<Key, std::set<V>> inter;
        std::map<Key, uint32_t> seenIn;
        std::map<Key, int> objectCount;
        bool ok = true;
        for (size_t si = 0; si < scenes.size() && ok; ++si)
            for (size_t oi = 0; oi < scenes[si].objs.size() && ok; ++oi) {
                const Options& o = opts[si][oi];
                if (o.any[comp]) continue;
                const Key k = keyOf(scenes[si].feats[oi], chosen);
                seenIn[k] |= 1u << si;
                ++objectCount[k];
                auto it = inter.find(k);
                if (it == inter.end()) {
                    inter.emplace(k, o.values[comp]);
                } else {
                    std::set<V> both;
                    std::set_intersection(it->second.begin(), it->second.end(), o.values[comp].begin(), o.values[comp].end(), std::inserter(both, both.begin()));
                    it->second = std::move(both);
                    ok         = !it->second.empty();
                }
            }
        if (!ok) continue;
        CompRule r { chosen, {} };
        int bestCount = 0;
        for (auto& [k, vals] : inter) {
            std::vector<V> sorted(vals.begin(), vals.end());
            std::stable_sort(sorted.begin(), sorted.end(), [&](const V& a, const V& b) {
                const int ca = cost(comp, a), cb = cost(comp, b);
                if (preferNonDefault && (ca == 0) != (cb == 0)) return cb == 0;
                return ca < cb;
            });
            r.table[k] = sorted[0];
            if (objectCount[k] > bestCount) {
                bestCount  = objectCount[k];
                r.fallback = sorted[0];
            }
        }
        const bool trivial = std::all_of(r.table.begin(), r.table.end(), [&](auto& e) { return cost(comp, e.second) == 0; });
        if (trivial && !chosen.empty()) continue;
        int unsupported = 0;
        for (auto& [k, mask] : seenIn) unsupported += std::popcount(mask) < 2;
        if (scenes.size() < 2) unsupported = 0;
        int explained = 0;
        if (comp == Addition) {
            for (size_t si = 0; si < scenes.size(); ++si) {
                std::set<int> painted;
                for (size_t oi = 0; oi < scenes[si].objs.size(); ++oi) {
                    const V& v = r.table[keyOf(scenes[si].feats[oi], chosen)];
                    for (const Paint& p : addition(scenes[si], static_cast<int>(oi), v)) if (scenes[si].g->at(p.x, p.y) == scenes[si].bg) painted.insert(p.y * scenes[si].g->w + p.x);
                }
                explained += static_cast<int>(painted.size());
            }
        }
        double bits = 4.0 * static_cast<double>(r.table.size()) + 8.0 * unsupported;
        for (int f : chosen) bits += formulaBits()[f];
        found.push_back({ -explained, bits, std::move(r) });
    }
    std::stable_sort(found.begin(), found.end(), [](const Scored& a, const Scored& b) {
        return std::tie(a.residual, a.bits) < std::tie(b.residual, b.bits);
    });
    std::vector<CompRule> ret;
    for (size_t i = 0; i < found.size() && i < maxTables; ++i) ret.push_back(std::move(found[i].rule));
    return ret;
}

std::string describe(const ParseDef& parse, const Rule& rule);

void why(const ParseDef& parse, const char* reason)
{
    static const bool on = std::getenv("INFOCELL_OBJ_DEBUG") != nullptr;
    if (on) std::fprintf(stderr, "objdebug %s: %s\n", parse.name, reason);
}

std::optional<Rule> learnRule(const ParseDef& parse, const std::vector<Pair>& train)
{
    if (!sameSizes(train)) return std::nullopt;
    std::vector<Scene> scenes;
    std::vector<std::vector<Options>> opts;
    for (const Pair& pair : train) {
        if (pair.in == pair.out) return std::nullopt;
        Scene s = perceive(pair.in, parse);
        if (s.objs.empty() || s.objs.size() > 60) { why(parse, "object count"); return std::nullopt; }
        std::vector<Options> o;
        const bool shifts = s.objs.size() <= 30 && parse.kind < 2;
        for (size_t i = 0; i < s.objs.size(); ++i) {
            o.push_back(observe(s, static_cast<int>(i), pair.out, shifts && s.objs[i].px.size() <= 200));
            if (o.back().values[Exists].empty()) { why(parse, "unexplained object"); return std::nullopt; }
        }
        if (std::getenv("INFOCELL_OBJ_DEBUG2")) {
            for (size_t i = 0; i < s.objs.size(); ++i) {
                std::fprintf(stderr, "obj color %d size %zu box %d,%d-%d,%d:", s.objs[i].main, s.objs[i].px.size(), s.objs[i].box.x0, s.objs[i].box.y0, s.objs[i].box.x1, s.objs[i].box.y1);
                for (int c = 0; c < CompCount; ++c) {
                    std::fprintf(stderr, " %s[", compName(c));
                    for (const V& v : o[i].values[c]) std::fprintf(stderr, "%s,", valueStr(c, v).c_str());
                    std::fprintf(stderr, "]%s", o[i].any[c] ? "*" : "");
                }
                std::fprintf(stderr, "\n");
            }
        }
        scenes.push_back(std::move(s));
        opts.push_back(std::move(o));
    }
    std::array<std::vector<CompRule>, CompCount> cands;
    for (int c = 0; c < CompCount; ++c) {
        const bool both = c == Exists || c == Addition;
        cands[c]        = componentTables(c, scenes, opts, false, both ? 2 : 4);
        if (both) {
            for (auto& r : componentTables(c, scenes, opts, true, 2)) cands[c].push_back(std::move(r));
        }
        if (cands[c].empty()) { why(parse, compName(c)); return std::nullopt; }
        if (std::getenv("INFOCELL_OBJ_DEBUG2")) {
            for (auto& r : cands[c]) {
                std::fprintf(stderr, "cand %s by", compName(c));
                for (int f : r.chosen) std::fprintf(stderr, " %s", featureName(f).c_str());
                std::fprintf(stderr, ":");
                for (auto& [k, v] : r.table) std::fprintf(stderr, " %s", valueStr(c, v).c_str());
                std::fprintf(stderr, "\n");
            }
        }
    }
    std::vector<std::array<size_t, CompCount>> combos { {} };
    for (int c = 0; c < CompCount; ++c) {
        std::vector<std::array<size_t, CompCount>> next;
        for (auto& combo : combos)
            for (size_t i = 0; i < cands[c].size(); ++i) {
                auto n = combo;
                n[c]   = i;
                next.push_back(n);
            }
        combos = std::move(next);
    }
    auto rankSum = [](const std::array<size_t, CompCount>& a) { size_t s = 0; for (size_t v : a) s += v; return s; };
    std::stable_sort(combos.begin(), combos.end(), [&](auto& a, auto& b) { return rankSum(a) < rankSum(b); });
    if (combos.size() > 300) combos.resize(300);
    for (const auto& idx : combos) {
        Rule rule;
        for (int c = 0; c < CompCount; ++c) rule[c] = cands[c][idx[c]];
        bool verified = true;
        for (size_t pi = 0; pi < train.size() && verified; ++pi) {
            auto out = applyRule(train[pi].in, parse, rule);
            verified = out && *out == train[pi].out;
        }
        if (verified) return rule;
        if (std::getenv("INFOCELL_OBJ_DEBUG") && idx == std::array<size_t, CompCount> {}) {
            std::fprintf(stderr, "objdebug first candidate: %s\n", describe(parse, rule).c_str());
            for (size_t pi = 0; pi < train.size(); ++pi) {
                auto out = applyRule(train[pi].in, parse, rule);
                if (!out) { std::fprintf(stderr, "  pair %zu: no output\n", pi); continue; }
                for (int y = 0; y < out->h; ++y)
                    for (int x = 0; x < out->w; ++x)
                        if (out->at(x, y) != train[pi].out.at(x, y)) std::fprintf(stderr, "  pair %zu (%d,%d): got %d want %d\n", pi, x, y, out->at(x, y), train[pi].out.at(x, y));
            }
        }
    }
    why(parse, "verification");
    return std::nullopt;
}

std::string describe(const ParseDef& parse, const Rule& rule)
{
    std::string s = std::string("objects[") + parse.name + "]";
    for (int c = 0; c < CompCount; ++c) {
        const CompRule& r = rule[c];
        if (std::all_of(r.table.begin(), r.table.end(), [&](auto& e) { return cost(c, e.second) == 0; })) continue;
        s += std::string(" ") + compName(c);
        if (r.chosen.empty()) {
            s += ": " + valueStr(c, r.table.begin()->second) + ";";
            continue;
        }
        s += " by ";
        for (size_t i = 0; i < r.chosen.size(); ++i) s += std::string(i ? ", " : "") + "{" + featureName(r.chosen[i]) + "}";
        s += ":";
        for (auto& [k, v] : r.table) {
            s += " ";
            for (size_t i = 0; i < k.size(); ++i) s += (i ? "/" : "") + std::to_string(k[i]);
            s += " -> " + valueStr(c, v) + ",";
        }
        s.back() = ';';
    }
    return s;
}

std::optional<Rule> learnRuleCached(const ParseDef& parse, const std::vector<Pair>& train)
{
    uint64_t h = 1469598103934665603ull;
    auto mix   = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (const char* c = parse.name; *c; ++c) mix(static_cast<uint64_t>(*c));
    for (const Pair& p : train)
        for (const G* g : { &p.in, &p.out }) {
            mix(static_cast<uint64_t>(g->w));
            mix(static_cast<uint64_t>(g->h));
            for (int v : g->p) mix(static_cast<uint64_t>(v + 1));
        }
    static std::mutex mutex;
    static std::map<uint64_t, std::optional<Rule>> cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(h);
        if (it != cache.end()) return it->second;
    }
    auto rule = learnRule(parse, train);
    std::lock_guard<std::mutex> lock(mutex);
    cache[h] = rule;
    return rule;
}

Learner objectRuleLearner(const ParseDef& parse, bool backoff)
{
    const std::string family = std::string(backoff ? "backoff objects[" : "objects[") + parse.name + "]";
    Learner l { family, 3.0, [parse, backoff](const std::vector<Pair>& train) -> std::optional<Predictor> {
                   auto rule = learnRuleCached(parse, train);
                   if (!rule) return std::nullopt;
                   return Predictor([parse, rule = *rule, backoff](const G& in) { return applyRule(in, parse, rule, backoff); });
               } };
    l.learnFull = [parse, backoff](const std::vector<Pair>& train) -> std::optional<Learner::Learned> {
        auto rule = learnRuleCached(parse, train);
        if (!rule) return std::nullopt;
        double complexity = backoff ? 3.5 : 2.0;
        for (int c = 0; c < CompCount; ++c) {
            const CompRule& r = (*rule)[c];
            if (std::all_of(r.table.begin(), r.table.end(), [&](auto& e) { return cost(c, e.second) == 0; })) continue;
            complexity += 0.3 + 0.6 * static_cast<double>(r.chosen.size()) + 0.05 * static_cast<double>(r.table.size());
        }
        const std::string name = (backoff ? "backoff " : "") + describe(parse, *rule);
        return Learner::Learned { Predictor([parse, rule = *rule, backoff](const G& in) { return applyRule(in, parse, rule, backoff); }), name, complexity };
    };
    return l;
}

struct Selection
{
    int feature;
    int mode;
    int value;
    int k;
    bool only;
};

std::optional<size_t> select(const Scene& s, const Selection& sel)
{
    std::optional<size_t> found;
    std::map<int, int> count;
    for (auto& f : s.feats) ++count[f[sel.feature]];
    for (size_t i = 0; i < s.objs.size(); ++i) {
        const int v = s.feats[i][sel.feature];
        bool hit    = false;
        switch (sel.mode) {
        case 0: hit = v == sel.value; break;
        case 1: hit = count[v] == 1 && count.size() > 1; break;
        case 2: hit = v == count.rbegin()->first && count.rbegin()->second == 1; break;
        case 3: hit = v == count.begin()->first && count.begin()->second == 1; break;
        }
        if (hit) {
            if (found) return std::nullopt;
            found = i;
        }
    }
    return found;
}

G cropObject(const Scene& s, size_t i, bool only)
{
    const Obj& o = s.objs[i];
    G out        = crop(*s.g, o.box);
    if (only) {
        G mask(o.box.w(), o.box.h(), s.bg);
        for (size_t k = 0; k < o.px.size(); ++k) mask.at(o.px[k].first - o.box.x0, o.px[k].second - o.box.y0) = o.col[k];
        out = mask;
    }
    return out;
}

std::optional<G> applySelection(const G& in, const ParseDef& parse, const Selection& sel)
{
    const Scene s = perceive(in, parse);
    if (s.objs.size() < 2 || s.objs.size() > 60) return std::nullopt;
    auto i = select(s, sel);
    if (!i) return std::nullopt;
    return dihedral(cropObject(s, *i, sel.only), sel.k);
}

std::optional<Selection> learnSelection(const ParseDef& parse, const std::vector<Pair>& train)
{
    if (train.empty() || sameSizes(train)) return std::nullopt;
    std::vector<Scene> scenes;
    for (const Pair& p : train) {
        scenes.push_back(perceive(p.in, parse));
        if (scenes.back().objs.size() < 2 || scenes.back().objs.size() > 60) return std::nullopt;
    }
    const int nf = static_cast<int>(grammar().size());
    for (int k = 0; k < 8; ++k)
        for (int only = 0; only < 2; ++only)
            for (size_t i = 0; i < scenes[0].objs.size(); ++i) {
                if (dihedral(cropObject(scenes[0], i, only), k) != train[0].out) continue;
                for (int mode = 1; mode <= 4; ++mode)
                    for (int f : formulaOrder()) {
                        Selection sel { f, mode % 4, scenes[0].feats[i][f], k, only != 0 };
                        bool ok = true;
                        for (size_t pi = 0; pi < train.size() && ok; ++pi) {
                            auto j = select(scenes[pi], sel);
                            ok     = j && dihedral(cropObject(scenes[pi], *j, sel.only), k) == train[pi].out;
                        }
                        if (ok) return sel;
                    }
            }
    return std::nullopt;
}

std::string describeSelection(const ParseDef& parse, const Selection& sel)
{
    static const char* modes[] = { "with", "with the unique", "with the largest", "with the smallest" };
    std::string s = std::string("crop the object[") + parse.name + "] " + modes[sel.mode] + " {" + featureName(sel.feature) + "}";
    if (sel.mode == 0) s += " = " + std::to_string(sel.value);
    if (sel.only) s += " (object pixels only)";
    if (sel.k) s += std::string(", then ") + dihedralName(sel.k);
    return s;
}

Learner selectionLearner(const ParseDef& parse)
{
    Learner l { std::string("crop the object[") + parse.name + "]", 3.0, [parse](const std::vector<Pair>& train) -> std::optional<Predictor> {
                   auto sel = learnSelection(parse, train);
                   if (!sel) return std::nullopt;
                   return Predictor([parse, sel = *sel](const G& in) { return applySelection(in, parse, sel); });
               } };
    l.explain = [parse](const std::vector<Pair>& train) {
        auto sel = learnSelection(parse, train);
        return sel ? describeSelection(parse, *sel) : std::string();
    };
    l.learnFull = [parse](const std::vector<Pair>& train) -> std::optional<Learner::Learned> {
        auto sel = learnSelection(parse, train);
        if (!sel) return std::nullopt;
        const double complexity = 2.6 + 0.3 * (sel->k != 0) + 0.3 * (sel->mode == 0) + 0.2 * sel->only;
        return Learner::Learned { Predictor([parse, sel = *sel](const G& in) { return applySelection(in, parse, sel); }), describeSelection(parse, *sel), complexity };
    };
    return l;
}

struct Pack
{
    int atom;
    bool desc;
    int layout;
    int sizeMode;
    int w, h;
    int valueFormula;
    std::map<int, int> valueTable;
};

const int kSortAtoms[] = { ALeft, ATop, ASize, AColor, ARight, ABottom };

int selfFormula(int atom)
{
    for (size_t f = 0; f < grammar().size(); ++f)
        if (grammar()[f].form == FSelf && grammar()[f].atom == atom) return static_cast<int>(f);
    return -1;
}

std::pair<int, int> cellOf(int layout, int k, int w, int h)
{
    switch (layout) {
    case 1: return { k / h, k % h };
    case 2: { const int y = k / w, x = k % w; return { y % 2 ? w - 1 - x : x, y }; }
    case 3: { const int x = k / h, y = k % h; return { x, x % 2 ? h - 1 - y : y }; }
    default: return { k % w, k / w };
    }
}

std::vector<size_t> sortedObjects(const Scene& s, int atom, bool desc)
{
    const int f = selfFormula(atom), top = selfFormula(ATop), left = selfFormula(ALeft);
    std::vector<size_t> idx(s.objs.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        const int va = s.feats[a][f], vb = s.feats[b][f];
        if (va != vb) return desc ? va > vb : va < vb;
        return std::tie(s.feats[a][top], s.feats[a][left]) < std::tie(s.feats[b][top], s.feats[b][left]);
    });
    return idx;
}

std::optional<std::pair<int, int>> packSize(const Pack& p, size_t n)
{
    if (p.sizeMode == 1) return std::make_pair(static_cast<int>(n), 1);
    if (p.sizeMode == 2) return std::make_pair(1, static_cast<int>(n));
    if (static_cast<size_t>(p.w * p.h) < n) return std::nullopt;
    return std::make_pair(p.w, p.h);
}

std::optional<G> applyPack(const G& in, const ParseDef& parse, const Pack& p)
{
    const Scene s = perceive(in, parse);
    if (s.objs.empty() || s.objs.size() > 30) return std::nullopt;
    auto size = packSize(p, s.objs.size());
    if (!size) return std::nullopt;
    G out(size->first, size->second, 0);
    const auto order = sortedObjects(s, p.atom, p.desc);
    for (size_t k = 0; k < order.size(); ++k) {
        int v = s.objs[order[k]].main;
        if (p.valueFormula >= 0) {
            auto it = p.valueTable.find(s.feats[order[k]][p.valueFormula]);
            if (it == p.valueTable.end()) return std::nullopt;
            v = it->second;
        }
        auto [x, y] = cellOf(p.layout, static_cast<int>(k), out.w, out.h);
        out.at(x, y) = v;
    }
    return out;
}

std::optional<Pack> learnPack(const ParseDef& parse, const std::vector<Pair>& train)
{
    if (train.empty() || sameSizes(train) || parse.kind >= 3) return std::nullopt;
    std::vector<Scene> scenes;
    for (const Pair& p : train) {
        scenes.push_back(perceive(p.in, parse));
        if (scenes.back().objs.empty() || scenes.back().objs.size() > 30) return std::nullopt;
    }
    for (int sizeMode = 0; sizeMode < 3; ++sizeMode) {
        if (sizeMode == 0 && std::any_of(train.begin(), train.end(), [&](const Pair& p) { return p.out.w != train[0].out.w || p.out.h != train[0].out.h; })) continue;
        for (int atom : kSortAtoms)
            for (int desc = 0; desc < 2; ++desc)
                for (int layout = 0; layout < 4; ++layout) {
                    Pack p { atom, desc != 0, layout, sizeMode, train[0].out.w, train[0].out.h, -1, {} };
                    std::vector<std::vector<int>> targets;
                    bool ok = true;
                    for (size_t pi = 0; pi < train.size() && ok; ++pi) {
                        auto size = packSize(p, scenes[pi].objs.size());
                        ok        = size && size->first == train[pi].out.w && size->second == train[pi].out.h;
                        if (!ok) break;
                        const size_t n = scenes[pi].objs.size();
                        std::vector<int> t;
                        std::set<std::pair<int, int>> used;
                        for (size_t k = 0; k < n; ++k) {
                            auto c = cellOf(layout, static_cast<int>(k), size->first, size->second);
                            used.insert(c);
                            t.push_back(train[pi].out.at(c.first, c.second));
                        }
                        for (int y = 0; y < train[pi].out.h && ok; ++y)
                            for (int x = 0; x < train[pi].out.w && ok; ++x) ok = used.count({ x, y }) || train[pi].out.at(x, y) == 0;
                        targets.push_back(std::move(t));
                    }
                    if (!ok) continue;
                    std::vector<std::vector<size_t>> orders;
                    for (auto& s : scenes) orders.push_back(sortedObjects(s, atom, desc != 0));
                    bool own = true;
                    for (size_t pi = 0; pi < scenes.size() && own; ++pi)
                        for (size_t k = 0; k < orders[pi].size() && own; ++k) own = scenes[pi].objs[orders[pi][k]].main == targets[pi][k];
                    if (own) return p;
                    size_t total = 0;
                    for (auto& s : scenes) total += s.objs.size();
                    for (int f : formulaOrder()) {
                        std::map<int, int> table;
                        bool consistent = true;
                        for (size_t pi = 0; pi < scenes.size() && consistent; ++pi)
                            for (size_t k = 0; k < orders[pi].size() && consistent; ++k) {
                                auto [it, inserted] = table.emplace(scenes[pi].feats[orders[pi][k]][f], targets[pi][k]);
                                consistent          = inserted || it->second == targets[pi][k];
                            }
                        if (consistent && table.size() * 2 <= total) {
                            p.valueFormula = f;
                            p.valueTable   = table;
                            return p;
                        }
                    }
                }
    }
    return std::nullopt;
}

std::string describePack(const ParseDef& parse, const Pack& p)
{
    static const char* layouts[] = { "row by row", "column by column", "snake rows", "snake columns" };
    static const char* sizes[]   = { "", " (one row)", " (one column)" };
    std::string s = std::string("pack the objects[") + parse.name + "] sorted by {" + kAtomName[p.atom] + "}" + (p.desc ? " descending" : "") + ", " + layouts[p.layout] + sizes[p.sizeMode];
    if (p.valueFormula >= 0) {
        s += ", value by {" + featureName(p.valueFormula) + "}:";
        for (auto& [k, v] : p.valueTable) s += " " + std::to_string(k) + " -> " + std::to_string(v) + ",";
        s.pop_back();
    }
    return s;
}

Learner packLearner(const ParseDef& parse)
{
    Learner l { std::string("pack the objects[") + parse.name + "]", 3.0, [parse](const std::vector<Pair>& train) -> std::optional<Predictor> {
                   auto p = learnPack(parse, train);
                   if (!p) return std::nullopt;
                   return Predictor([parse, p = *p](const G& in) { return applyPack(in, parse, p); });
               } };
    l.learnFull = [parse](const std::vector<Pair>& train) -> std::optional<Learner::Learned> {
        auto p = learnPack(parse, train);
        if (!p) return std::nullopt;
        const double complexity = 2.8 + 0.2 * (p->layout != 0) + (p->valueFormula >= 0 ? 0.6 + 0.05 * static_cast<double>(p->valueTable.size()) : 0.0);
        return Learner::Learned { Predictor([parse, p = *p](const G& in) { return applyPack(in, parse, p); }), describePack(parse, *p), complexity };
    };
    return l;
}

} // namespace

std::vector<Learner> objectRuleLearners()
{
    std::vector<Learner> ret;
    for (const ParseDef& parse : kParses) ret.push_back(objectRuleLearner(parse, false));
    for (const ParseDef& parse : kParses) ret.push_back(objectRuleLearner(parse, true));
    for (const ParseDef& parse : kParses) ret.push_back(selectionLearner(parse));
    for (const ParseDef& parse : kParses) ret.push_back(packLearner(parse));
    return ret;
}

std::vector<std::string> conceptsIn(const std::string& description)
{
    std::vector<std::string> ret;
    for (size_t open = description.find('{'); open != std::string::npos; open = description.find('{', open + 1)) {
        const size_t close = description.find('}', open);
        if (close == std::string::npos) break;
        ret.push_back(description.substr(open + 1, close - open - 1));
    }
    return ret;
}

size_t loadConceptPrior(const std::map<std::string, int>& counts)
{
    constexpr double kPriorWeight = 20.0;
    double total                  = 0.0;
    size_t known                  = 0;
    for (auto& [name, n] : counts) total += n;
    double grammarMass = 0.0;
    for (const Formula& f : grammar()) grammarMass += std::exp2(-f.bits);
    auto& bits = formulaBits();
    for (size_t i = 0; i < grammar().size(); ++i) {
        auto it        = counts.find(grammar()[i].name);
        const double n = it == counts.end() ? 0.0 : it->second;
        known += it != counts.end();
        const double prior = std::exp2(-grammar()[i].bits) / grammarMass;
        bits[i]            = -std::log2((n + kPriorWeight * prior) / (total + kPriorWeight));
    }
    loadPixelConceptPrior(counts);
    auto& order = formulaOrderStorage();
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return bits[a] < bits[b]; });
    return known;
}

ObjectVocabulary objectVocabulary()
{
    ObjectVocabulary v;
    for (const char* a : kAtomName) v.atoms.push_back(a);
    for (const char* r : kRelName) v.relations.push_back(r);
    for (const ParseDef& p : kParses) v.parses.push_back(p.name);
    for (int f : formulaOrder()) v.formulas.push_back({ grammar()[f].name, formulaBits()[f] });
    auto values = [&](int comp, std::vector<V> vs) {
        std::vector<std::string> names;
        for (const V& x : vs) names.push_back(valueStr(comp, x));
        v.components.push_back({ compName(comp), names });
    };
    values(Exists, { { 0 }, { 1 } });
    values(Geometry, { { 0 }, { 1 }, { 2 }, { 3 }, { 4 }, { 5 }, { 6 }, { 7 } });
    values(Position, { { 0 }, { 0, 1, 0 }, { 1, 0 }, { 2 } });
    values(Color, { { 0 }, { 1, 4 }, { 3, 4 }, { 2 } });
    values(Addition, { { 0 }, { 1, 0 }, { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6 }, { 7, 4 }, { 8, 1 }, { 9, 1 } });
    return v;
}

std::vector<std::pair<std::string, double>> conceptLibrary(size_t top)
{
    std::vector<std::pair<std::string, double>> ret;
    for (int f : formulaOrder()) {
        if (ret.size() >= top) break;
        ret.push_back({ grammar()[f].name, formulaBits()[f] });
    }
    return ret;
}

} // namespace search
} // namespace arc
} // namespace infocell
