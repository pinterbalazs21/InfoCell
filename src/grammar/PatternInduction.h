#pragma once

#include <string>
#include <vector>

namespace infocell {
namespace grammar {

struct Token
{
    bool isVar = false;
    char c = 0;
    int v = 0;
};

struct Induction
{
    bool found = false;
    std::vector<Token> pattern;
    std::vector<std::string> domains;
    std::vector<std::string> names;
    double modelBits = 0;
    double dataBits = 0;
    double rawBits = 0;
    size_t candidates = 0;

    std::string patternStr() const;
    std::string axiomSchema() const;
    std::string ruleSchema() const;
    std::vector<std::string> axioms() const;
    std::vector<std::string> rules() const;
    std::vector<std::string> generate(size_t maxLength, size_t maxCount) const;
    bool fits(const std::string& s) const;
};

Induction induce(const std::vector<std::string>& examples, int maxVars = 4);

} // namespace grammar
} // namespace infocell
