#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Grid.h"

namespace infocell {
namespace arc {
namespace search {

struct G
{
    int w = 0;
    int h = 0;
    std::vector<int> p;

    G() = default;
    G(int w, int h, int fill = 0) :
        w(w), h(h), p(w * h, fill) { }

    int at(int x, int y) const { return p[y * w + x]; }
    int& at(int x, int y) { return p[y * w + x]; }
    bool inRange(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    bool operator==(const G& rhs) const = default;

    static G fromNative(const native::Grid& grid);
};

struct Pair
{
    G in;
    G out;
};

using Predictor = std::function<std::optional<G>(const G&)>;

struct Learner
{
    std::string name;
    double complexity;
    std::function<std::optional<Predictor>(const std::vector<Pair>&)> learn;
    std::function<std::string(const std::vector<Pair>&)> explain = {};
    struct Learned
    {
        Predictor predict;
        std::string name;
        double complexity;
    };
    std::function<std::optional<Learned>(const std::vector<Pair>&)> learnFull = {};
};

struct Candidate
{
    std::string name;
    double complexity;
    bool generalizes;
    Predictor predict;
    std::string detail = {};
};

class ProgramSearch
{
public:
    ProgramSearch();

    std::vector<Candidate> search(const std::vector<Pair>& train) const;
    static std::vector<std::pair<std::string, G>> attempts(const std::vector<Candidate>& ranked, const G& testInput, size_t maxAttempts = 2);
    std::vector<std::pair<std::string, double>> families() const;

private:
    std::vector<Learner> m_learners;
};

void loadPixelConceptPrior(const std::map<std::string, int>& counts);

struct SearchLimits
{
    int depth        = 3;
    size_t beam      = 500;
    size_t finishers = 2;
};
SearchLimits searchLimits();
void setSearchLimits(const SearchLimits& limits);

std::vector<std::pair<std::string, double>> pixelFeatureVocabulary();
std::vector<std::string> composeStepVocabulary();

} // namespace search
} // namespace arc
} // namespace infocell
