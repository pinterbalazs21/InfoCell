#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Grid.h"

namespace infocell {
namespace arc {

class RuleInduction
{
public:
    struct Rule
    {
        int anchorColor;
        int dx;
        int dy;
        int resultColor;

        bool operator<(const Rule& rhs) const;
        std::string toString() const;
    };

    struct Pair
    {
        const native::Grid& input;
        const native::Grid& output;
    };

    bool learn(const std::vector<Pair>& trainPairs, int maxRadius = 3);

    std::optional<std::vector<int>> apply(const native::Grid& input) const;

    const std::vector<Rule>& rules() const { return m_rules; }
    int radius() const { return m_radius; }
    std::string description() const;

    static std::vector<int> colorIds(const native::Grid& grid);
    static int backgroundColor(const std::vector<int>& pixels);

private:
    bool learnWithRadius(const std::vector<Pair>& trainPairs, int radius);

    std::vector<Rule> m_rules;
    int m_radius = 0;
};

} // namespace arc
} // namespace infocell
