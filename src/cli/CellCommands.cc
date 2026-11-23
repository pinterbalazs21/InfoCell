#include "CellCommands.h"

#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "cells/ToolFinder.h"
#include "cells/World.h"
#include "util/Log.h"

namespace infocell {
namespace cli {

using namespace infocell::cells;

namespace {

void silenceLoggers()
{
    spdlog::apply_all([](std::shared_ptr<spdlog::logger> l) { l->set_level(spdlog::level::off); });
}

nlohmann::json typesJson(World& w)
{
    nlohmann::json types = nlohmann::json::array();
    for (CellI& kv : w.arcLib().structs()[w.id.list]) {
        nlohmann::json j = { { "members", nlohmann::json::array() }, { "memberOf", nlohmann::json::array() }, { "methods", nlohmann::json::array() } };
        auto part = [&](const char* what, auto&& fn) {
            try {
                fn();
            } catch (const char* e) {
                j["errors"].push_back(std::string(what) + ": " + e);
            } catch (const std::exception& e) {
                j["errors"].push_back(std::string(what) + ": " + e.what());
            } catch (...) {
                j["errors"].push_back(std::string(what) + ": unknown");
            }
        };
        CellI* tp = nullptr;
        part("name", [&]() { j["name"] = kv[w.id.key].label(); tp = &kv[w.id.value]; j["enum"] = tp->has(w.id.enum_); });
        if (!tp) {
            types.push_back(j);
            continue;
        }
        CellI& t = *tp;
        part("members", [&]() {
            if (!t.has(w.id.members)) return;
            for (CellI& mkv : t[w.id.members][w.id.list]) {
                CellI& m = mkv.has(w.id.value) ? mkv[w.id.value] : mkv;
                nlohmann::json mj = { { "name", m.has(w.id.name) ? m[w.id.name].label() : (mkv.has(w.id.key) ? mkv[w.id.key].label() : m.label()) },
                                      { "type", m.has(w.id.type) ? m[w.id.type].label() : "" } };
                if (m.has(w.id.relation)) mj["relation"] = m[w.id.relation].label();
                if (m.has(w.id.role)) mj["role"] = m[w.id.role].label();
                j["members"].push_back(mj);
            }
        });
        part("memberOf", [&]() {
            if (!t.has(w.id.memberOf)) return;
            for (CellI& mkv : t[w.id.memberOf][w.id.list]) j["memberOf"].push_back((mkv.has(w.id.key) ? mkv[w.id.key] : mkv).label());
        });
        part("methods", [&]() {
            if (!t.has(w.id.methods)) return;
            for (CellI& mkv : t[w.id.methods][w.id.list]) {
                nlohmann::json fj = { { "name", (mkv.has(w.id.key) ? mkv[w.id.key] : mkv).label() }, { "parameters", nlohmann::json::array() } };
                CellI& f = mkv.has(w.id.value) ? mkv[w.id.value] : mkv;
                if (f.has(w.id.parameters) && f[w.id.parameters].has(w.id.list))
                    for (CellI& pkv : f[w.id.parameters][w.id.list]) {
                        CellI& q = pkv.has(w.id.value) ? pkv[w.id.value] : pkv;
                        fj["parameters"].push_back({ { "name", q.has(w.id.name) ? q[w.id.name].label() : q.label() }, { "type", q.has(w.id.type) ? q[w.id.type].label() : "" } });
                    }
                j["methods"].push_back(fj);
            }
        });
        types.push_back(j);
    }
    return types;
}

struct Tool
{
    std::string name;
    Object* fn = nullptr;
};

std::map<std::string, Tool> conversionTools(World& w, CellI& from, CellI& to)
{
    std::map<std::string, Tool> tools;
    for (CellI& kv : w.stdLib().toolFinder().findConversionTools(from, to)) {
        std::string name;
        for (CellI& part : kv[w.id.key]) name += (name.empty() ? "" : " ") + part.label();
        tools[name] = { name, &static_cast<Object&>(kv[w.id.value]) };
    }
    return tools;
}

CellI* apply(World& w, Object& fn, CellI& from)
{
    try {
        fn.createSelfStack({ "from", from });
        fn();
        return fn.has(w.id.value) ? &fn[w.id.value] : nullptr;
    } catch (...) {
        return nullptr;
    }
}

nlohmann::json findTools(const std::vector<std::pair<int, int>>& pairs, std::optional<int> test)
{
    World w(silenceLoggers);
    auto number = [&](int n) -> CellI& { return w.pools.numbers.get(n); };

    std::map<std::string, Tool> common;
    for (size_t i = 0; i < pairs.size(); ++i) {
        auto tools = conversionTools(w, number(pairs[i].first), number(pairs[i].second));
        if (i == 0) {
            common = std::move(tools);
            continue;
        }
        for (auto it = common.begin(); it != common.end();) {
            auto found = tools.find(it->first);
            if (found == tools.end()) {
                it = common.erase(it);
            } else {
                it->second.fn = found->second.fn;
                ++it;
            }
        }
    }

    nlohmann::json list = nlohmann::json::array();
    for (auto& [name, tool] : common) {
        bool fitsAll = true;
        for (auto& [from, to] : pairs) {
            CellI* r = apply(w, *tool.fn, number(from));
            fitsAll  = fitsAll && r == &number(to);
        }
        nlohmann::json j = { { "tool", name }, { "fitsAll", fitsAll } };
        if (test) {
            CellI* r    = apply(w, *tool.fn, number(*test));
            j["answer"] = r ? nlohmann::json(r->label()) : nlohmann::json();
        }
        list.push_back(j);
    }
    nlohmann::json pairsJson = nlohmann::json::array();
    for (auto& [from, to] : pairs) pairsJson.push_back({ from, to });
    return { { "pairs", pairsJson }, { "test", test ? nlohmann::json(*test) : nlohmann::json() }, { "tools", list } };
}

} // namespace

void addCellCommands(CLI::App& app)
{
    auto types = app.add_subcommand("types", "Print the compiled InfoCell types as JSON");
    types->callback([]() {
        World w(silenceLoggers);
        std::cout << nlohmann::json { { "types", typesJson(w) } }.dump() << std::endl;
    });

    static std::vector<int> sequence;
    static std::vector<std::string> pairs;
    static std::optional<int> test;
    auto toolfinder = app.add_subcommand("toolfinder", "Find the ToolFinder tools that explain number examples, as JSON");
    toolfinder->add_option("-s,--sequence", sequence, "A sequence: every step is an example, the last value is the test input");
    toolfinder->add_option("-p,--pairs", pairs, "Examples as from:to, e.g. 2:4 3:6");
    toolfinder->add_option("-t,--test", test, "The test input for --pairs");
    toolfinder->callback([]() {
        std::vector<std::pair<int, int>> examples;
        std::optional<int> testInput = test;
        if (!sequence.empty()) {
            for (size_t i = 0; i + 1 < sequence.size(); ++i) examples.push_back({ sequence[i], sequence[i + 1] });
            testInput = sequence.back();
        }
        for (const std::string& p : pairs) {
            const auto colon = p.find(':');
            if (colon == std::string::npos) throw CLI::ValidationError("--pairs", "expected from:to, got " + p);
            examples.push_back({ std::stoi(p.substr(0, colon)), std::stoi(p.substr(colon + 1)) });
        }
        if (examples.empty()) throw CLI::ValidationError("toolfinder", "give --sequence or --pairs");
        std::cout << findTools(examples, testInput).dump() << std::endl;
    });
}

} // namespace cli
} // namespace infocell
