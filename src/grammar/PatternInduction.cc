#include "PatternInduction.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>

namespace infocell {
namespace grammar {

namespace {

double lengthBits(size_t n)
{
    return 2.0 * std::floor(std::log2(double(n))) + 1.0;
}

struct Span
{
    size_t start = 0;
    size_t len = 0;
};

void matchAll(const std::vector<Token>& p, int vars, const std::string& s, const std::vector<std::string>* domains,
    size_t limit, const std::function<void(const std::vector<Span>&)>& found)
{
    std::vector<Span> bind(vars);
    std::vector<bool> bound(vars, false);
    size_t count = 0;
    std::function<void(size_t, size_t)> rec = [&](size_t ti, size_t si) {
        if (count >= limit)
            return;
        if (ti == p.size()) {
            if (si == s.size()) {
                ++count;
                found(bind);
            }
            return;
        }
        size_t rest = p.size() - ti;
        if (s.size() - si < rest)
            return;
        const Token& t = p[ti];
        if (!t.isVar) {
            if (s[si] == t.c)
                rec(ti + 1, si + 1);
            return;
        }
        if (bound[t.v]) {
            const Span& b = bind[t.v];
            if (si + b.len <= s.size() && s.compare(si, b.len, s, b.start, b.len) == 0)
                rec(ti + 1, si + b.len);
            return;
        }
        size_t maxLen = s.size() - si - (rest - 1);
        for (size_t len = 1; len <= maxLen; ++len) {
            if (domains && (*domains)[t.v].find(s[si + len - 1]) == std::string::npos)
                break;
            bound[t.v] = true;
            bind[t.v] = {si, len};
            rec(ti + 1, si + len);
            bound[t.v] = false;
        }
    };
    rec(0, 0);
}

std::string chars(const std::string& s)
{
    std::set<char> set(s.begin(), s.end());
    return std::string(set.begin(), set.end());
}

std::string render(const std::vector<Token>& p, const std::vector<std::string>& names, const std::vector<std::string>& subst)
{
    std::string out;
    for (const Token& t : p) {
        if (!out.empty())
            out += ' ';
        if (!t.isVar)
            out += t.c;
        else
            out += subst[t.v];
    }
    return out;
}

} // namespace

std::string Induction::patternStr() const
{
    return render(pattern, names, names);
}

std::string Induction::axiomSchema() const
{
    std::string out;
    int last = int(names.size()) - 1;
    for (char c : domains[last]) {
        std::vector<std::string> subst = names;
        subst[last] = std::string(1, c);
        if (!out.empty())
            out += "   |   ";
        out += render(pattern, names, subst);
    }
    return out;
}

std::string Induction::ruleSchema() const
{
    std::string out;
    int last = int(names.size()) - 1;
    for (char c : domains[last]) {
        std::vector<std::string> subst = names;
        subst[last] = names[last] + c;
        if (!out.empty())
            out += "\n               ";
        out += patternStr() + "  ->  " + render(pattern, names, subst);
    }
    return out;
}

std::vector<std::string> Induction::axioms() const
{
    std::vector<std::string> out;
    std::vector<std::string> subst(names.size());
    std::function<void(size_t)> rec = [&](size_t v) {
        if (out.size() >= 16)
            return;
        if (v == names.size()) {
            std::string s;
            for (const Token& t : pattern)
                s += t.isVar ? subst[t.v] : std::string(1, t.c);
            out.push_back(s);
            return;
        }
        for (char c : domains[v]) {
            subst[v] = std::string(1, c);
            rec(v + 1);
        }
    };
    rec(0);
    return out;
}

std::vector<std::string> Induction::rules() const
{
    std::vector<std::string> out;
    for (size_t v = 0; v < names.size(); ++v)
        for (char c : domains[v]) {
            std::vector<std::string> subst = names;
            subst[v] = names[v] + c;
            out.push_back(patternStr() + "  ->  " + render(pattern, names, subst));
        }
    return out;
}

std::vector<std::string> Induction::generate(size_t maxLength, size_t maxCount) const
{
    std::set<std::vector<std::string>> seen;
    std::vector<std::vector<std::string>> frontier;
    std::vector<std::string> subst(names.size());
    std::function<void(size_t)> start = [&](size_t v) {
        if (v == names.size()) {
            if (seen.insert(subst).second)
                frontier.push_back(subst);
            return;
        }
        for (char c : domains[v]) {
            subst[v] = std::string(1, c);
            start(v + 1);
        }
    };
    start(0);
    auto instance = [&](const std::vector<std::string>& b) {
        std::string s;
        for (const Token& t : pattern)
            s += t.isVar ? b[t.v] : std::string(1, t.c);
        return s;
    };
    std::set<std::pair<size_t, std::string>> out;
    while (!frontier.empty() && seen.size() < 200000) {
        std::vector<std::vector<std::string>> next;
        for (const auto& b : frontier) {
            std::string s = instance(b);
            if (s.size() > maxLength)
                continue;
            out.insert({s.size(), s});
            for (size_t v = 0; v < names.size(); ++v)
                for (char c : domains[v]) {
                    auto n = b;
                    n[v] += c;
                    if (seen.insert(n).second)
                        next.push_back(n);
                }
        }
        frontier.swap(next);
    }
    std::vector<std::string> list;
    for (const auto& [len, s] : out) {
        if (list.size() >= maxCount)
            break;
        list.push_back(s);
    }
    return list;
}

bool Induction::fits(const std::string& s) const
{
    bool ok = false;
    matchAll(pattern, int(names.size()), s, &domains, 1, [&](const std::vector<Span>&) { ok = true; });
    return ok;
}

Induction induce(const std::vector<std::string>& input, int maxVars)
{
    Induction best;
    std::vector<std::string> ex = input;
    std::sort(ex.begin(), ex.end(), [](const auto& a, const auto& b) { return a.size() != b.size() ? a.size() < b.size() : a < b; });
    ex.erase(std::unique(ex.begin(), ex.end()), ex.end());
    if (ex.empty() || ex.front().empty())
        return best;

    std::string sigma;
    for (const auto& s : ex)
        sigma += s;
    sigma = chars(sigma);
    for (const auto& s : ex)
        best.rawBits += lengthBits(s.size()) + double(s.size()) * std::log2(double(std::max<size_t>(sigma.size(), 2)));

    double bestTotal = std::numeric_limits<double>::infinity();
    const std::string& s0 = ex.front();

    std::vector<std::vector<size_t>> ptr(1, std::vector<size_t>(ex.size(), 0));

    std::vector<Token> tokens;
    std::vector<std::string> bindings;

    auto evaluate = [&]() {
        ++best.candidates;
        int vars = int(bindings.size());
        std::vector<std::vector<std::string>> parses;
        for (const auto& s : ex) {
            double cheapest = std::numeric_limits<double>::infinity();
            std::vector<std::string> chosen;
            matchAll(tokens, vars, s, nullptr, 256, [&](const std::vector<Span>& b) {
                double bits = 0;
                for (const Span& sp : b) {
                    std::string sub = s.substr(sp.start, sp.len);
                    bits += lengthBits(sp.len) + double(sp.len) * std::log2(double(chars(sub).size()));
                }
                if (bits < cheapest) {
                    cheapest = bits;
                    chosen.clear();
                    for (const Span& sp : b)
                        chosen.push_back(s.substr(sp.start, sp.len));
                }
            });
            if (chosen.empty() && vars > 0)
                return;
            if (vars == 0 && s != s0)
                return;
            parses.push_back(chosen);
        }
        std::vector<std::string> domains(vars);
        for (const auto& parse : parses)
            for (int v = 0; v < vars; ++v)
                domains[v] = chars(domains[v] + parse[v]);
        double data = 0;
        for (const auto& parse : parses)
            for (int v = 0; v < vars; ++v)
                data += lengthBits(parse[v].size()) + double(parse[v].size()) * std::log2(double(domains[v].size()));
        double model = lengthBits(tokens.size()) + double(vars) * double(sigma.size());
        int seenVars = 0;
        for (const Token& t : tokens) {
            model += std::log2(double(sigma.size() + seenVars + 1));
            if (t.isVar && t.v == seenVars)
                ++seenVars;
        }
        double total = model + data;
        if (total < bestTotal - 1e-9) {
            bestTotal = total;
            best.found = true;
            best.pattern = tokens;
            best.domains = domains;
            best.modelBits = model;
            best.dataBits = data;
        }
    };

    std::function<void(size_t)> dfs = [&](size_t i) {
        if (best.candidates >= 3000000)
            return;
        if (i == s0.size()) {
            evaluate();
            return;
        }
        {
            const auto& cur = ptr.back();
            std::vector<size_t> nxt(ex.size());
            bool ok = true;
            for (size_t e = 0; e < ex.size() && ok; ++e) {
                size_t f = ex[e].find(s0[i], cur[e]);
                ok = f != std::string::npos;
                nxt[e] = f + 1;
            }
            if (ok) {
                ptr.push_back(nxt);
                tokens.push_back({false, s0[i], 0});
                dfs(i + 1);
                tokens.pop_back();
                ptr.pop_back();
            }
        }
        for (size_t j = i + 1; j <= s0.size(); ++j) {
            std::string sub = s0.substr(i, j - i);
            for (size_t v = 0; v < bindings.size(); ++v)
                if (bindings[v] == sub) {
                    tokens.push_back({true, 0, int(v)});
                    dfs(j);
                    tokens.pop_back();
                }
            if (int(bindings.size()) < maxVars) {
                tokens.push_back({true, 0, int(bindings.size())});
                bindings.push_back(sub);
                dfs(j);
                bindings.pop_back();
                tokens.pop_back();
            }
        }
    };
    dfs(0);

    int vars = int(best.domains.size());
    for (char c : std::string("xyzwuvstabcdefghijklmnopqr")) {
        if (int(best.names.size()) == vars)
            break;
        if (sigma.find(c) == std::string::npos)
            best.names.push_back(std::string(1, c));
    }
    while (int(best.names.size()) < vars)
        best.names.push_back("V" + std::to_string(best.names.size()));
    return best;
}

} // namespace grammar
} // namespace infocell
