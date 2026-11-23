#include "ProgramSearch.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <bit>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

#include "ObjectRules.h"
#include "Perception.h"
#include "RuleInduction.h"

namespace infocell {
namespace arc {
namespace search {

G G::fromNative(const native::Grid& grid)
{
    G ret(grid.width(), grid.height());
    for (size_t i = 0; i < grid.pixels().size(); ++i) {
        ret.p[i] = static_cast<int>(grid.pixels()[i].id());
    }
    return ret;
}

namespace {

using Key      = std::vector<int>;
using Features = std::function<std::vector<Key>(const G&)>;

Learner lookupLearner(const std::string& name, double complexity, Features features)
{
    return { name, complexity, [features](const std::vector<Pair>& train) -> std::optional<Predictor> {
                if (!sameSizes(train)) {
                    return std::nullopt;
                }
                auto table = std::make_shared<std::map<Key, int>>();
                for (const Pair& pair : train) {
                    std::vector<Key> keys = features(pair.in);
                    for (size_t i = 0; i < keys.size(); ++i) {
                        auto [it, inserted] = table->emplace(keys[i], pair.out.p[i]);
                        if (!inserted && it->second != pair.out.p[i]) {
                            return std::nullopt;
                        }
                    }
                }
                size_t pixels = 0;
                for (const Pair& pair : train) pixels += pair.out.p.size();
                if (table->size() * 8 > pixels) {
                    return std::nullopt;
                }
                return Predictor([features, table](const G& in) -> std::optional<G> {
                    G out(in.w, in.h);
                    std::vector<Key> keys = features(in);
                    for (size_t i = 0; i < keys.size(); ++i) {
                        auto it = table->find(keys[i]);
                        if (it == table->end()) {
                            return std::nullopt;
                        }
                        out.p[i] = it->second;
                    }
                    return out;
                });
            } };
}

std::vector<Learner> lookupLearners()
{
    std::vector<Learner> ret;
    auto perPixel = [](auto fn) {
        return [fn](const G& g) {
            const int bg = background(g);
            std::vector<Key> keys;
            keys.reserve(g.p.size());
            for (int y = 0; y < g.h; ++y) {
                for (int x = 0; x < g.w; ++x) {
                    keys.push_back(fn(g, bg, x, y));
                }
            }
            return keys;
        };
    };
    auto nb = [](const G& g, int x, int y) { return g.inRange(x, y) ? g.at(x, y) : -1; };

    ret.push_back(lookupLearner("pixel color map", 2.0, perPixel([](const G& g, int, int x, int y) { return Key { g.at(x, y) }; })));
    ret.push_back(lookupLearner("color + enclosed", 3.0, [](const G& g) {
        const int bg    = background(g);
        const auto mask = enclosedMask(g, bg);
        std::vector<Key> keys;
        for (size_t i = 0; i < g.p.size(); ++i) {
            keys.push_back({ g.p[i], mask[i] });
        }
        return keys;
    }));
    ret.push_back(lookupLearner("color + between same colors", 3.5, perPixel([](const G& g, int bg, int x, int y) {
        const int l = ray(g, bg, x, y, -1, 0), r = ray(g, bg, x, y, 1, 0);
        const int u = ray(g, bg, x, y, 0, -1), d = ray(g, bg, x, y, 0, 1);
        return Key { g.at(x, y), l == r ? l : -1, u == d ? u : -1 };
    })));
    ret.push_back(lookupLearner("color + object size rank", 3.5, [](const G& g) {
        const int bg = background(g);
        auto objs    = objects(g, bg, true, false);
        size_t minSize = 1 << 20, maxSize = 0;
        for (auto& o : objs) {
            minSize = std::min(minSize, o.pixels.size());
            maxSize = std::max(maxSize, o.pixels.size());
        }
        std::vector<Key> keys(g.p.size());
        for (size_t i = 0; i < g.p.size(); ++i) {
            keys[i] = { g.p[i], 0, 0 };
        }
        for (auto& o : objs) {
            for (auto [x, y] : o.pixels) {
                keys[y * g.w + x] = { g.at(x, y), o.pixels.size() == maxSize ? 1 : 0, o.pixels.size() == minSize ? 1 : 0 };
            }
        }
        return keys;
    }));
    ret.push_back(lookupLearner("color + object size", 4.0, [](const G& g) {
        const int bg = background(g);
        std::vector<Key> keys(g.p.size());
        for (size_t i = 0; i < g.p.size(); ++i) {
            keys[i] = { g.p[i], 0 };
        }
        for (auto& o : objects(g, bg, true, false)) {
            for (auto [x, y] : o.pixels) {
                keys[y * g.w + x] = { g.at(x, y), static_cast<int>(o.pixels.size()) };
            }
        }
        return keys;
    }));
    ret.push_back(lookupLearner("color + 4 neighbours", 4.5, perPixel([nb](const G& g, int, int x, int y) {
        return Key { g.at(x, y), nb(g, x, y - 1), nb(g, x, y + 1), nb(g, x - 1, y), nb(g, x + 1, y) };
    })));
    ret.push_back(lookupLearner("color + rays", 5.0, perPixel([](const G& g, int bg, int x, int y) {
        return Key { g.at(x, y), ray(g, bg, x, y, -1, 0), ray(g, bg, x, y, 1, 0), ray(g, bg, x, y, 0, -1), ray(g, bg, x, y, 0, 1) };
    })));
    ret.push_back(lookupLearner("color + 8 neighbours", 5.5, perPixel([nb](const G& g, int, int x, int y) {
        Key k { g.at(x, y) };
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx || dy) {
                    k.push_back(nb(g, x + dx, y + dy));
                }
            }
        }
        return k;
    })));
    return ret;
}

std::optional<std::map<int, int>> learnColorMap(const std::vector<std::pair<G, G>>& pairs)
{
    std::map<int, int> map;
    for (auto& [in, out] : pairs) {
        if (in.w != out.w || in.h != out.h) {
            return std::nullopt;
        }
        for (size_t i = 0; i < in.p.size(); ++i) {
            auto [it, inserted] = map.emplace(in.p[i], out.p[i]);
            if (!inserted && it->second != out.p[i]) {
                return std::nullopt;
            }
        }
    }
    return map;
}

G applyColorMap(G g, const std::map<int, int>& map)
{
    for (int& c : g.p) {
        auto it = map.find(c);
        if (it != map.end()) {
            c = it->second;
        }
    }
    return g;
}

std::vector<Learner> symmetryLearners()
{
    std::vector<Learner> ret;
    for (int k = 1; k < 8; ++k) {
        ret.push_back({ std::string(dihedralName(k)), 1.0, [k](const std::vector<Pair>& train) -> std::optional<Predictor> {
                           for (auto& p : train) {
                               if (dihedral(p.in, k) != p.out) {
                                   return std::nullopt;
                               }
                           }
                           return Predictor([k](const G& in) -> std::optional<G> { return dihedral(in, k); });
                       } });
    }
    for (int k = 0; k < 8; ++k) {
        ret.push_back({ std::string(dihedralName(k)) + " + color map", 2.5, [k](const std::vector<Pair>& train) -> std::optional<Predictor> {
                           std::vector<std::pair<G, G>> pairs;
                           for (auto& p : train) {
                               pairs.push_back({ dihedral(p.in, k), p.out });
                           }
                           auto map = learnColorMap(pairs);
                           if (!map) {
                               return std::nullopt;
                           }
                           return Predictor([k, map = *map](const G& in) -> std::optional<G> { return applyColorMap(dihedral(in, k), map); });
                       } });
    }
    return ret;
}

std::optional<std::pair<int, int>> upRatio(const std::vector<Pair>& train)
{
    std::optional<std::pair<int, int>> r;
    for (auto& p : train) {
        if (p.out.w % p.in.w || p.out.h % p.in.h) {
            return std::nullopt;
        }
        std::pair<int, int> cur { p.out.w / p.in.w, p.out.h / p.in.h };
        if (r && *r != cur) {
            return std::nullopt;
        }
        r = cur;
    }
    if (r && r->first == 1 && r->second == 1) {
        return std::nullopt;
    }
    return r;
}

std::optional<std::pair<int, int>> downRatio(const std::vector<Pair>& train)
{
    std::optional<std::pair<int, int>> r;
    for (auto& p : train) {
        if (p.in.w % p.out.w || p.in.h % p.out.h) {
            return std::nullopt;
        }
        std::pair<int, int> cur { p.in.w / p.out.w, p.in.h / p.out.h };
        if (r && *r != cur) {
            return std::nullopt;
        }
        r = cur;
    }
    if (r && r->first == 1 && r->second == 1) {
        return std::nullopt;
    }
    return r;
}

G scaleUp(const G& in, int fx, int fy)
{
    G out(in.w * fx, in.h * fy);
    for (int y = 0; y < out.h; ++y) {
        for (int x = 0; x < out.w; ++x) {
            out.at(x, y) = in.at(x / fx, y / fy);
        }
    }
    return out;
}

struct Regions
{
    int wall = -1;
    int count = 0;
    std::vector<int> region;
    std::vector<int> layer;
};

Regions regionsAndLayers(const G& g, bool diagonal)
{
    Regions r;
    std::map<int, int> hist;
    for (int c : g.p) ++hist[c];
    if (hist.size() < 2) return r;
    std::vector<std::pair<int, int>> byCount;
    for (auto& [c, n] : hist) byCount.push_back({ -n, c });
    std::sort(byCount.begin(), byCount.end());
    r.wall = byCount[1].second;

    const int n = g.w * g.h;
    r.region.assign(n, -1);
    for (int start = 0; start < n; ++start) {
        if (r.region[start] >= 0 || g.p[start] == r.wall) continue;
        std::vector<int> stack { start };
        r.region[start] = r.count;
        while (!stack.empty()) {
            const int i = stack.back(), x = i % g.w, y = i / g.w;
            stack.pop_back();
            const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (auto& dd : d) {
                const int nx = x + dd[0], ny = y + dd[1];
                if (!g.inRange(nx, ny)) continue;
                const int k = ny * g.w + nx;
                if (r.region[k] >= 0 || g.p[k] == r.wall) continue;
                r.region[k] = r.count;
                stack.push_back(k);
            }
        }
        ++r.count;
    }

    r.layer.assign(n, -1);
    std::vector<char> left(n);
    for (int i = 0; i < n; ++i) left[i] = r.region[i] >= 0;
    for (int t = 0;; ++t) {
        std::vector<int> border;
        for (int i = 0; i < n; ++i) {
            if (!left[i]) continue;
            const int x = i % g.w, y = i / g.w;
            bool edge = false;
            for (int dy = -1; dy <= 1 && !edge; ++dy)
                for (int dx = -1; dx <= 1 && !edge; ++dx) {
                    if ((!dx && !dy) || (!diagonal && dx && dy)) continue;
                    const int nx = x + dx, ny = y + dy;
                    edge = !g.inRange(nx, ny) || !left[ny * g.w + nx] || r.region[ny * g.w + nx] != r.region[i];
                }
            if (edge) border.push_back(i);
        }
        if (border.empty()) break;
        for (int i : border) {
            r.layer[i] = t;
            left[i]    = 0;
        }
    }
    return r;
}

std::optional<G> layersFromSeeds(const G& in, bool diagonal)
{
    const Regions r = regionsAndLayers(in, diagonal);
    if (r.count == 0) return std::nullopt;
    std::vector<std::map<int, int>> colors(r.count);
    for (size_t i = 0; i < in.p.size(); ++i)
        if (r.region[i] >= 0) ++colors[r.region[i]][in.p[i]];
    std::vector<int> base(r.count);
    for (int reg = 0; reg < r.count; ++reg)
        base[reg] = std::max_element(colors[reg].begin(), colors[reg].end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
    std::vector<std::map<int, int>> seeds(r.count);
    for (size_t i = 0; i < in.p.size(); ++i) {
        const int reg = r.region[i];
        if (reg < 0 || in.p[i] == base[reg]) continue;
        auto [it, inserted] = seeds[reg].emplace(r.layer[i], in.p[i]);
        if (!inserted && it->second != in.p[i]) return std::nullopt;
    }
    G out = in;
    for (size_t i = 0; i < in.p.size(); ++i) {
        const int reg = r.region[i];
        if (reg < 0 || seeds[reg].empty()) continue;
        const int period = seeds[reg].rbegin()->first + 1;
        auto seed        = seeds[reg].find(r.layer[i] % period);
        out.p[i]         = seed != seeds[reg].end() ? seed->second : base[reg];
    }
    return out;
}

std::vector<Learner> structuralLearners()
{
    std::vector<Learner> ret;
    ret.push_back({ "scale up", 2.0, [](const std::vector<Pair>& train) -> std::optional<Predictor> {
                       auto r = upRatio(train);
                       if (!r) {
                           return std::nullopt;
                       }
                       for (auto& p : train) {
                           if (scaleUp(p.in, r->first, r->second) != p.out) {
                               return std::nullopt;
                           }
                       }
                       return Predictor([r = *r](const G& in) -> std::optional<G> { return scaleUp(in, r.first, r.second); });
                   } });

    ret.push_back({ "tile with symmetries", 3.0, [](const std::vector<Pair>& train) -> std::optional<Predictor> {
                       auto r = upRatio(train);
                       if (!r) {
                           return std::nullopt;
                       }
                       const int tx = r->first, ty = r->second;
                       std::vector<int> tileK(tx * ty, -1);
                       for (int j = 0; j < ty; ++j) {
                           for (int i = 0; i < tx; ++i) {
                               for (int k = 0; k < 8 && tileK[j * tx + i] < 0; ++k) {
                                   bool ok = true;
                                   for (auto& p : train) {
                                       const G t = dihedral(p.in, k);
                                       if (t.w != p.in.w || t.h != p.in.h) {
                                           ok = false;
                                           break;
                                       }
                                       for (int y = 0; y < p.in.h && ok; ++y) {
                                           for (int x = 0; x < p.in.w && ok; ++x) {
                                               ok = p.out.at(i * p.in.w + x, j * p.in.h + y) == t.at(x, y);
                                           }
                                       }
                                   }
                                   if (ok) {
                                       tileK[j * tx + i] = k;
                                   }
                               }
                               if (tileK[j * tx + i] < 0) {
                                   return std::nullopt;
                               }
                           }
                       }
                       return Predictor([tx, ty, tileK](const G& in) -> std::optional<G> {
                           G out(in.w * tx, in.h * ty);
                           for (int j = 0; j < ty; ++j) {
                               for (int i = 0; i < tx; ++i) {
                                   const G t = dihedral(in, tileK[j * tx + i]);
                                   if (t.w != in.w || t.h != in.h) {
                                       return std::nullopt;
                                   }
                                   for (int y = 0; y < in.h; ++y) {
                                       for (int x = 0; x < in.w; ++x) {
                                           out.at(i * in.w + x, j * in.h + y) = t.at(x, y);
                                       }
                                   }
                               }
                           }
                           return out;
                       });
                   } });

    for (int invert = 0; invert < 2; ++invert) {
        ret.push_back({ invert ? "self-similar (inverted)" : "self-similar", 3.0, [invert](const std::vector<Pair>& train) -> std::optional<Predictor> {
                           auto make = [invert](const G& in) -> std::optional<G> {
                               const int bg = std::count(in.p.begin(), in.p.end(), 0) ? 0 : background(in);
                               G out(in.w * in.w, in.h * in.h, bg);
                               for (int j = 0; j < in.h; ++j) {
                                   for (int i = 0; i < in.w; ++i) {
                                       if ((in.at(i, j) != bg) == (invert == 1)) {
                                           continue;
                                       }
                                       for (int y = 0; y < in.h; ++y) {
                                           for (int x = 0; x < in.w; ++x) {
                                               out.at(i * in.w + x, j * in.h + y) = in.at(x, y);
                                           }
                                       }
                                   }
                               }
                               return out;
                           };
                           for (auto& p : train) {
                               if (make(p.in) != p.out) {
                                   return std::nullopt;
                               }
                           }
                           return Predictor(make);
                       } });
    }

    for (int diagonal = 0; diagonal < 2; ++diagonal) {
        ret.push_back({ diagonal ? "layers of each region, colored by its seeds (8-neighbour peel)" : "layers of each region, colored by its seeds", 3.0,
                        [diagonal](const std::vector<Pair>& train) -> std::optional<Predictor> {
                            auto make = [diagonal](const G& in) { return layersFromSeeds(in, diagonal == 1); };
                            for (auto& p : train) {
                                auto out = make(p.in);
                                if (!out || *out != p.out) {
                                    return std::nullopt;
                                }
                            }
                            return Predictor(make);
                        } });
    }

    for (int mode = 0; mode < 2; ++mode) {
        ret.push_back({ mode ? "down-scale (any foreground)" : "down-scale (block majority)", 2.5, [mode](const std::vector<Pair>& train) -> std::optional<Predictor> {
                           auto r = downRatio(train);
                           if (!r) {
                               return std::nullopt;
                           }
                           auto make = [mode, fx = r->first, fy = r->second](const G& in) -> std::optional<G> {
                               if (in.w % fx || in.h % fy) {
                                   return std::nullopt;
                               }
                               const int bg = background(in);
                               G out(in.w / fx, in.h / fy, bg);
                               for (int y = 0; y < out.h; ++y) {
                                   for (int x = 0; x < out.w; ++x) {
                                       std::map<int, int> hist;
                                       for (int by = 0; by < fy; ++by) {
                                           for (int bx = 0; bx < fx; ++bx) {
                                               ++hist[in.at(x * fx + bx, y * fy + by)];
                                           }
                                       }
                                       if (mode == 0) {
                                           out.at(x, y) = std::max_element(hist.begin(), hist.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
                                       } else {
                                           hist.erase(bg);
                                           if (!hist.empty()) {
                                               out.at(x, y) = std::max_element(hist.begin(), hist.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
                                           }
                                       }
                                   }
                               }
                               return out;
                           };
                           for (auto& p : train) {
                               if (make(p.in) != p.out) {
                                   return std::nullopt;
                               }
                           }
                           return Predictor(make);
                       } });
    }
    return ret;
}

std::optional<std::vector<Box>> cellBoxes(const G& g)
{
    for (int sep = 0; sep < 10; ++sep) {
        std::vector<int> cutRows { -1 }, cutCols { -1 };
        for (int y = 0; y < g.h; ++y) {
            bool full = true;
            for (int x = 0; x < g.w && full; ++x) full = g.at(x, y) == sep;
            if (full) cutRows.push_back(y);
        }
        for (int x = 0; x < g.w; ++x) {
            bool full = true;
            for (int y = 0; y < g.h && full; ++y) full = g.at(x, y) == sep;
            if (full) cutCols.push_back(x);
        }
        if (cutRows.size() + cutCols.size() <= 2) continue;
        cutRows.push_back(g.h);
        cutCols.push_back(g.w);
        std::vector<Box> boxes;
        for (size_t r = 0; r + 1 < cutRows.size(); ++r)
            for (size_t c = 0; c + 1 < cutCols.size(); ++c)
                if (cutRows[r + 1] - cutRows[r] > 1 && cutCols[c + 1] - cutCols[c] > 1)
                    boxes.push_back({ cutCols[c] + 1, cutRows[r] + 1, cutCols[c + 1] - 1, cutRows[r + 1] - 1 });
        if (boxes.size() >= 2) return boxes;
    }
    return std::nullopt;
}

using Selector = std::function<std::optional<Box>(const G&)>;

std::vector<std::pair<std::string, Selector>> objectSelectors()
{
    std::vector<std::pair<std::string, Selector>> selectors;

    selectors.push_back({ "all foreground", [](const G& g) -> std::optional<Box> {
                             auto objs = objects(g, background(g), false, true);
                             if (objs.empty()) {
                                 return std::nullopt;
                             }
                             Box b = objs[0].box;
                             for (auto& o : objs) {
                                 b = { std::min(b.x0, o.box.x0), std::min(b.y0, o.box.y0), std::max(b.x1, o.box.x1), std::max(b.y1, o.box.y1) };
                             }
                             return b;
                         } });

    auto extreme = [](bool sameColor, std::function<double(const G&, const Object&)> measure, bool largest) {
        return [=](const G& g) -> std::optional<Box> {
            auto objs = objects(g, background(g), sameColor, !sameColor);
            if (objs.empty()) {
                return std::nullopt;
            }
            std::vector<double> m;
            for (auto& o : objs) {
                m.push_back(largest ? measure(g, o) : -measure(g, o));
            }
            const double best = *std::max_element(m.begin(), m.end());
            if (std::count(m.begin(), m.end(), best) != 1) {
                return std::nullopt;
            }
            return objs[std::max_element(m.begin(), m.end()) - m.begin()].box;
        };
    };
    auto pixelCount = [](const G&, const Object& o) { return double(o.pixels.size()); };
    auto boxArea    = [](const G&, const Object& o) { return double(o.box.w() * o.box.h()); };
    auto colorCount = [](const G&, const Object& o) { return double(o.colors.size()); };
    auto rarity     = [](const G& g, const Object& o) {
        auto hist = histogram(g);
        return -double(hist[*o.colors.begin()]);
    };
    for (int sameColor = 0; sameColor < 2; ++sameColor) {
        const std::string conn = sameColor ? " (single color)" : " (multi color)";
        selectors.push_back({ "largest object" + conn, extreme(sameColor, pixelCount, true) });
        selectors.push_back({ "smallest object" + conn, extreme(sameColor, pixelCount, false) });
        selectors.push_back({ "largest box" + conn, extreme(sameColor, boxArea, true) });
        selectors.push_back({ "most colorful object" + conn, extreme(sameColor, colorCount, true) });
    }
    selectors.push_back({ "rarest color object", extreme(true, rarity, true) });

    using Regions = std::function<std::vector<Box>(const G&)>;
    const std::vector<std::pair<std::string, Regions>> regionLists {
        { "object", [](const G& g) { std::vector<Box> r; for (auto& o : objects(g, background(g), false, true)) r.push_back(o.box); return r; } },
        { "single color object", [](const G& g) { std::vector<Box> r; for (auto& o : objects(g, background(g), true, false)) r.push_back(o.box); return r; } },
        { "cell", [](const G& g) { return cellBoxes(g).value_or(std::vector<Box> {}); } },
    };
    using Measure = std::function<int(const G&)>;
    auto fgCount  = [](const G& c) { const int bg = background(c); return int(std::count_if(c.p.begin(), c.p.end(), [bg](int v) { return v != bg; })); };
    auto colors   = [](const G& c) { return int(histogram(c).size()); };
    auto symmetric = [](const G& c) { return int(dihedral(c, 4) == c || dihedral(c, 5) == c || (c.w == c.h && dihedral(c, 6) == c)); };
    const std::vector<std::pair<std::string, Measure>> measures { { "most pixels", fgCount }, { "most colors", colors }, { "symmetric", symmetric } };
    for (auto& [regionName, regions] : regionLists) {
        for (auto& [measureName, measure] : measures) {
            for (int sign = 1; sign >= -1; sign -= 2) {
                const std::string label = (sign > 0 ? "" : "not ") + measureName;
                selectors.push_back({ "the " + regionName + " with " + label, [regions, measure, sign](const G& g) -> std::optional<Box> {
                                         auto boxes = regions(g);
                                         if (boxes.size() < 2) return std::nullopt;
                                         std::vector<int> m;
                                         for (auto& b : boxes) m.push_back(sign * measure(crop(g, b)));
                                         const int best = *std::max_element(m.begin(), m.end());
                                         if (std::count(m.begin(), m.end(), best) != 1) return std::nullopt;
                                         return boxes[std::max_element(m.begin(), m.end()) - m.begin()];
                                     } });
            }
        }
        selectors.push_back({ "the unique " + regionName, [regions](const G& g) -> std::optional<Box> {
                                 auto boxes = regions(g);
                                 if (boxes.size() < 3) return std::nullopt;
                                 std::vector<G> contents;
                                 for (auto& b : boxes) contents.push_back(crop(g, b));
                                 std::optional<Box> found;
                                 for (size_t i = 0; i < boxes.size(); ++i) {
                                     if (std::count(contents.begin(), contents.end(), contents[i]) == 1) {
                                         if (found) return std::nullopt;
                                         found = boxes[i];
                                     }
                                 }
                                 return found;
                             } });
    }

    for (int color = 0; color < 10; ++color) {
        const std::string cname = getArcColorName(static_cast<ColorId>(color));
        selectors.push_back({ "object marked by " + cname, [color](const G& g) -> std::optional<Box> {
                                 std::optional<Box> found;
                                 for (auto& o : objects(g, background(g), false, true)) {
                                     if (!o.colors.count(color) || o.colors.size() < 2) continue;
                                     if (found) return std::nullopt;
                                     Box b { 1 << 20, 1 << 20, -1, -1 };
                                     for (auto [x, y] : o.pixels)
                                         if (g.at(x, y) != color) b = { std::min(b.x0, x), std::min(b.y0, y), std::max(b.x1, x), std::max(b.y1, y) };
                                     found = b;
                                 }
                                 return found;
                             } });
        selectors.push_back({ "object with most " + cname, [color](const G& g) -> std::optional<Box> {
                                 auto objs = objects(g, background(g), false, true);
                                 std::vector<int> m;
                                 for (auto& o : objs) m.push_back(int(std::count_if(o.pixels.begin(), o.pixels.end(), [&](auto& px) { return g.at(px.first, px.second) == color; })));
                                 if (objs.size() < 2) return std::nullopt;
                                 const int best = *std::max_element(m.begin(), m.end());
                                 if (best == 0 || std::count(m.begin(), m.end(), best) != 1) return std::nullopt;
                                 return objs[std::max_element(m.begin(), m.end()) - m.begin()].box;
                             } });
    }

    for (int color = 0; color < 10; ++color) {
        for (int inner = 0; inner < 2; ++inner) {
            selectors.push_back({ std::string(inner ? "inside " : "") + "box of color " + getArcColorName(static_cast<ColorId>(color)),
                [color, inner](const G& g) -> std::optional<Box> {
                    Box b { 1 << 20, 1 << 20, -1, -1 };
                    for (int y = 0; y < g.h; ++y) {
                        for (int x = 0; x < g.w; ++x) {
                            if (g.at(x, y) == color) {
                                b = { std::min(b.x0, x), std::min(b.y0, y), std::max(b.x1, x), std::max(b.y1, y) };
                            }
                        }
                    }
                    if (b.x1 < 0 || (b.x0 == 0 && b.y0 == 0 && b.x1 == g.w - 1 && b.y1 == g.h - 1)) {
                        return std::nullopt;
                    }
                    if (inner) {
                        b = { b.x0 + 1, b.y0 + 1, b.x1 - 1, b.y1 - 1 };
                        if (b.x1 < b.x0 || b.y1 < b.y0) {
                            return std::nullopt;
                        }
                    }
                    return b;
                } });
        }
    }
    return selectors;
}

std::vector<Learner> cropLearners()
{
    std::vector<Learner> ret;
    for (auto& [name, select] : objectSelectors()) {
        for (int k = 0; k < 8; ++k) {
            ret.push_back({ "crop " + name + (k ? std::string(" + ") + dihedralName(k) : ""), k ? 4.0 : 3.0,
                [select, k](const std::vector<Pair>& train) -> std::optional<Predictor> {
                    auto make = [select, k](const G& in) -> std::optional<G> {
                        auto box = select(in);
                        if (!box) {
                            return std::nullopt;
                        }
                        return dihedral(crop(in, *box), k);
                    };
                    for (auto& p : train) {
                        auto out = make(p.in);
                        if (!out || *out != p.out) {
                            return std::nullopt;
                        }
                    }
                    return Predictor(make);
                } });
        }
    }
    return ret;
}

std::optional<std::vector<G>> splitParts(const G& g, int outW, int outH)
{
    auto fullRow = [&](int y) { for (int x = 1; x < g.w; ++x) if (g.at(x, y) != g.at(0, y)) return false; return true; };
    auto fullCol = [&](int x) { for (int y = 1; y < g.h; ++y) if (g.at(x, y) != g.at(x, 0)) return false; return true; };

    for (int sep = 0; sep <= 1; ++sep) {
        if (g.h == outH) {
            const int n = (g.w + sep) / (outW + sep);
            bool ok     = n >= 2 && n * outW + (n - 1) * sep == g.w;
            for (int i = 1; i < n && ok && sep; ++i) {
                ok = fullCol(i * (outW + 1) - 1);
            }
            if (ok) {
                std::vector<G> parts;
                for (int i = 0; i < n; ++i) {
                    parts.push_back(crop(g, { i * (outW + sep), 0, i * (outW + sep) + outW - 1, g.h - 1 }));
                }
                return parts;
            }
        }
        if (g.w == outW) {
            const int n = (g.h + sep) / (outH + sep);
            bool ok     = n >= 2 && n * outH + (n - 1) * sep == g.h;
            for (int i = 1; i < n && ok && sep; ++i) {
                ok = fullRow(i * (outH + 1) - 1);
            }
            if (ok) {
                std::vector<G> parts;
                for (int i = 0; i < n; ++i) {
                    parts.push_back(crop(g, { 0, i * (outH + sep), g.w - 1, i * (outH + sep) + outH - 1 }));
                }
                return parts;
            }
        }
    }
    return std::nullopt;
}

Learner splitLearner()
{
    return { "split into parts + combine", 4.0, [](const std::vector<Pair>& train) -> std::optional<Predictor> {
                const int ow = train[0].out.w, oh = train[0].out.h;
                for (auto& p : train) {
                    if (p.out.w != ow || p.out.h != oh) {
                        return std::nullopt;
                    }
                }
                auto table = std::make_shared<std::map<Key, int>>();
                for (auto& p : train) {
                    auto parts = splitParts(p.in, ow, oh);
                    if (!parts) {
                        return std::nullopt;
                    }
                    for (int i = 0; i < ow * oh; ++i) {
                        Key k;
                        for (auto& part : *parts) {
                            k.push_back(part.p[i]);
                        }
                        auto [it, inserted] = table->emplace(k, p.out.p[i]);
                        if (!inserted && it->second != p.out.p[i]) {
                            return std::nullopt;
                        }
                    }
                }
                return Predictor([table, ow, oh](const G& in) -> std::optional<G> {
                    auto parts = splitParts(in, ow, oh);
                    if (!parts) {
                        return std::nullopt;
                    }
                    G out(ow, oh);
                    for (int i = 0; i < ow * oh; ++i) {
                        Key k;
                        for (auto& part : *parts) {
                            k.push_back(part.p[i]);
                        }
                        auto it = table->find(k);
                        if (it == table->end()) {
                            return std::nullopt;
                        }
                        out.p[i] = it->second;
                    }
                    return out;
                });
            } };
}

std::vector<Learner> symmetryCompletionLearners()
{
    std::vector<Learner> ret;
    const std::vector<std::pair<std::string, std::vector<int>>> groups {
        { "mirrorH", { 4 } }, { "mirrorV", { 5 } }, { "mirrorH+V", { 4, 5, 2 } }, { "transpose", { 6 } }, { "D4", { 1, 2, 3, 4, 5, 6, 7 } }
    };
    for (auto& [name, ks] : groups) {
        ret.push_back({ "complete symmetry " + name, 4.0, [ks](const std::vector<Pair>& train) -> std::optional<Predictor> {
                           if (!sameSizes(train)) {
                               return std::nullopt;
                           }
                           std::set<int> noise;
                           for (auto& p : train) {
                               for (size_t i = 0; i < p.in.p.size(); ++i) {
                                   if (p.in.p[i] != p.out.p[i]) {
                                       noise.insert(p.in.p[i]);
                                   }
                               }
                           }
                           if (noise.size() != 1) {
                               return std::nullopt;
                           }
                           auto make = [ks, n = *noise.begin()](const G& in) -> std::optional<G> {
                               G out = in;
                               for (int y = 0; y < in.h; ++y) {
                                   for (int x = 0; x < in.w; ++x) {
                                       if (in.at(x, y) != n) {
                                           continue;
                                       }
                                       for (int k : ks) {
                                           int sx = x, sy = y;
                                           switch (k) {
                                           case 1: sx = in.w - 1 - y; sy = x; break;
                                           case 2: sx = in.w - 1 - x; sy = in.h - 1 - y; break;
                                           case 3: sx = y; sy = in.h - 1 - x; break;
                                           case 4: sx = in.w - 1 - x; break;
                                           case 5: sy = in.h - 1 - y; break;
                                           case 6: sx = y; sy = x; break;
                                           case 7: sx = in.w - 1 - y; sy = in.h - 1 - x; break;
                                           }
                                           if (in.inRange(sx, sy) && in.at(sx, sy) != n) {
                                               out.at(x, y) = in.at(sx, sy);
                                               break;
                                           }
                                       }
                                       if (out.at(x, y) == n) {
                                           return std::nullopt;
                                       }
                                   }
                               }
                               return out;
                           };
                           for (auto& p : train) {
                               auto out = make(p.in);
                               if (!out || *out != p.out) {
                                   return std::nullopt;
                               }
                           }
                           return Predictor(make);
                       } });
    }
    return ret;
}

Learner stampLearner()
{
    return { "local consequence rules", 3.0, [](const std::vector<Pair>& train) -> std::optional<Predictor> {
                auto toNative = [](const G& g) {
                    auto grid = std::make_shared<native::Grid>("g");
                    grid->loadFromVector(g.w, g.h, g.p);
                    return grid;
                };
                std::vector<std::shared_ptr<native::Grid>> keep;
                std::vector<RuleInduction::Pair> pairs;
                for (auto& p : train) {
                    keep.push_back(toNative(p.in));
                    keep.push_back(toNative(p.out));
                    pairs.push_back({ *keep[keep.size() - 2], *keep.back() });
                }
                auto induction = std::make_shared<RuleInduction>();
                if (!induction->learn(pairs)) {
                    return std::nullopt;
                }
                return Predictor([induction, toNative](const G& in) -> std::optional<G> {
                    auto grid   = toNative(in);
                    auto pixels = induction->apply(*grid);
                    if (!pixels) {
                        return std::nullopt;
                    }
                    G out(in.w, in.h);
                    out.p = *pixels;
                    return out;
                });
            } };
}

std::vector<Learner> gravityLearners()
{
    std::vector<Learner> ret;
    const char* names[] = { "gravity down", "gravity up", "gravity right", "gravity left" };
    for (int dir = 0; dir < 4; ++dir) {
        ret.push_back({ names[dir], 2.5, [dir](const std::vector<Pair>& train) -> std::optional<Predictor> {
                           auto make = [dir](const G& in) -> std::optional<G> {
                               const int bg     = background(in);
                               const bool vert  = dir < 2;
                               const int lines  = vert ? in.w : in.h;
                               const int length = vert ? in.h : in.w;
                               G out(in.w, in.h, bg);
                               for (int l = 0; l < lines; ++l) {
                                   std::vector<int> fg;
                                   for (int i = 0; i < length; ++i) {
                                       const int c = vert ? in.at(l, i) : in.at(i, l);
                                       if (c != bg) {
                                           fg.push_back(c);
                                       }
                                   }
                                   const bool toEnd = (dir == 0 || dir == 2);
                                   for (size_t j = 0; j < fg.size(); ++j) {
                                       const int i = toEnd ? length - static_cast<int>(fg.size()) + static_cast<int>(j) : static_cast<int>(j);
                                       (vert ? out.at(l, i) : out.at(i, l)) = fg[j];
                                   }
                               }
                               return out;
                           };
                           for (auto& p : train) {
                               if (make(p.in) != p.out) {
                                   return std::nullopt;
                               }
                           }
                           return Predictor(make);
                       } });
    }
    return ret;
}

std::optional<G> completePeriodic(const G& in, int noise)
{
    std::vector<std::pair<int, int>> periods;
    for (int py = 1; py <= in.h; ++py) {
        for (int px = 1; px <= in.w; ++px) {
            if (px < in.w || py < in.h) {
                periods.push_back({ px, py });
            }
        }
    }
    std::stable_sort(periods.begin(), periods.end(), [](auto& a, auto& b) { return a.first * a.second < b.first * b.second; });
    for (auto [px, py] : periods) {
        std::vector<int> cls(px * py, -1);
        bool ok = true;
        for (int y = 0; y < in.h && ok; ++y) {
            for (int x = 0; x < in.w && ok; ++x) {
                const int c = in.at(x, y);
                if (c == noise) continue;
                int& v = cls[(y % py) * px + (x % px)];
                if (v < 0) v = c;
                else ok = v == c;
            }
        }
        if (!ok) continue;
        G out = in;
        for (int y = 0; y < in.h && ok; ++y) {
            for (int x = 0; x < in.w && ok; ++x) {
                if (in.at(x, y) == noise) {
                    const int v = cls[(y % py) * px + (x % px)];
                    ok          = v >= 0;
                    out.at(x, y) = v;
                }
            }
        }
        if (ok) return out;
    }
    return std::nullopt;
}

std::vector<Learner> periodicLearners()
{
    std::vector<Learner> ret;
    for (int cropToNoise = 0; cropToNoise < 2; ++cropToNoise) {
        ret.push_back({ cropToNoise ? "complete periodic pattern, show the hidden part" : "complete periodic pattern", 3.5,
            [cropToNoise](const std::vector<Pair>& train) -> std::optional<Predictor> {
                std::set<int> candidates;
                for (int c = 0; c < 10; ++c) candidates.insert(c);
                for (auto& p : train) {
                    std::set<int> keep;
                    auto hi = histogram(p.in), ho = histogram(p.out);
                    for (int c : candidates) {
                        if (hi.count(c) && (cropToNoise || !ho.count(c))) keep.insert(c);
                    }
                    candidates = keep;
                }
                for (int noise : candidates) {
                    auto make = [noise, cropToNoise](const G& in) -> std::optional<G> {
                        auto full = completePeriodic(in, noise);
                        if (!full || !cropToNoise) return full;
                        Box b { 1 << 20, 1 << 20, -1, -1 };
                        for (int y = 0; y < in.h; ++y)
                            for (int x = 0; x < in.w; ++x)
                                if (in.at(x, y) == noise) b = { std::min(b.x0, x), std::min(b.y0, y), std::max(b.x1, x), std::max(b.y1, y) };
                        if (b.x1 < 0) return std::nullopt;
                        return crop(*full, b);
                    };
                    bool ok = std::all_of(train.begin(), train.end(), [&](const Pair& p) { auto o = make(p.in); return o && *o == p.out; });
                    if (ok) return Predictor(make);
                }
                return std::nullopt;
            } });
    }
    return ret;
}

std::optional<G> objectGravity(const G& in, int dir, int fixedColor)
{
    const int bg   = background(in);
    const int dx[] = { 0, 0, 1, -1 }, dy[] = { 1, -1, 0, 0 };
    auto objs      = objects(in, bg, false, true);
    G out(in.w, in.h, bg);
    std::vector<const Object*> moving;
    for (auto& o : objs) {
        if (o.colors.count(fixedColor)) {
            for (auto [x, y] : o.pixels) out.at(x, y) = in.at(x, y);
        } else {
            moving.push_back(&o);
        }
    }
    auto lead = [&](const Object* o) {
        switch (dir) {
        case 0: return -o->box.y1;
        case 1: return o->box.y0;
        case 2: return -o->box.x1;
        default: return o->box.x0;
        }
    };
    std::sort(moving.begin(), moving.end(), [&](auto a, auto b) { return lead(a) < lead(b); });
    for (const Object* o : moving) {
        int step = 0;
        while (true) {
            bool free = true;
            for (auto [x, y] : o->pixels) {
                const int nx = x + dx[dir] * (step + 1), ny = y + dy[dir] * (step + 1);
                if (!in.inRange(nx, ny) || out.at(nx, ny) != bg) {
                    free = false;
                    break;
                }
            }
            if (!free) break;
            ++step;
        }
        for (auto [x, y] : o->pixels) out.at(x + dx[dir] * step, y + dy[dir] * step) = in.at(x, y);
    }
    return out;
}

std::vector<Learner> objectGravityLearners()
{
    std::vector<Learner> ret;
    const char* names[] = { "objects fall down", "objects fall up", "objects fall right", "objects fall left" };
    for (int dir = 0; dir < 4; ++dir) {
        ret.push_back({ names[dir], 3.0, [dir](const std::vector<Pair>& train) -> std::optional<Predictor> {
                           for (int fixed = -1; fixed < 10; ++fixed) {
                               auto make = [dir, fixed](const G& in) { return objectGravity(in, dir, fixed); };
                               bool ok   = std::all_of(train.begin(), train.end(), [&](const Pair& p) { auto o = make(p.in); return o && *o == p.out; });
                               if (ok) return Predictor(make);
                           }
                           return std::nullopt;
                       } });
    }
    return ret;
}

std::vector<Learner> classifierLearners()
{
    using GridFeature = std::function<Key(const G&)>;
    const std::vector<std::pair<std::string, GridFeature>> features {
        { "foreground shape", [](const G& g) { const int bg = background(g); Key k { g.w, g.h }; for (int c : g.p) k.push_back(c != bg); return k; } },
        { "object count", [](const G& g) { return Key { int(objects(g, background(g), false, true).size()) }; } },
        { "single color object count", [](const G& g) { return Key { int(objects(g, background(g), true, false).size()) }; } },
        { "color count", [](const G& g) { return Key { int(histogram(g).size()) }; } },
        { "symmetry", [](const G& g) { return Key { dihedral(g, 4) == g, dihedral(g, 5) == g, g.w == g.h && dihedral(g, 6) == g }; } },
    };
    std::vector<Learner> ret;
    for (auto& [name, feature] : features) {
        ret.push_back({ "classify by " + name, 3.5, [feature](const std::vector<Pair>& train) -> std::optional<Predictor> {
                           auto table = std::make_shared<std::map<Key, G>>();
                           std::map<Key, int> seen;
                           for (auto& p : train) {
                               const Key k = feature(p.in);
                               auto [it, inserted] = table->emplace(k, p.out);
                               if (!inserted && it->second != p.out) return std::nullopt;
                               ++seen[k];
                           }
                           if (table->size() < 2 || std::any_of(seen.begin(), seen.end(), [](auto& s) { return s.second < 2; })) return std::nullopt;
                           return Predictor([feature, table](const G& in) -> std::optional<G> {
                               auto it = table->find(feature(in));
                               if (it == table->end()) return std::nullopt;
                               return it->second;
                           });
                       } });
    }
    return ret;
}

std::optional<std::pair<int, int>> constantOutputSize(const std::vector<Pair>& train)
{
    for (auto& p : train) {
        if (p.out.w != train[0].out.w || p.out.h != train[0].out.h) return std::nullopt;
    }
    return std::make_pair(train[0].out.w, train[0].out.h);
}

std::optional<Box> cornerBox(const G& g, int corner, int w, int h)
{
    if (w > g.w || h > g.h) return std::nullopt;
    const int x0 = (corner == 1 || corner == 3) ? g.w - w : (corner == 4 ? (g.w - w) / 2 : 0);
    const int y0 = (corner == 2 || corner == 3) ? g.h - h : (corner == 4 ? (g.h - h) / 2 : 0);
    return Box { x0, y0, x0 + w - 1, y0 + h - 1 };
}

std::vector<Learner> smallAnswerLearners()
{
    std::vector<Learner> ret;
    const char* corners[] = { "top-left", "top-right", "bottom-left", "bottom-right", "center" };
    for (int corner = 0; corner < 5; ++corner) {
        ret.push_back({ std::string("crop ") + corners[corner] + " of fixed size", 2.5, [corner](const std::vector<Pair>& train) -> std::optional<Predictor> {
                           auto size = constantOutputSize(train);
                           if (!size) return std::nullopt;
                           auto make = [corner, size = *size](const G& in) -> std::optional<G> {
                               auto b = cornerBox(in, corner, size.first, size.second);
                               if (!b) return std::nullopt;
                               return crop(in, *b);
                           };
                           bool ok = std::all_of(train.begin(), train.end(), [&](const Pair& p) { auto o = make(p.in); return o && *o == p.out; });
                           return ok ? std::optional<Predictor>(make) : std::nullopt;
                       } });
    }

    ret.push_back({ "overlay the corners", 4.0, [](const std::vector<Pair>& train) -> std::optional<Predictor> {
                       auto size = constantOutputSize(train);
                       if (!size) return std::nullopt;
                       auto parts = [size = *size](const G& in) -> std::optional<std::vector<G>> {
                           std::vector<G> ret;
                           for (int c = 0; c < 4; ++c) {
                               auto b = cornerBox(in, c, size.first, size.second);
                               if (!b || size.first * 2 > in.w + 1 || size.second * 2 > in.h + 1) return std::nullopt;
                               ret.push_back(crop(in, *b));
                           }
                           return ret;
                       };
                       auto table = std::make_shared<std::map<Key, int>>();
                       for (auto& p : train) {
                           auto ps = parts(p.in);
                           if (!ps) return std::nullopt;
                           for (int i = 0; i < p.out.w * p.out.h; ++i) {
                               Key k;
                               for (auto& part : *ps) k.push_back(part.p[i]);
                               auto [it, inserted] = table->emplace(k, p.out.p[i]);
                               if (!inserted && it->second != p.out.p[i]) return std::nullopt;
                           }
                       }
                       return Predictor([parts, table, size = *size](const G& in) -> std::optional<G> {
                           auto ps = parts(in);
                           if (!ps) return std::nullopt;
                           G out(size.first, size.second);
                           for (int i = 0; i < out.w * out.h; ++i) {
                               Key k;
                               for (auto& part : *ps) k.push_back(part.p[i]);
                               auto it = table->find(k);
                               if (it == table->end()) return std::nullopt;
                               out.p[i] = it->second;
                           }
                           return out;
                       });
                   } });

    for (int sameColor = 0; sameColor < 2; ++sameColor) {
        for (int descending = 0; descending < 2; ++descending) {
            for (int asRow = 0; asRow < 2; ++asRow) {
                std::string name = std::string("object colors sorted by size") + (descending ? " (largest first)" : " (smallest first)") + (asRow ? " as row" : " as column") + (sameColor ? "" : ", multi color objects");
                ret.push_back({ name, 3.5, [sameColor, descending, asRow](const std::vector<Pair>& train) -> std::optional<Predictor> {
                                   auto make = [=](const G& in) -> std::optional<G> {
                                       auto objs = objects(in, background(in), sameColor, !sameColor);
                                       if (objs.empty()) return std::nullopt;
                                       std::stable_sort(objs.begin(), objs.end(), [&](auto& a, auto& b) { return descending ? a.pixels.size() > b.pixels.size() : a.pixels.size() < b.pixels.size(); });
                                       const int n = int(objs.size());
                                       G out(asRow ? n : 1, asRow ? 1 : n);
                                       for (int i = 0; i < n; ++i) {
                                           std::map<int, int> h;
                                           for (auto [x, y] : objs[i].pixels) ++h[in.at(x, y)];
                                           out.p[i] = std::max_element(h.begin(), h.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
                                       }
                                       return out;
                                   };
                                   bool ok = std::all_of(train.begin(), train.end(), [&](const Pair& p) { auto o = make(p.in); return o && *o == p.out; });
                                   return ok ? std::optional<Predictor>(make) : std::nullopt;
                               } });
            }
        }
    }

    using ColorChoice = std::function<std::optional<int>(const G&)>;
    auto byObjects = [](bool largest) {
        return [largest](const G& g) -> std::optional<int> {
            auto objs = objects(g, background(g), true, false);
            if (objs.empty()) return std::nullopt;
            auto it = largest ? std::max_element(objs.begin(), objs.end(), [](auto& a, auto& b) { return a.pixels.size() < b.pixels.size(); })
                              : std::min_element(objs.begin(), objs.end(), [](auto& a, auto& b) { return a.pixels.size() < b.pixels.size(); });
            return *it->colors.begin();
        };
    };
    const std::vector<std::pair<std::string, ColorChoice>> choices {
        { "most frequent foreground color", [](const G& g) -> std::optional<int> {
              auto h = histogram(g); h.erase(background(g)); if (h.empty()) return std::nullopt;
              return std::max_element(h.begin(), h.end(), [](auto& a, auto& b) { return a.second < b.second; })->first; } },
        { "least frequent color", [](const G& g) -> std::optional<int> {
              auto h = histogram(g);
              return std::min_element(h.begin(), h.end(), [](auto& a, auto& b) { return a.second < b.second; })->first; } },
        { "color of the largest object", byObjects(true) },
        { "color of the smallest object", byObjects(false) },
        { "color with most objects", [](const G& g) -> std::optional<int> {
              std::map<int, int> h; for (auto& o : objects(g, background(g), true, false)) ++h[*o.colors.begin()];
              if (h.empty()) return std::nullopt;
              return std::max_element(h.begin(), h.end(), [](auto& a, auto& b) { return a.second < b.second; })->first; } },
        { "enclosed color", [](const G& g) -> std::optional<int> {
              const int bg = background(g);
              for (auto& o : objects(g, bg, true, false)) {
                  G box = crop(g, o.box);
                  const int c = *o.colors.begin();
                  for (int y = 1; y + 1 < box.h; ++y) for (int x = 1; x + 1 < box.w; ++x) {
                      const int v = box.at(x, y);
                      if (v != c && v != bg && box.at(x - 1, y) == c && box.at(x + 1, y) == c && box.at(x, y - 1) == c && box.at(x, y + 1) == c) return v;
                  }
              }
              return std::nullopt; } },
    };
    for (auto& [name, choose] : choices) {
        ret.push_back({ "answer: " + name, 3.0, [choose](const std::vector<Pair>& train) -> std::optional<Predictor> {
                           auto size = constantOutputSize(train);
                           if (!size) return std::nullopt;
                           auto make = [choose, size = *size](const G& in) -> std::optional<G> {
                               auto c = choose(in);
                               if (!c) return std::nullopt;
                               return G(size.first, size.second, *c);
                           };
                           bool ok = std::all_of(train.begin(), train.end(), [&](const Pair& p) { auto o = make(p.in); return o && *o == p.out; });
                           return ok ? std::optional<Predictor>(make) : std::nullopt;
                       } });
    }
    return ret;
}

const std::vector<std::string>& featureNames()
{
    static const std::vector<std::string> names {
        "color", "up", "down", "left", "right", "up-left", "up-right", "down-left", "down-right",
        "enclosed", "between horizontally", "between vertically", "between diagonally \\", "between diagonally /",
        "ray left", "ray right", "ray up", "ray down", "ray up-left", "ray up-right", "ray down-left", "ray down-right",
        "object size", "is largest object", "is smallest object", "multi object size", "multi object color count",
        "object touches border", "x", "y", "x from right", "y from bottom", "x mod 2", "y mod 2", "x mod 3", "y mod 3",
        "row foreground count", "column foreground count", "mirror H color", "mirror V color", "rotate180 color",
        "foreground neighbours", "color frequency rank", "inside object box (color)", "on object box border",
        "dominant foreground color", "rarest color", "object main color", "transpose color", "object size rank",
        "object has hole", "object width", "object height", "object is rectangle", "same shape count",
        "x in object", "y in object", "x in object from right", "y in object from bottom",
        "region size", "row color", "column color", "nearest color", "distance to foreground",
        "is foreground", "has partner horizontally", "has partner vertically", "has partner diagonally \\", "has partner diagonally /",
        "horizontal midpoint", "vertical midpoint", "4 neighbour pattern", "diagonal pattern", "object half (vertical)", "object half (horizontal)"
    };
    return names;
}

std::vector<double>& pixelFeatureCosts()
{
    static std::vector<double> costs(featureNames().size(), 0.6);
    return costs;
}

double featureCost(const std::vector<int>& features)
{
    double c = 0.0;
    for (int f : features) c += pixelFeatureCosts()[f];
    return c;
}

const std::vector<int>& copyFeatures()
{
    static const std::vector<int> ids { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 38, 39, 40, 43, 45, 46, 47, 48, 60, 61, 62 };
    return ids;
}

std::vector<std::vector<int>> pixelFeatures(const G& g)
{
    const int n  = g.w * g.h;
    const int bg = background(g);
    std::vector<std::vector<int>> f(featureNames().size(), std::vector<int>(n, -1));
    auto nb   = [&](int x, int y) { return g.inRange(x, y) ? g.at(x, y) : -1; };
    auto fgAt = [&](int x, int y) { return g.inRange(x, y) && g.at(x, y) != bg ? 1 : 0; };

    auto hist = histogram(g);
    std::vector<std::pair<int, int>> byFreq;
    for (auto [c, cnt] : hist) {
        byFreq.push_back({ -cnt, c });
    }
    std::sort(byFreq.begin(), byFreq.end());
    std::map<int, int> rank;
    for (size_t i = 0; i < byFreq.size(); ++i) {
        rank[byFreq[i].second] = static_cast<int>(i);
    }
    int dominantFg = -1;
    for (auto& [negCnt, c] : byFreq) {
        if (c != bg) {
            dominantFg = c;
            break;
        }
    }
    const int rarest = byFreq.back().second;
    const auto enclosed = enclosedMask(g, bg);

    std::vector<int> rowCount(g.h, 0), colCount(g.w, 0);
    for (int y = 0; y < g.h; ++y) {
        for (int x = 0; x < g.w; ++x) {
            if (g.at(x, y) != bg) {
                ++rowCount[y];
                ++colCount[x];
            }
        }
    }

    auto single = objects(g, bg, true, false);
    size_t minSize = 1 << 20, maxSize = 0;
    for (auto& o : single) {
        minSize = std::min(minSize, o.pixels.size());
        maxSize = std::max(maxSize, o.pixels.size());
    }
    std::vector<int> objSize(n, 0), isLargest(n, 0), isSmallest(n, 0);
    for (auto& o : single) {
        for (auto [x, y] : o.pixels) {
            objSize[y * g.w + x]    = static_cast<int>(o.pixels.size());
            isLargest[y * g.w + x]  = o.pixels.size() == maxSize;
            isSmallest[y * g.w + x] = o.pixels.size() == minSize;
        }
    }
    auto multi = objects(g, bg, false, true);
    std::vector<size_t> sizes;
    for (auto& o : multi) {
        sizes.push_back(o.pixels.size());
    }
    std::sort(sizes.rbegin(), sizes.rend());
    std::vector<int> mSize(n, 0), mColors(n, 0), touches(n, 0), mainColor(n, -1), sizeRank(n, -1), insideBox(n, -1), onBoxBorder(n, 0);
    std::vector<int> hasHole(n, -1), objW(n, -1), objH(n, -1), isRect(n, -1), shapeCount(n, -1), relX(n, -1), relY(n, -1), relXr(n, -1), relYb(n, -1);
    {
        std::map<Key, int> shapes;
        std::vector<Key> sig;
        for (auto& o : single) {
            Key k { o.box.w(), o.box.h() };
            std::vector<int> mask(o.box.w() * o.box.h(), 0);
            for (auto [x, y] : o.pixels) mask[(y - o.box.y0) * o.box.w() + (x - o.box.x0)] = 1;
            k.insert(k.end(), mask.begin(), mask.end());
            ++shapes[k];
            sig.push_back(k);
        }
        for (size_t i = 0; i < single.size(); ++i)
            for (auto [x, y] : single[i].pixels) shapeCount[y * g.w + x] = std::min(shapes[sig[i]], 3);
    }
    for (auto& o : multi) {
        std::map<int, int> oh;
        for (auto [x, y] : o.pixels) {
            ++oh[g.at(x, y)];
        }
        const int main  = std::max_element(oh.begin(), oh.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
        const bool tb   = o.box.x0 == 0 || o.box.y0 == 0 || o.box.x1 == g.w - 1 || o.box.y1 == g.h - 1;
        const int sRank = std::min<int>(3, static_cast<int>(std::find(sizes.begin(), sizes.end(), o.pixels.size()) - sizes.begin()));
        G box           = crop(g, o.box);
        const auto holes = enclosedMask(box, bg);
        const int hole  = std::any_of(holes.begin(), holes.end(), [](int v) { return v; });
        const int rect  = int(o.pixels.size()) == o.box.w() * o.box.h();
        for (int y = o.box.y0; y <= o.box.y1; ++y) {
            for (int x = o.box.x0; x <= o.box.x1; ++x) {
                const int i = y * g.w + x;
                relX[i]  = x - o.box.x0;
                relY[i]  = y - o.box.y0;
                relXr[i] = o.box.x1 - x;
                relYb[i] = o.box.y1 - y;
                hasHole[i] = hole;
                objW[i]    = o.box.w();
                objH[i]    = o.box.h();
                isRect[i]  = rect;
            }
        }
        for (auto [x, y] : o.pixels) {
            const int i = y * g.w + x;
            mSize[i]     = static_cast<int>(o.pixels.size());
            mColors[i]   = static_cast<int>(o.colors.size());
            touches[i]   = tb;
            mainColor[i] = main;
            sizeRank[i]  = sRank;
        }
        if (o.pixels.size() >= 2) {
            for (int y = o.box.y0; y <= o.box.y1; ++y) {
                for (int x = o.box.x0; x <= o.box.x1; ++x) {
                    insideBox[y * g.w + x] = main;
                    if (x == o.box.x0 || x == o.box.x1 || y == o.box.y0 || y == o.box.y1) {
                        onBoxBorder[y * g.w + x] = 1;
                    }
                }
            }
        }
    }

    std::vector<int> regionSize(n, 0), rowColor(g.h, -1), colColor(g.w, -1), nearest(n, -1), dist(n, -1);
    {
        std::vector<int> comp(n, -1);
        for (int i = 0; i < n; ++i) {
            if (comp[i] >= 0) continue;
            std::vector<int> stack { i }, members;
            comp[i] = i;
            while (!stack.empty()) {
                const int j = stack.back();
                stack.pop_back();
                members.push_back(j);
                const int jx = j % g.w, jy = j / g.w;
                const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                for (auto& dd : d) {
                    const int nx = jx + dd[0], ny = jy + dd[1];
                    if (g.inRange(nx, ny) && comp[ny * g.w + nx] < 0 && g.at(nx, ny) == g.p[j]) {
                        comp[ny * g.w + nx] = i;
                        stack.push_back(ny * g.w + nx);
                    }
                }
            }
            for (int j : members) regionSize[j] = int(members.size());
        }
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x)
                if (g.at(x, y) != bg) {
                    if (rowColor[y] < 0) rowColor[y] = g.at(x, y);
                    if (colColor[x] < 0) colColor[x] = g.at(x, y);
                }
        std::vector<int> queue;
        for (int i = 0; i < n; ++i)
            if (g.p[i] != bg) {
                nearest[i] = g.p[i];
                dist[i]    = 0;
                queue.push_back(i);
            }
        for (size_t q = 0; q < queue.size(); ++q) {
            const int j = queue[q], jx = j % g.w, jy = j / g.w;
            const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (auto& dd : d) {
                const int nx = jx + dd[0], ny = jy + dd[1];
                if (!g.inRange(nx, ny)) continue;
                const int k = ny * g.w + nx;
                if (dist[k] < 0) {
                    dist[k]    = dist[j] + 1;
                    nearest[k] = nearest[j];
                    queue.push_back(k);
                } else if (dist[k] == dist[j] + 1 && nearest[k] != nearest[j]) {
                    nearest[k] = -2;
                }
            }
        }
        for (int& d : dist) d = std::min(d, 6);
    }

    for (int y = 0; y < g.h; ++y) {
        for (int x = 0; x < g.w; ++x) {
            const int i = y * g.w + x;
            const int c = g.at(x, y);
            const int l = ray(g, bg, x, y, -1, 0), r = ray(g, bg, x, y, 1, 0);
            const int u = ray(g, bg, x, y, 0, -1), d = ray(g, bg, x, y, 0, 1);
            const int ul = ray(g, bg, x, y, -1, -1), ur = ray(g, bg, x, y, 1, -1);
            const int dl = ray(g, bg, x, y, -1, 1), dr = ray(g, bg, x, y, 1, 1);
            int k = 0, fgNb = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((dx || dy) && g.inRange(x + dx, y + dy) && g.at(x + dx, y + dy) != bg) {
                        ++fgNb;
                    }
                }
            }
            const int values[] = {
                c, nb(x, y - 1), nb(x, y + 1), nb(x - 1, y), nb(x + 1, y), nb(x - 1, y - 1), nb(x + 1, y - 1), nb(x - 1, y + 1), nb(x + 1, y + 1),
                enclosed[i], l == r ? l : -1, u == d ? u : -1, ul == dr ? ul : -1, ur == dl ? ur : -1,
                l, r, u, d, ul, ur, dl, dr,
                objSize[i], isLargest[i], isSmallest[i], mSize[i], mColors[i],
                touches[i], x, y, g.w - 1 - x, g.h - 1 - y, x % 2, y % 2, x % 3, y % 3,
                rowCount[y], colCount[x], g.at(g.w - 1 - x, y), g.at(x, g.h - 1 - y), g.at(g.w - 1 - x, g.h - 1 - y),
                fgNb, rank[c], insideBox[i], onBoxBorder[i],
                dominantFg, rarest, mainColor[i], g.w == g.h ? g.at(y, x) : -1, sizeRank[i],
                hasHole[i], objW[i], objH[i], isRect[i], shapeCount[i], relX[i], relY[i], relXr[i], relYb[i],
                regionSize[i], rowColor[y], colColor[x], nearest[i], dist[i],
                c != bg, l == r && l >= 0, u == d && u >= 0, ul == dr && ul >= 0, ur == dl && ur >= 0,
                l == r && l >= 0 && rayDist(g, bg, x, y, -1, 0) == rayDist(g, bg, x, y, 1, 0),
                u == d && u >= 0 && rayDist(g, bg, x, y, 0, -1) == rayDist(g, bg, x, y, 0, 1),
                fgAt(x, y - 1) | fgAt(x, y + 1) << 1 | fgAt(x - 1, y) << 2 | fgAt(x + 1, y) << 3,
                fgAt(x - 1, y - 1) | fgAt(x + 1, y - 1) << 1 | fgAt(x - 1, y + 1) << 2 | fgAt(x + 1, y + 1) << 3,
                objH[i] < 0 ? -1 : (relY[i] * 2 + 1 == objH[i] ? 2 : relY[i] * 2 < objH[i] ? 0 : 1),
                objW[i] < 0 ? -1 : (relX[i] * 2 + 1 == objW[i] ? 2 : relX[i] * 2 < objW[i] ? 0 : 1)
            };
            for (int v : values) {
                f[k++][i] = v;
            }
        }
    }
    return f;
}

struct ColorCode
{
    bool active = false;
    std::map<int, int> toCode;
    std::map<int, int> toColor;

    static ColorCode of(const G& g, bool active)
    {
        ColorCode code;
        code.active = active;
        if (!active) {
            return code;
        }
        std::vector<std::pair<int, int>> byFreq;
        for (auto [c, cnt] : histogram(g)) {
            byFreq.push_back({ -cnt, c });
        }
        std::sort(byFreq.begin(), byFreq.end());
        for (size_t i = 0; i < byFreq.size(); ++i) {
            code.toCode[byFreq[i].second] = static_cast<int>(i);
            code.toColor[static_cast<int>(i)] = byFreq[i].second;
        }
        return code;
    }
    G encode(G g) const
    {
        if (active) {
            for (int& c : g.p) {
                c = toCode.at(c);
            }
        }
        return g;
    }
    int encodeOut(int c) const
    {
        if (!active) {
            return c;
        }
        auto it = toCode.find(c);
        return it != toCode.end() ? it->second : 100 + c;
    }
    std::optional<int> decode(int v) const
    {
        if (!active) {
            return v;
        }
        if (v >= 100) {
            return v - 100;
        }
        auto it = toColor.find(v);
        if (it == toColor.end()) {
            return std::nullopt;
        }
        return it->second;
    }
};

std::optional<G> separatorCells(const G& g)
{
    for (int sep = 0; sep < 10; ++sep) {
        std::vector<int> cutRows { -1 }, cutCols { -1 };
        for (int y = 0; y < g.h; ++y) {
            bool full = true;
            for (int x = 0; x < g.w && full; ++x) full = g.at(x, y) == sep;
            if (full) cutRows.push_back(y);
        }
        for (int x = 0; x < g.w; ++x) {
            bool full = true;
            for (int y = 0; y < g.h && full; ++y) full = g.at(x, y) == sep;
            if (full) cutCols.push_back(x);
        }
        if (cutRows.size() + cutCols.size() <= 2) {
            continue;
        }
        cutRows.push_back(g.h);
        cutCols.push_back(g.w);
        std::map<int, int> hist;
        for (int c : g.p) if (c != sep) ++hist[c];
        if (hist.empty()) continue;
        const int bg = std::max_element(hist.begin(), hist.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
        std::vector<std::pair<int, int>> rows, cols;
        for (size_t i = 0; i + 1 < cutRows.size(); ++i) if (cutRows[i + 1] - cutRows[i] > 1) rows.push_back({ cutRows[i] + 1, cutRows[i + 1] - 1 });
        for (size_t i = 0; i + 1 < cutCols.size(); ++i) if (cutCols[i + 1] - cutCols[i] > 1) cols.push_back({ cutCols[i] + 1, cutCols[i + 1] - 1 });
        if (rows.empty() || cols.empty() || rows.size() * cols.size() < 2) continue;
        G out(static_cast<int>(cols.size()), static_cast<int>(rows.size()), bg);
        for (size_t r = 0; r < rows.size(); ++r) {
            for (size_t c = 0; c < cols.size(); ++c) {
                std::map<int, int> cell;
                for (int y = rows[r].first; y <= rows[r].second; ++y)
                    for (int x = cols[c].first; x <= cols[c].second; ++x)
                        if (g.at(x, y) != bg && g.at(x, y) != sep) ++cell[g.at(x, y)];
                if (!cell.empty()) out.at(static_cast<int>(c), static_cast<int>(r)) = std::max_element(cell.begin(), cell.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
            }
        }
        return out;
    }
    return std::nullopt;
}

struct PreTransform
{
    std::string name;
    double cost;
    std::function<std::optional<G>(const G&)> apply;
};

std::vector<PreTransform> preTransforms(const std::vector<Pair>& train)
{
    std::vector<PreTransform> ret;
    ret.push_back({ "", 0.0, [](const G& g) -> std::optional<G> { return g; } });
    for (auto& [name, select] : objectSelectors()) {
        ret.push_back({ "crop " + name, 1.0, [select](const G& g) -> std::optional<G> {
                           auto box = select(g);
                           if (!box) {
                               return std::nullopt;
                           }
                           return crop(g, *box);
                       } });
    }
    if (auto r = upRatio(train)) {
        ret.push_back({ "scale up", 1.0, [r = *r](const G& g) -> std::optional<G> { return scaleUp(g, r.first, r.second); } });
        ret.push_back({ "tile", 1.0, [r = *r](const G& g) -> std::optional<G> {
                           G out(g.w * r.first, g.h * r.second);
                           for (int y = 0; y < out.h; ++y) {
                               for (int x = 0; x < out.w; ++x) {
                                   out.at(x, y) = g.at(x % g.w, y % g.h);
                               }
                           }
                           return out;
                       } });
    }
    if (auto r = downRatio(train)) {
        ret.push_back({ "sample blocks", 1.0, [r = *r](const G& g) -> std::optional<G> {
                           if (g.w % r.first || g.h % r.second) {
                               return std::nullopt;
                           }
                           G out(g.w / r.first, g.h / r.second);
                           for (int y = 0; y < out.h; ++y) {
                               for (int x = 0; x < out.w; ++x) {
                                   out.at(x, y) = g.at(x * r.first, y * r.second);
                               }
                           }
                           return out;
                       } });
    }
    for (int k = 1; k < 8; ++k) {
        ret.push_back({ dihedralName(k), 0.5, [k](const G& g) -> std::optional<G> { return dihedral(g, k); } });
    }
    ret.push_back({ "separator cells", 1.0, [](const G& g) { return separatorCells(g); } });

    std::vector<PreTransform> fitting;
    for (auto& pre : ret) {
        bool ok = std::all_of(train.begin(), train.end(), [&](const Pair& p) {
            auto t = pre.apply(p.in);
            return t && t->w == p.out.w && t->h == p.out.h;
        });
        if (ok) {
            fitting.push_back(pre);
        }
    }
    return fitting;
}

std::string tableDetail(const std::vector<int>& features, const std::unordered_map<uint64_t, int>& table, bool abstractColors)
{
    static const char* names[] = { "black", "blue", "red", "green", "yellow", "grey", "magenta", "orange", "sky", "maroon" };
    const std::set<int> colorValued(copyFeatures().begin(), copyFeatures().end());
    auto color = [&](int v) -> std::string {
        if (v < 0) return "none";
        if (abstractColors) return "color #" + std::to_string(v + 1) + " by frequency";
        return v < 10 ? names[v] : std::to_string(v);
    };
    std::vector<std::pair<std::vector<int>, int>> rows;
    for (auto& [h, value] : table) {
        std::vector<int> key(features.size());
        for (size_t i = 0; i < features.size(); ++i)
            key[i] = static_cast<int>((h >> (12 * (features.size() - 1 - i))) & 0xFFF) - 2;
        rows.push_back({ key, value });
    }
    std::sort(rows.begin(), rows.end());
    std::string s;
    for (size_t r = 0; r < rows.size() && r < 60; ++r) {
        const auto& [key, value] = rows[r];
        for (size_t i = 0; i < features.size(); ++i) {
            s += (i ? ", " : "") + featureNames()[features[i]] + " = ";
            s += colorValued.count(features[i]) ? color(key[i]) : std::to_string(key[i]);
        }
        s += "  ->  ";
        s += value >= 0 ? color(value) : "copy " + featureNames()[copyFeatures()[-value - 1]];
        s += "\n";
    }
    if (rows.size() > 60) s += "... " + std::to_string(rows.size() - 60) + " more cases\n";
    return s;
}

void featureTableCandidates(const std::vector<Pair>& train, std::vector<Candidate>& out)
{
    const int featureCount = static_cast<int>(featureNames().size());
    for (const PreTransform& pre : preTransforms(train)) {
        for (int abstractColors = 0; abstractColors < 2; ++abstractColors) {
            std::vector<std::vector<std::vector<int>>> feats;
            std::vector<std::vector<int>> targets;
            bool ok = true;
            for (auto& p : train) {
                auto t = pre.apply(p.in);
                if (!t) {
                    ok = false;
                    break;
                }
                const ColorCode code = ColorCode::of(*t, abstractColors);
                feats.push_back(pixelFeatures(code.encode(*t)));
                std::vector<int> tgt;
                for (int c : p.out.p) {
                    tgt.push_back(code.encodeOut(c));
                }
                targets.push_back(std::move(tgt));
            }
            if (!ok) {
                continue;
            }
            std::vector<std::vector<uint32_t>> copyMasks;
            for (size_t pi = 0; pi < targets.size(); ++pi) {
                std::vector<uint32_t> masks(targets[pi].size(), 0);
                for (size_t px = 0; px < targets[pi].size(); ++px) {
                    for (size_t j = 0; j < copyFeatures().size(); ++j) {
                        if (feats[pi][copyFeatures()[j]][px] == targets[pi][px]) {
                            masks[px] |= 1u << j;
                        }
                    }
                }
                copyMasks.push_back(std::move(masks));
            }

            struct Found
            {
                std::vector<int> features;
                bool generalizes;
                std::shared_ptr<std::unordered_map<uint64_t, int>> table;
            };
            std::vector<Found> found;
            const int maxK = 3;
            for (int k = 1; k <= maxK; ++k) {
                std::vector<int> idx(k);
                for (int i = 0; i < k; ++i) {
                    idx[i] = i;
                }
                while (true) {
                    auto key = [&](size_t pair, int pixel) {
                        uint64_t h = 0;
                        for (int f : idx) {
                            h = (h << 12) | static_cast<uint64_t>((feats[pair][f][pixel] + 2) & 0xFFF);
                        }
                        return h;
                    };
                    struct Entry
                    {
                        int constValue;
                        bool constOk;
                        uint32_t copyMask;
                        uint32_t pairs;
                    };
                    std::unordered_map<uint64_t, Entry> table;
                    bool consistent = true;
                    for (size_t pi = 0; pi < train.size() && consistent; ++pi) {
                        for (size_t px = 0; px < targets[pi].size(); ++px) {
                            const int target = targets[pi][px];
                            const uint32_t copyMask = copyMasks[pi][px];
                            auto [it, inserted] = table.emplace(key(pi, static_cast<int>(px)), Entry { target, true, copyMask, 1u << pi });
                            if (!inserted) {
                                Entry& e = it->second;
                                if (e.constOk && e.constValue != target) {
                                    e.constOk = false;
                                }
                                e.copyMask &= copyMask;
                                e.pairs |= 1u << pi;
                                if (!e.constOk && e.copyMask == 0) {
                                    consistent = false;
                                    break;
                                }
                            }
                        }
                    }
                    if (consistent) {
                        bool generalizes = train.size() >= 2;
                        for (size_t pi = 0; pi < train.size() && generalizes; ++pi) {
                            for (size_t px = 0; px < targets[pi].size(); ++px) {
                                if ((table[key(pi, static_cast<int>(px))].pairs & ~(1u << pi)) == 0) {
                                    generalizes = false;
                                    break;
                                }
                            }
                        }
                        size_t pixels = 0;
                        for (auto& t : targets) {
                            pixels += t.size();
                        }
                        if (!generalizes || table.size() * 8 > pixels) {
                            consistent = false;
                        }
                    }
                    if (consistent) {
                        auto plain = std::make_shared<std::unordered_map<uint64_t, int>>();
                        for (auto& [h, e] : table) {
                            (*plain)[h] = e.constOk ? e.constValue : -(std::countr_zero(e.copyMask) + 1);
                        }
                        found.push_back({ idx, true, plain });
                    }
                    int i = k - 1;
                    while (i >= 0 && idx[i] == featureCount - k + i) {
                        --i;
                    }
                    if (i < 0) {
                        break;
                    }
                    ++idx[i];
                    for (int j = i + 1; j < k; ++j) {
                        idx[j] = idx[j - 1] + 1;
                    }
                }
                if (std::any_of(found.begin(), found.end(), [](const Found& f) { return f.generalizes; })) {
                    break;
                }
            }
            std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
                if (a.generalizes != b.generalizes) {
                    return a.generalizes;
                }
                if (a.features.size() != b.features.size()) {
                    return a.features.size() < b.features.size();
                }
                return a.table->size() < b.table->size();
            });
            if (found.size() > 4) {
                found.resize(4);
            }
            for (const Found& f : found) {
              for (int backoff = 0; backoff < 2; ++backoff) {
                std::string name = backoff ? "backoff table[" : "table[";
                for (size_t i = 0; i < f.features.size(); ++i) {
                    name += std::string(i ? ", " : "") + "{" + featureNames()[f.features[i]] + "}";
                }
                name += "]";
                if (abstractColors) {
                    name += " on color ranks";
                }
                if (!pre.name.empty()) {
                    name = pre.name + " -> " + name;
                }
                const double complexity = 2.0 + featureCost(f.features) + pre.cost + 0.3 * abstractColors + 0.001 * f.table->size() + 2.0 * backoff;
                Predictor predict = [apply = pre.apply, abstractColors, features = f.features, table = f.table, backoff](const G& in) -> std::optional<G> {
                    auto t = apply(in);
                    if (!t) {
                        return std::nullopt;
                    }
                    const ColorCode code = ColorCode::of(*t, abstractColors);
                    auto fv              = pixelFeatures(code.encode(*t));
                    G out(t->w, t->h);
                    for (int px = 0; px < t->w * t->h; ++px) {
                        uint64_t h = 0;
                        for (int fi : features) {
                            h = (h << 12) | static_cast<uint64_t>((fv[fi][px] + 2) & 0xFFF);
                        }
                        auto it = table->find(h);
                        if (it == table->end() && !backoff) {
                            return std::nullopt;
                        }
                        int value = it != table->end() ? it->second : fv[0][px];
                        if (value < 0) {
                            value = fv[copyFeatures()[-value - 1]][px];
                            if (value < 0) {
                                return std::nullopt;
                            }
                        }
                        auto color = code.decode(value);
                        if (!color) {
                            return std::nullopt;
                        }
                        out.p[px] = *color;
                    }
                    return out;
                };
                out.push_back({ name, complexity, f.generalizes, predict, tableDetail(f.features, *f.table, abstractColors) });
              }
            }
        }
    }
}

std::optional<std::pair<G, std::vector<Box>>> cellGrid(const G& layout, const G& content)
{
    auto boxes = cellBoxes(layout);
    if (!boxes) return std::nullopt;
    std::set<int> xs, ys;
    for (auto& b : *boxes) {
        xs.insert(b.x0);
        ys.insert(b.y0);
    }
    if (xs.size() * ys.size() != boxes->size()) return std::nullopt;
    const int bg = background(content);
    G cells(int(xs.size()), int(ys.size()), bg);
    for (size_t i = 0; i < boxes->size(); ++i) {
        std::map<int, int> h;
        const Box& b = (*boxes)[i];
        for (int y = b.y0; y <= b.y1; ++y)
            for (int x = b.x0; x <= b.x1; ++x) ++h[content.at(x, y)];
        if (h.size() == 1) {
            cells.p[i] = h.begin()->first;
        } else {
            h.erase(bg);
            cells.p[i] = std::max_element(h.begin(), h.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
        }
    }
    return std::make_pair(cells, *boxes);
}

std::vector<Learner> magnetLearners()
{
    return { { "objects move to the anchor", 3.5, [](const std::vector<Pair>& train) -> std::optional<Predictor> {
                  for (int anchor = 0; anchor < 10; ++anchor) {
                      auto make = [anchor](const G& in) -> std::optional<G> {
                          const int bg = background(in);
                          if (anchor == bg) return std::nullopt;
                          auto objs = objects(in, bg, true, false);
                          std::vector<const Object*> anchors;
                          for (auto& o : objs) if (o.colors.count(anchor)) anchors.push_back(&o);
                          if (anchors.size() != 1) return std::nullopt;
                          const Box a = anchors[0]->box;
                          G out(in.w, in.h, bg);
                          for (auto [x, y] : anchors[0]->pixels) out.at(x, y) = anchor;
                          for (auto& o : objs) {
                              if (&o == anchors[0]) continue;
                              int dx = 0, dy = 0;
                              if (o.box.x1 < a.x0) dx = 1; else if (o.box.x0 > a.x1) dx = -1;
                              if (o.box.y1 < a.y0) dy = 1; else if (o.box.y0 > a.y1) dy = -1;
                              int step = 0;
                              auto touches = [&](int s) {
                                  for (auto [x, y] : o.pixels)
                                      for (int ny = -1; ny <= 1; ++ny)
                                          for (int nx = -1; nx <= 1; ++nx) {
                                              if ((dx == 0 || dy == 0) && nx != 0 && ny != 0) continue;
                                              const int px = x + dx * s + nx, py = y + dy * s + ny;
                                              if (in.inRange(px, py) && out.at(px, py) == anchor) return true;
                                          }
                                  return false;
                              };
                              while ((dx || dy) && !touches(step) && step < 30) ++step;
                              for (auto [x, y] : o.pixels) {
                                  const int px = x + dx * step, py = y + dy * step;
                                  if (!in.inRange(px, py)) return std::nullopt;
                                  if (out.at(px, py) == bg) out.at(px, py) = in.at(x, y);
                              }
                          }
                          return out;
                      };
                      bool ok = std::all_of(train.begin(), train.end(), [&](const Pair& p) { auto o = make(p.in); return o && *o == p.out; });
                      if (ok) return Predictor(make);
                  }
                  return std::nullopt;
              } } };
}

std::vector<Learner> variableScaleLearners()
{
    using Count = std::function<int(const G&)>;
    const std::vector<std::pair<std::string, Count>> counts {
        { "foreground color count", [](const G& g) { return int(histogram(g).size()) - 1; } },
        { "object count", [](const G& g) { return int(objects(g, background(g), false, true).size()); } },
        { "foreground pixel count", [](const G& g) { const int bg = background(g); return int(std::count_if(g.p.begin(), g.p.end(), [bg](int c) { return c != bg; })); } },
    };
    std::vector<Learner> ret;
    for (auto& [name, count] : counts) {
        ret.push_back({ "scale up by " + name, 3.0, [count](const std::vector<Pair>& train) -> std::optional<Predictor> {
                           auto make = [count](const G& in) -> std::optional<G> {
                               const int f = count(in);
                               if (f < 1 || f > 10 || in.w * f > 30 || in.h * f > 30) return std::nullopt;
                               return scaleUp(in, f, f);
                           };
                           bool ok = std::all_of(train.begin(), train.end(), [&](const Pair& p) { auto o = make(p.in); return o && *o == p.out; });
                           return ok ? std::optional<Predictor>(make) : std::nullopt;
                       } });
    }
    return ret;
}

const std::vector<std::pair<std::string, std::function<std::optional<G>(const G&)>>>& firstSteps()
{
    static const std::vector<std::pair<std::string, std::function<std::optional<G>(const G&)>>> steps = [] {
        std::vector<std::pair<std::string, std::function<std::optional<G>(const G&)>>> ret;
        for (int k = 1; k < 8; ++k) {
            ret.push_back({ dihedralName(k), [k](const G& g) -> std::optional<G> { return dihedral(g, k); } });
        }
        for (auto& [name, select] : objectSelectors()) {
            if (name == "all foreground" || name == "largest object (multi color)" || name == "the unique object" || name == "the unique cell") {
                ret.push_back({ "crop " + name, [select](const G& g) -> std::optional<G> {
                                   auto b = select(g);
                                   if (!b) return std::nullopt;
                                   return crop(g, *b);
                               } });
            }
        }
        ret.push_back({ "separator cells", [](const G& g) { return separatorCells(g); } });
        ret.push_back({ "remove single pixels", [](const G& g) -> std::optional<G> {
                           const int bg = background(g);
                           G out        = g;
                           for (auto& o : objects(g, bg, false, true))
                               if (o.pixels.size() == 1) out.at(o.pixels[0].first, o.pixels[0].second) = bg;
                           return out;
                       } });
        const char* names[] = { "objects fall down", "objects fall up", "objects fall right", "objects fall left" };
        for (int dir = 0; dir < 4; ++dir) {
            ret.push_back({ names[dir], [dir](const G& g) { return objectGravity(g, dir, -1); } });
        }
        return ret;
    }();
    return steps;
}

using Step = std::pair<std::string, std::function<std::optional<G>(const G&)>>;

int bgOf(const G& g)
{
    return std::count(g.p.begin(), g.p.end(), 0) ? 0 : background(g);
}

std::optional<G> cropToContent(const G& g)
{
    const int bg = bgOf(g);
    Box b { 1 << 20, 1 << 20, -1, -1 };
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x)
            if (g.at(x, y) != bg) b = { std::min(b.x0, x), std::min(b.y0, y), std::max(b.x1, x), std::max(b.y1, y) };
    if (b.x1 < 0 || (b.w() == g.w && b.h() == g.h)) return std::nullopt;
    return crop(g, b);
}

std::optional<G> half(const G& g, int which)
{
    if (which < 2 && g.w < 2) return std::nullopt;
    if (which >= 2 && g.h < 2) return std::nullopt;
    const int hw = g.w / 2, hh = g.h / 2;
    switch (which) {
    case 0: return crop(g, { 0, 0, hw - 1, g.h - 1 });
    case 1: return crop(g, { g.w - hw, 0, g.w - 1, g.h - 1 });
    case 2: return crop(g, { 0, 0, g.w - 1, hh - 1 });
    default: return crop(g, { 0, g.h - hh, g.w - 1, g.h - 1 });
    }
}

std::optional<G> overlayHalves(const G& g, bool vertical, int op)
{
    auto a = half(g, vertical ? 2 : 0), b = half(g, vertical ? 3 : 1);
    if (!a || !b) return std::nullopt;
    const int bg = bgOf(g);
    G out(a->w, a->h, 0);
    for (size_t i = 0; i < a->p.size(); ++i) {
        const bool fa = a->p[i] != bg, fb = b->p[i] != bg;
        switch (op) {
        case 0: out.p[i] = fa ? a->p[i] : (fb ? b->p[i] : 0); break;
        case 1: out.p[i] = fa && fb; break;
        case 2: out.p[i] = fa != fb; break;
        default: out.p[i] = !fa && !fb; break;
        }
    }
    return out;
}

std::optional<G> removeEmptyLines(const G& g)
{
    const int bg = bgOf(g);
    std::vector<int> rows, cols;
    for (int y = 0; y < g.h; ++y) {
        bool empty = true;
        for (int x = 0; x < g.w && empty; ++x) empty = g.at(x, y) == bg;
        if (!empty) rows.push_back(y);
    }
    for (int x = 0; x < g.w; ++x) {
        bool empty = true;
        for (int y = 0; y < g.h && empty; ++y) empty = g.at(x, y) == bg;
        if (!empty) cols.push_back(x);
    }
    if (rows.empty() || cols.empty() || (static_cast<int>(rows.size()) == g.h && static_cast<int>(cols.size()) == g.w)) return std::nullopt;
    G out(static_cast<int>(cols.size()), static_cast<int>(rows.size()));
    for (size_t y = 0; y < rows.size(); ++y)
        for (size_t x = 0; x < cols.size(); ++x) out.at(static_cast<int>(x), static_cast<int>(y)) = g.at(cols[x], rows[y]);
    return out;
}

std::optional<G> removeDuplicateLines(const G& g)
{
    std::vector<int> rows { 0 }, cols { 0 };
    for (int y = 1; y < g.h; ++y) {
        bool same = true;
        for (int x = 0; x < g.w && same; ++x) same = g.at(x, y) == g.at(x, y - 1);
        if (!same) rows.push_back(y);
    }
    for (int x = 1; x < g.w; ++x) {
        bool same = true;
        for (int y = 0; y < g.h && same; ++y) same = g.at(x, y) == g.at(x - 1, y);
        if (!same) cols.push_back(x);
    }
    if (static_cast<int>(rows.size()) == g.h && static_cast<int>(cols.size()) == g.w) return std::nullopt;
    G out(static_cast<int>(cols.size()), static_cast<int>(rows.size()));
    for (size_t y = 0; y < rows.size(); ++y)
        for (size_t x = 0; x < cols.size(); ++x) out.at(static_cast<int>(x), static_cast<int>(y)) = g.at(cols[x], rows[y]);
    return out;
}

std::optional<G> downscale(const G& g, int f)
{
    if (g.w % f || g.h % f) return std::nullopt;
    G out(g.w / f, g.h / f);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) out.at(x, y) = g.at(x * f, y * f);
    return out;
}

std::optional<G> mirrorConcat(const G& g, int mode)
{
    if (g.w * 2 > 30 || g.h * 2 > 30) return std::nullopt;
    const G h = dihedral(g, 4), v = dihedral(g, 5), r = dihedral(g, 2);
    G out(mode == 1 ? g.w : g.w * 2, mode == 0 ? g.h : g.h * 2);
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x) {
            out.at(x, y) = g.at(x, y);
            if (mode != 1) out.at(x + g.w, y) = h.at(x, y);
            if (mode != 0) out.at(x, y + g.h) = v.at(x, y);
            if (mode == 2) out.at(x + g.w, y + g.h) = r.at(x, y);
        }
    return out;
}

std::optional<G> pixelGravity(const G& g, int dir)
{
    const int bg = bgOf(g);
    G out(g.w, g.h, bg);
    const bool vertical = dir < 2;
    const int lines = vertical ? g.w : g.h, len = vertical ? g.h : g.w;
    for (int l = 0; l < lines; ++l) {
        std::vector<int> fg;
        for (int k = 0; k < len; ++k) {
            const int c = vertical ? g.at(l, k) : g.at(k, l);
            if (c != bg) fg.push_back(c);
        }
        const bool toEnd = dir == 1 || dir == 3;
        for (size_t k = 0; k < fg.size(); ++k) {
            const int pos = toEnd ? len - static_cast<int>(fg.size()) + static_cast<int>(k) : static_cast<int>(k);
            if (vertical) out.at(l, pos) = fg[k]; else out.at(pos, l) = fg[k];
        }
    }
    if (out == g) return std::nullopt;
    return out;
}

std::optional<G> outlineOnly(const G& g)
{
    const int bg = bgOf(g);
    G out = g;
    bool changed = false;
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x) {
            if (g.at(x, y) == bg) continue;
            bool inner = true;
            for (int d = 0; d < 4 && inner; ++d) {
                const int nx = x + (d == 0) - (d == 1), ny = y + (d == 2) - (d == 3);
                inner = g.inRange(nx, ny) && g.at(nx, ny) != bg;
            }
            if (inner) {
                out.at(x, y) = bg;
                changed      = true;
            }
        }
    if (!changed) return std::nullopt;
    return out;
}

const std::vector<Step>& composeSteps()
{
    static const std::vector<Step> steps = [] {
        std::vector<Step> r;
        for (int k = 1; k < 8; ++k) r.push_back({ dihedralName(k), [k](const G& g) -> std::optional<G> { return dihedral(g, k); } });
        r.push_back({ "crop to content", cropToContent });
        for (auto& [name, select] : objectSelectors()) {
            if (name == "largest object (multi color)" || name == "smallest object (multi color)" || name == "the unique object") {
                r.push_back({ "crop " + name, [select](const G& g) -> std::optional<G> { auto b = select(g); if (!b) return std::nullopt; return crop(g, *b); } });
            }
        }
        const char* halves[] = { "left half", "right half", "top half", "bottom half" };
        for (int i = 0; i < 4; ++i) r.push_back({ halves[i], [i](const G& g) { return half(g, i); } });
        const char* ops[] = { "OR", "AND", "XOR", "NOR" };
        for (int v = 0; v < 2; ++v)
            for (int op = 0; op < 4; ++op) r.push_back({ std::string("overlay ") + (v ? "top/bottom " : "left/right ") + ops[op], [v, op](const G& g) { return overlayHalves(g, v != 0, op); } });
        for (int c = 0; c < 10; ++c) {
            r.push_back({ "remove color " + std::to_string(c), [c](const G& g) -> std::optional<G> {
                             const int bg = bgOf(g);
                             if (c == bg || !std::count(g.p.begin(), g.p.end(), c)) return std::nullopt;
                             G out = g;
                             for (int& v : out.p) if (v == c) v = bg;
                             return out;
                         } });
            r.push_back({ "keep only color " + std::to_string(c), [c](const G& g) -> std::optional<G> {
                             const int bg = bgOf(g);
                             if (c == bg || !std::count(g.p.begin(), g.p.end(), c)) return std::nullopt;
                             G out = g;
                             for (int& v : out.p) if (v != c) v = bg;
                             if (out == g) return std::nullopt;
                             return out;
                         } });
        }
        r.push_back({ "remove empty rows and columns", removeEmptyLines });
        r.push_back({ "remove repeated rows and columns", removeDuplicateLines });
        for (int f = 2; f <= 3; ++f) {
            r.push_back({ "scale up " + std::to_string(f), [f](const G& g) -> std::optional<G> { if (g.w * f > 30 || g.h * f > 30) return std::nullopt; return scaleUp(g, f, f); } });
            r.push_back({ "scale down " + std::to_string(f), [f](const G& g) { return downscale(g, f); } });
        }
        const char* mirrors[] = { "append mirror right", "append mirror below", "mirror four ways" };
        for (int m = 0; m < 3; ++m) r.push_back({ mirrors[m], [m](const G& g) { return mirrorConcat(g, m); } });
        const char* gravity[] = { "pixels fall up", "pixels fall down", "pixels fall left", "pixels fall right" };
        for (int d = 0; d < 4; ++d) r.push_back({ gravity[d], [d](const G& g) { return pixelGravity(g, d); } });
        r.push_back({ "outline only", outlineOnly });
        r.push_back({ "mark enclosed areas", [](const G& g) -> std::optional<G> {
                         const int bg  = bgOf(g);
                         const auto in = enclosedMask(g, bg);
                         if (std::none_of(in.begin(), in.end(), [](int v) { return v; })) return std::nullopt;
                         G out = g;
                         for (size_t i = 0; i < in.size(); ++i) if (in[i]) out.p[i] = 10;
                         return out;
                     } });
        const char* rays[] = { "extend pixels up", "extend pixels down", "extend pixels left", "extend pixels right" };
        for (int d = 0; d < 4; ++d)
            r.push_back({ rays[d], [d](const G& g) -> std::optional<G> {
                             const int bg = bgOf(g), dx = (d == 3) - (d == 2), dy = (d == 1) - (d == 0);
                             G out        = g;
                             for (int y = 0; y < g.h; ++y)
                                 for (int x = 0; x < g.w; ++x) {
                                     if (g.at(x, y) == bg) continue;
                                     for (int nx = x + dx, ny = y + dy; g.inRange(nx, ny) && g.at(nx, ny) == bg; nx += dx, ny += dy) out.at(nx, ny) = g.at(x, y);
                                 }
                             if (out == g) return std::nullopt;
                             return out;
                         } });
        for (int v = 0; v < 2; ++v)
            r.push_back({ v ? "connect same colors vertically" : "connect same colors horizontally", [v](const G& g) -> std::optional<G> {
                             const int bg = bgOf(g);
                             G out        = g;
                             const int lines = v ? g.w : g.h, len = v ? g.h : g.w;
                             for (int l = 0; l < lines; ++l) {
                                 int last = -1;
                                 for (int k = 0; k < len; ++k) {
                                     const int c = v ? g.at(l, k) : g.at(k, l);
                                     if (c == bg) continue;
                                     if (last >= 0 && (v ? g.at(l, last) : g.at(last, l)) == c)
                                         for (int j = last + 1; j < k; ++j) (v ? out.at(l, j) : out.at(j, l)) = c;
                                     last = k;
                                 }
                             }
                             if (out == g) return std::nullopt;
                             return out;
                         } });
        r.push_back({ "mark object boxes", [](const G& g) -> std::optional<G> {
                         const int bg = bgOf(g);
                         G out        = g;
                         for (auto& o : objects(g, bg, false, true))
                             for (int y = o.box.y0; y <= o.box.y1; ++y)
                                 for (int x = o.box.x0; x <= o.box.x1; ++x)
                                     if (g.at(x, y) == bg) out.at(x, y) = 10;
                         if (out == g) return std::nullopt;
                         return out;
                     } });
        for (int largest = 0; largest < 2; ++largest)
            r.push_back({ largest ? "keep only the largest object" : "keep only the smallest object", [largest](const G& g) -> std::optional<G> {
                             const int bg = bgOf(g);
                             auto objs    = objects(g, bg, false, true);
                             if (objs.size() < 2) return std::nullopt;
                             auto it = std::max_element(objs.begin(), objs.end(), [largest](auto& a, auto& b) { return largest ? a.pixels.size() < b.pixels.size() : a.pixels.size() > b.pixels.size(); });
                             G out(g.w, g.h, bg);
                             for (auto [x, y] : it->pixels) out.at(x, y) = g.at(x, y);
                             return out;
                         } });
        r.push_back({ "invert", [](const G& g) -> std::optional<G> {
                         const int bg = bgOf(g);
                         G out        = g;
                         for (int& c : out.p) c = c == bg ? 10 : bg;
                         return out;
                     } });
        r.push_back({ "separator cells", [](const G& g) { return separatorCells(g); } });
        r.push_back({ "remove single pixels", [](const G& g) -> std::optional<G> {
                         const int bg = bgOf(g);
                         G out        = g;
                         for (auto& o : objects(g, bg, false, true))
                             if (o.pixels.size() == 1) out.at(o.pixels[0].first, o.pixels[0].second) = bg;
                         if (out == g) return std::nullopt;
                         return out;
                     } });
        const char* fall[] = { "objects fall down", "objects fall up", "objects fall right", "objects fall left" };
        for (int d = 0; d < 4; ++d) r.push_back({ fall[d], [d](const G& g) { return objectGravity(g, d, -1); } });
        return r;
    }();
    return steps;
}

struct ComposeState
{
    std::vector<G> grids;
    std::vector<int> steps;
    double score;
};

double distanceToOutputs(const std::vector<G>& grids, const std::vector<Pair>& train)
{
    double d = 0.0;
    for (size_t i = 0; i < train.size(); ++i) {
        const G& g = grids[i];
        const G& o = train[i].out;
        if (g.w != o.w || g.h != o.h) {
            d += 1.0 + 0.01 * (std::abs(g.w - o.w) + std::abs(g.h - o.h));
            continue;
        }
        int diff = 0;
        for (size_t k = 0; k < g.p.size(); ++k) diff += g.p[k] != o.p[k];
        d += static_cast<double>(diff) / static_cast<double>(g.p.size());
    }
    return d / static_cast<double>(train.size());
}

uint64_t hashGrids(const std::vector<G>& grids)
{
    uint64_t h = 1469598103934665603ull;
    for (const G& g : grids) {
        h = (h ^ static_cast<uint64_t>(g.w * 31 + g.h)) * 1099511628211ull;
        for (int v : g.p) h = (h ^ static_cast<uint64_t>(v + 1)) * 1099511628211ull;
    }
    return h;
}

std::optional<G> replay(const std::vector<int>& steps, G g)
{
    for (int s : steps) {
        auto next = composeSteps()[s].second(g);
        if (!next || next->w == 0 || next->h == 0) return std::nullopt;
        g = std::move(*next);
    }
    return g;
}

std::string stepsName(const std::vector<int>& steps)
{
    std::string s;
    for (int k : steps) s += (s.empty() ? "" : ", then ") + composeSteps()[k].first;
    return s;
}

void composeCandidates(const std::vector<Pair>& train, const std::vector<Learner>& learners, std::vector<Candidate>& out)
{
    const SearchLimits limits = searchLimits();
    const int kDepth          = limits.depth;
    const size_t kLearnTop    = limits.finishers;
    const size_t kBeam        = limits.beam;
    auto trim = [kBeam](std::vector<ComposeState>& states) {
        std::stable_sort(states.begin(), states.end(), [](auto& a, auto& b) { return a.score < b.score; });
        if (states.size() > kBeam) states.resize(kBeam);
    };
    std::vector<ComposeState> frontier { { {}, {}, 0.0 } };
    for (auto& p : train) frontier[0].grids.push_back(p.in);
    std::set<uint64_t> seen { hashGrids(frontier[0].grids) };
    std::vector<ComposeState> all;
    auto exactName = [](const std::vector<int>& steps) { return stepsName(steps); };
    for (int depth = 1; depth <= kDepth; ++depth) {
        std::vector<ComposeState> next;
        for (const ComposeState& st : frontier) {
            for (size_t k = 0; k < composeSteps().size(); ++k) {
                ComposeState n { {}, st.steps, 0.0 };
                n.steps.push_back(static_cast<int>(k));
                bool ok = true;
                for (const G& g : st.grids) {
                    auto r = composeSteps()[k].second(g);
                    if (!r || r->w == 0 || r->h == 0) { ok = false; break; }
                    n.grids.push_back(std::move(*r));
                }
                if (!ok || !seen.insert(hashGrids(n.grids)).second) continue;
                n.score = distanceToOutputs(n.grids, train);
                next.push_back(std::move(n));
                if (next.size() >= 2 * kBeam + 64) trim(next);
            }
        }
        trim(next);
        for (auto& n : next) all.push_back(n);
        frontier = std::move(next);
    }
    std::stable_sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.score < b.score; });
    size_t learned = 0;
    for (const ComposeState& st : all) {
        const double stepCost = 1.0 * static_cast<double>(st.steps.size());
        auto program          = [steps = st.steps](const G& in) { return replay(steps, in); };
        if (st.score == 0.0) {
            out.push_back({ exactName(st.steps), 1.0 + stepCost, true, program });
            continue;
        }
        std::vector<std::pair<G, G>> pairs;
        for (size_t i = 0; i < train.size(); ++i) pairs.push_back({ st.grids[i], train[i].out });
        if (auto map = learnColorMap(pairs)) {
            out.push_back({ exactName(st.steps) + ", then color map", 1.5 + stepCost, true, [program, map = *map](const G& in) -> std::optional<G> {
                               auto t = program(in);
                               if (!t) return std::nullopt;
                               return applyColorMap(*t, map);
                           } });
            continue;
        }
        if (learned >= kLearnTop) continue;
        ++learned;
        std::vector<Pair> stepped;
        for (size_t i = 0; i < train.size(); ++i) stepped.push_back({ st.grids[i], train[i].out });
        for (const Learner& learner : learners) {
            std::optional<Predictor> predictor;
            std::string name = learner.name;
            double complexity = learner.complexity;
            if (learner.learnFull) {
                auto full = learner.learnFull(stepped);
                if (full) { predictor = full->predict; name = full->name; complexity = full->complexity; }
            } else {
                predictor = learner.learn(stepped);
            }
            if (!predictor) continue;
            const bool verified = std::all_of(stepped.begin(), stepped.end(), [&](const Pair& p) { auto o = (*predictor)(p.in); return o && *o == p.out; });
            if (!verified) continue;
            out.push_back({ exactName(st.steps) + ", then " + name, complexity + stepCost, false, [program, second = *predictor](const G& in) -> std::optional<G> {
                               auto t = program(in);
                               return t ? second(*t) : std::nullopt;
                           } });
        }
    }
}
} // namespace

namespace {
SearchLimits& limitsStorage()
{
    static SearchLimits limits = [] {
        SearchLimits l;
        if (const char* b = std::getenv("INFOCELL_COMPOSE_BEAM")) l.beam = std::strtoul(b, nullptr, 10);
        if (const char* f = std::getenv("INFOCELL_COMPOSE_LEARN")) l.finishers = std::strtoul(f, nullptr, 10);
        return l;
    }();
    return limits;
}
} // namespace

SearchLimits searchLimits()
{
    return limitsStorage();
}

void setSearchLimits(const SearchLimits& limits)
{
    limitsStorage() = limits;
}

std::vector<std::pair<std::string, double>> pixelFeatureVocabulary()
{
    std::vector<std::pair<std::string, double>> ret;
    for (size_t i = 0; i < featureNames().size(); ++i) ret.push_back({ featureNames()[i], pixelFeatureCosts()[i] });
    return ret;
}

std::vector<std::string> composeStepVocabulary()
{
    std::vector<std::string> ret;
    for (const auto& s : composeSteps()) ret.push_back(s.first);
    return ret;
}

std::vector<std::pair<std::string, double>> ProgramSearch::families() const
{
    std::vector<std::pair<std::string, double>> ret;
    for (const Learner& l : m_learners) ret.push_back({ l.name, l.complexity });
    return ret;
}

ProgramSearch::ProgramSearch()
{
    for (auto&& l : symmetryLearners()) m_learners.push_back(std::move(l));
    for (auto&& l : structuralLearners()) m_learners.push_back(std::move(l));
    for (auto&& l : cropLearners()) m_learners.push_back(std::move(l));
    for (auto&& l : lookupLearners()) m_learners.push_back(std::move(l));
    for (auto&& l : symmetryCompletionLearners()) m_learners.push_back(std::move(l));
    m_learners.push_back(splitLearner());
    m_learners.push_back(stampLearner());
    for (auto&& l : gravityLearners()) m_learners.push_back(std::move(l));
    for (auto&& l : periodicLearners()) m_learners.push_back(std::move(l));
    for (auto&& l : objectGravityLearners()) m_learners.push_back(std::move(l));
    for (auto&& l : classifierLearners()) m_learners.push_back(std::move(l));
    for (auto&& l : smallAnswerLearners()) m_learners.push_back(std::move(l));
    for (auto&& l : magnetLearners()) m_learners.push_back(std::move(l));
    for (auto&& l : variableScaleLearners()) m_learners.push_back(std::move(l));
    for (auto&& l : objectRuleLearners()) m_learners.push_back(std::move(l));
}

std::vector<Candidate> ProgramSearch::search(const std::vector<Pair>& train) const
{
    std::vector<Candidate> ret;
    if (train.empty()) {
        return ret;
    }
    for (const Learner& learner : m_learners) {
        std::optional<Predictor> predictor;
        std::string name     = learner.name;
        double complexity    = learner.complexity;
        if (learner.learnFull) {
            auto full = learner.learnFull(train);
            if (full) {
                predictor  = full->predict;
                name       = full->name;
                complexity = full->complexity;
            }
        } else {
            predictor = learner.learn(train);
            if (predictor && learner.explain) name = learner.explain(train);
        }
        if (!predictor) {
            continue;
        }
        bool verified = std::all_of(train.begin(), train.end(), [&](const Pair& p) {
            auto out = (*predictor)(p.in);
            return out && *out == p.out;
        });
        if (!verified) {
            continue;
        }
        bool generalizes = train.size() >= 2;
        for (size_t i = 0; i < train.size() && generalizes; ++i) {
            std::vector<Pair> rest;
            for (size_t j = 0; j < train.size(); ++j) {
                if (j != i) {
                    rest.push_back(train[j]);
                }
            }
            auto partial = learner.learn(rest);
            auto out     = partial ? (*partial)(train[i].in) : std::nullopt;
            generalizes  = out && *out == train[i].out;
        }
        ret.push_back({ name, complexity, generalizes, *predictor });
    }
    for (const auto& [firstName, first] : firstSteps()) {
        std::vector<Pair> stepped;
        bool changes = false, ok = true;
        for (const Pair& p : train) {
            auto t = first(p.in);
            if (!t || t->w == 0 || t->h == 0) {
                ok = false;
                break;
            }
            changes = changes || *t != p.in;
            stepped.push_back({ *t, p.out });
        }
        if (!ok || !changes) {
            continue;
        }
        for (const Learner& learner : m_learners) {
            if (learner.name.rfind("crop ", 0) == 0) {
                continue;
            }
            auto predictor = learner.learn(stepped);
            if (!predictor) {
                continue;
            }
            Predictor composed = [first, second = *predictor](const G& in) -> std::optional<G> {
                auto t = first(in);
                return t ? second(*t) : std::nullopt;
            };
            bool verified = std::all_of(train.begin(), train.end(), [&](const Pair& p) {
                auto out = composed(p.in);
                return out && *out == p.out;
            });
            if (!verified) {
                continue;
            }
            bool generalizes = stepped.size() >= 2;
            for (size_t i = 0; i < stepped.size() && generalizes; ++i) {
                std::vector<Pair> rest;
                for (size_t j = 0; j < stepped.size(); ++j) {
                    if (j != i) rest.push_back(stepped[j]);
                }
                auto partial = learner.learn(rest);
                auto out     = partial ? (*partial)(stepped[i].in) : std::nullopt;
                generalizes  = out && *out == stepped[i].out;
            }
            ret.push_back({ firstName + ", then " + (learner.explain ? learner.explain(stepped) : learner.name), learner.complexity + 1.5, generalizes, composed });
        }
    }

    composeCandidates(train, m_learners, ret);

    std::vector<Candidate> tables;
    featureTableCandidates(train, tables);
    for (Candidate& c : tables) {
        const bool verified = std::all_of(train.begin(), train.end(), [&](const Pair& p) {
            auto out = c.predict(p.in);
            return out && *out == p.out;
        });
        if (verified) {
            ret.push_back(std::move(c));
        }
    }

    if (std::all_of(train.begin(), train.end(), [](const Pair& p) { return p.in.w == p.out.w && p.in.h == p.out.h; })) {
        std::vector<Pair> cellPairs;
        bool ok = true;
        for (const Pair& p : train) {
            auto in  = cellGrid(p.in, p.in);
            auto out = cellGrid(p.in, p.out);
            if (!in || !out) {
                ok = false;
                break;
            }
            cellPairs.push_back({ in->first, out->first });
        }
        if (ok) {
            std::vector<Candidate> cellTables;
            featureTableCandidates(cellPairs, cellTables);
            for (Candidate& c : cellTables) {
                Predictor paint = [inner = c.predict](const G& in) -> std::optional<G> {
                    auto cells = cellGrid(in, in);
                    if (!cells) return std::nullopt;
                    auto predicted = inner(cells->first);
                    if (!predicted || predicted->w != cells->first.w || predicted->h != cells->first.h) return std::nullopt;
                    G out = in;
                    for (size_t i = 0; i < cells->second.size(); ++i) {
                        if (predicted->p[i] == cells->first.p[i]) continue;
                        const Box& b = cells->second[i];
                        for (int y = b.y0; y <= b.y1; ++y)
                            for (int x = b.x0; x <= b.x1; ++x) out.at(x, y) = predicted->p[i];
                    }
                    return out;
                };
                const bool verified = std::all_of(train.begin(), train.end(), [&](const Pair& p) {
                    auto out = paint(p.in);
                    return out && *out == p.out;
                });
                if (verified) {
                    ret.push_back({ "cells: " + c.name, c.complexity + 1.0, c.generalizes, paint });
                }
            }
        }
    }
    std::stable_sort(ret.begin(), ret.end(), [](const Candidate& a, const Candidate& b) {
        if (a.generalizes != b.generalizes) {
            return a.generalizes;
        }
        return a.complexity < b.complexity;
    });
    return ret;
}

std::vector<std::pair<std::string, G>> ProgramSearch::attempts(const std::vector<Candidate>& ranked, const G& testInput, size_t maxAttempts)
{
    struct Vote
    {
        G grid;
        double weight;
        std::string bestName;
        double bestWeight;
    };
    std::vector<Vote> votes;
    for (const Candidate& candidate : ranked) {
        auto out = candidate.predict(testInput);
        if (!out || out->w == 0 || out->h == 0) {
            continue;
        }
        const double weight = std::exp2(-2.0 * candidate.complexity) * (candidate.generalizes ? 1.0 : 1.0 / 64);
        auto it             = std::find_if(votes.begin(), votes.end(), [&](const Vote& v) { return v.grid == *out; });
        if (it == votes.end()) {
            votes.push_back({ *out, weight, candidate.name, weight });
        } else {
            it->weight += weight;
            if (weight > it->bestWeight) {
                it->bestName   = candidate.name;
                it->bestWeight = weight;
            }
        }
    }
    std::stable_sort(votes.begin(), votes.end(), [](const Vote& a, const Vote& b) { return a.weight > b.weight; });
    std::vector<std::pair<std::string, G>> ret;
    for (size_t i = 0; i < votes.size() && i < maxAttempts; ++i) {
        ret.push_back({ votes[i].bestName, votes[i].grid });
    }
    return ret;
}

void loadPixelConceptPrior(const std::map<std::string, int>& counts)
{
    constexpr double kPriorWeight = 20.0;
    const double features         = static_cast<double>(featureNames().size());
    double total                  = 0.0;
    for (const std::string& name : featureNames()) {
        auto it = counts.find(name);
        if (it != counts.end()) total += it->second;
    }
    for (size_t f = 0; f < featureNames().size(); ++f) {
        auto it              = counts.find(featureNames()[f]);
        const double n       = it == counts.end() ? 0.0 : it->second;
        const double bits    = -std::log2((n + kPriorWeight / features) / (total + kPriorWeight));
        pixelFeatureCosts()[f] = 0.6 * bits / std::log2(features);
    }
}

} // namespace search
} // namespace arc
} // namespace infocell
