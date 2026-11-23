#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ProgramSearch.h"

namespace infocell {
namespace arc {
namespace search {

std::vector<Learner> objectRuleLearners();

std::vector<std::string> conceptsIn(const std::string& description);
size_t loadConceptPrior(const std::map<std::string, int>& counts);
std::vector<std::pair<std::string, double>> conceptLibrary(size_t top);

struct ObjectVocabulary
{
    std::vector<std::string> atoms;
    std::vector<std::string> relations;
    std::vector<std::string> parses;
    std::vector<std::pair<std::string, double>> formulas;
    std::vector<std::pair<std::string, std::vector<std::string>>> components;
};
ObjectVocabulary objectVocabulary();

} // namespace search
} // namespace arc
} // namespace infocell
