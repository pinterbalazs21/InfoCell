#include "RuleInduction.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace infocell {
namespace arc {

bool RuleInduction::Rule::operator<(const Rule& rhs) const
{
    return std::tie(anchorColor, dx, dy, resultColor) < std::tie(rhs.anchorColor, rhs.dx, rhs.dy, rhs.resultColor);
}

std::string RuleInduction::Rule::toString() const
{
    std::ostringstream os;
    os << "color_in(P) == " << getArcColorName(static_cast<ColorId>(anchorColor))
       << "  =>  color_out(P + (" << dx << ", " << dy << ")) == " << getArcColorName(static_cast<ColorId>(resultColor));
    return os.str();
}

std::vector<int> RuleInduction::colorIds(const native::Grid& grid)
{
    std::vector<int> ret;
    ret.reserve(grid.pixels().size());
    for (const Color& color : grid.pixels()) {
        ret.push_back(static_cast<int>(color.id()));
    }
    return ret;
}

int RuleInduction::backgroundColor(const std::vector<int>& pixels)
{
    std::map<int, int> histogram;
    for (int color : pixels) {
        ++histogram[color];
    }
    int best      = 0;
    int bestCount = -1;
    for (const auto& [color, count] : histogram) {
        if (count > bestCount) {
            best      = color;
            bestCount = count;
        }
    }
    return best;
}

namespace {

struct Observation
{
    int width;
    int height;
    int background;
    std::vector<int> in;
    std::vector<int> out;

    int at(const std::vector<int>& pixels, int x, int y) const { return pixels[y * width + x]; }
    bool inRange(int x, int y) const { return x >= 0 && y >= 0 && x < width && y < height; }
};

} // namespace

bool RuleInduction::learn(const std::vector<Pair>& trainPairs, int maxRadius)
{
    m_rules.clear();
    m_radius = 0;
    if (trainPairs.empty()) {
        return false;
    }
    for (int radius = 1; radius <= maxRadius; ++radius) {
        if (learnWithRadius(trainPairs, radius)) {
            m_radius = radius;
            return true;
        }
    }
    m_rules.clear();
    return false;
}

bool RuleInduction::learnWithRadius(const std::vector<Pair>& trainPairs, int radius)
{
    m_rules.clear();

    std::vector<Observation> observations;
    for (const Pair& pair : trainPairs) {
        if (pair.input.width() != pair.output.width() || pair.input.height() != pair.output.height()) {
            return false;
        }
        Observation obs { pair.input.width(), pair.input.height(), 0, colorIds(pair.input), colorIds(pair.output) };
        obs.background = backgroundColor(obs.in);
        observations.push_back(std::move(obs));
    }

    std::set<Rule> candidates;
    for (const Observation& obs : observations) {
        for (int y = 0; y < obs.height; ++y) {
            for (int x = 0; x < obs.width; ++x) {
                const int inColor  = obs.at(obs.in, x, y);
                const int outColor = obs.at(obs.out, x, y);
                if (inColor == outColor) {
                    continue;
                }
                if (inColor != obs.background) {
                    return false;
                }
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const int ax = x - dx;
                        const int ay = y - dy;
                        if (!obs.inRange(ax, ay)) {
                            continue;
                        }
                        const int anchorColor = obs.at(obs.in, ax, ay);
                        if (anchorColor != obs.background) {
                            candidates.insert({ anchorColor, dx, dy, outColor });
                        }
                    }
                }
            }
        }
    }

    for (const Rule& rule : candidates) {
        bool consistent = true;
        for (const Observation& obs : observations) {
            for (int y = 0; y < obs.height && consistent; ++y) {
                for (int x = 0; x < obs.width && consistent; ++x) {
                    if (obs.at(obs.in, x, y) != rule.anchorColor) {
                        continue;
                    }
                    const int tx = x + rule.dx;
                    const int ty = y + rule.dy;
                    if (obs.inRange(tx, ty) && obs.at(obs.in, tx, ty) == obs.background && obs.at(obs.out, tx, ty) != rule.resultColor) {
                        consistent = false;
                    }
                }
            }
            if (!consistent) {
                break;
            }
        }
        if (consistent) {
            m_rules.push_back(rule);
        }
    }

    for (size_t i = 0; i < trainPairs.size(); ++i) {
        auto predicted = apply(trainPairs[i].input);
        if (!predicted || *predicted != observations[i].out) {
            return false;
        }
    }
    return true;
}

std::optional<std::vector<int>> RuleInduction::apply(const native::Grid& input) const
{
    const int width            = input.width();
    const int height           = input.height();
    const std::vector<int> in  = colorIds(input);
    const int background       = backgroundColor(in);
    std::vector<int> out       = in;
    std::vector<bool> written(in.size(), false);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int anchorColor = in[y * width + x];
            if (anchorColor == background) {
                continue;
            }
            for (const Rule& rule : m_rules) {
                if (rule.anchorColor != anchorColor) {
                    continue;
                }
                const int tx = x + rule.dx;
                const int ty = y + rule.dy;
                if (tx < 0 || ty < 0 || tx >= width || ty >= height) {
                    continue;
                }
                const int index = ty * width + tx;
                if (in[index] != background) {
                    continue;
                }
                if (written[index] && out[index] != rule.resultColor) {
                    return std::nullopt;
                }
                out[index]     = rule.resultColor;
                written[index] = true;
            }
        }
    }
    return out;
}

std::string RuleInduction::description() const
{
    std::ostringstream os;
    if (m_rules.empty()) {
        os << "  (identity: output == input)\n";
    }
    for (const Rule& rule : m_rules) {
        os << "  " << rule.toString() << "\n";
    }
    return os.str();
}

} // namespace arc
} // namespace infocell
