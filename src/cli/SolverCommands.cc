#include "SolverCommands.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "arc/ObjectRules.h"
#include "arc/Perception.h"
#include "arc/ProgramSearch.h"
#include "arc/Task.h"
#include "Config.h"
#include "grammar/PatternInduction.h"

namespace fs = std::filesystem;

namespace infocell {
namespace cli {

static std::map<std::string, int> readConcepts(const std::string& file)
{
    std::map<std::string, int> counts;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const size_t tab = line.find('\t');
        if (tab != std::string::npos) counts[line.substr(tab + 1)] += std::stoi(line.substr(0, tab));
    }
    return counts;
}

static int inducePattern(const std::string& file, bool json)
{
    std::ifstream in(file);
    if (!in) {
        std::cerr << "cannot open " << file << "\n";
        return EXIT_FAILURE;
    }
    std::vector<std::string> examples;
    for (std::string line; std::getline(in, line);) {
        line.erase(std::remove_if(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c); }), line.end());
        if (!line.empty() && line[0] != '#')
            examples.push_back(line);
    }
    auto r = grammar::induce(examples);
    if (json) {
        nlohmann::json j = { { "examples", examples }, { "candidates", r.candidates }, { "found", r.found && !r.names.empty() } };
        if (r.found && !r.names.empty()) {
            j["pattern"] = r.patternStr();
            for (size_t v = 0; v < r.names.size(); ++v) j["variables"].push_back({ { "name", r.names[v] }, { "alphabet", r.domains[v] } });
            j["axiomSchema"] = r.axiomSchema();
            std::vector<std::string> schemas;
            std::istringstream lines(r.ruleSchema());
            for (std::string line; std::getline(lines, line);) {
                line.erase(0, line.find_first_not_of(' '));
                schemas.push_back(line);
            }
            j["ruleSchemas"] = schemas;
            j["axioms"]      = r.axioms();
            j["rules"]       = r.rules();
            j["bits"]        = { { "model", r.modelBits }, { "data", r.dataBits }, { "listing", r.rawBits } };
            std::set<std::string> known(examples.begin(), examples.end());
            std::vector<std::string> derived;
            for (const auto& g : r.generate(64, 100000))
                if (!known.count(g) && derived.size() < 12) derived.push_back(g);
            j["derived"] = derived;
        }
        std::cout << j.dump() << std::endl;
        return EXIT_SUCCESS;
    }
    std::cout << examples.size() << " examples, " << r.candidates << " patterns tried\n";
    if (!r.found || r.names.empty()) {
        std::cout << "No pattern with variables fits all examples.\n";
        return EXIT_SUCCESS;
    }
    std::cout << "\nPattern:       " << r.patternStr() << "\n";
    for (size_t v = 0; v < r.names.size(); ++v)
        std::cout << "               " << r.names[v] << " = any non-empty string of {" << r.domains[v] << "}\n";
    std::cout << "\nAxiom schema:  " << r.axiomSchema() << "\n";
    std::cout << "Rule schema:   " << r.ruleSchema() << "\n";
    std::cout << "\nOr with concrete axioms:\n";
    for (const auto& a : r.axioms())
        std::cout << "  axiom  " << a << "\n";
    for (const auto& rule : r.rules())
        std::cout << "  rule   " << rule << "\n";
    std::cout << "\nDescription length: " << r.modelBits << " + " << r.dataBits << " bits (listing them: " << r.rawBits << " bits)\n";
    std::set<std::string> known(examples.begin(), examples.end());
    std::cout << "New strings it derives:";
    int shown = 0;
    for (const auto& s : r.generate(64, 100000))
        if (!known.count(s) && shown < 8) {
            std::cout << "  " << s;
            ++shown;
        }
    std::cout << "\n";
    return EXIT_SUCCESS;
}

static arc::search::G taskGrid(const nlohmann::json& jsonGrid)
{
    return arc::search::G::fromNative(arc::native::Grid("grid", to_string(jsonGrid)));
}

static nlohmann::json gridJson(const arc::search::G& g)
{
    nlohmann::json rows = nlohmann::json::array();
    for (int y = 0; y < g.h; ++y) {
        nlohmann::json row = nlohmann::json::array();
        for (int x = 0; x < g.w; ++x) row.push_back(g.at(x, y));
        rows.push_back(row);
    }
    return rows;
}

static nlohmann::json optionalGrid(const std::optional<arc::search::G>& g)
{
    return g ? gridJson(*g) : nlohmann::json();
}

static nlohmann::json limitsJson()
{
    const auto l = arc::search::searchLimits();
    return { { "depth", l.depth }, { "beam", l.beam }, { "finishers", l.finishers } };
}

static double secondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

static int explainPartial(const std::string& file, size_t topN)
{
    using namespace arc::search;
    const auto start          = std::chrono::steady_clock::now();
    const nlohmann::json task = nlohmann::json::parse(std::ifstream(file));
    std::vector<Pair> train;
    for (const auto& example : task.at("train")) train.push_back({ taskGrid(example.at("input")), taskGrid(example.at("output")) });
    std::vector<G> tests;
    for (const auto& test : task.at("test")) tests.push_back(taskGrid(test.at("input")));

    struct Partial
    {
        Candidate c;
        size_t learnedFrom;
        int fits        = 0;
        double accuracy = 0;
        nlohmann::json outputs = nlohmann::json::array();
    };
    const ProgramSearch programSearch;
    std::vector<Partial> found;
    std::set<std::string> seen;
    for (size_t i = 0; i < train.size(); ++i) {
        const auto ranked = programSearch.search({ train[i] });
        size_t taken = 0;
        for (const Candidate& c : ranked) {
            if (taken == 8) break;
            if (!seen.insert(c.name).second) continue;
            ++taken;
            Partial p { c, i };
            for (const Pair& pair : train) {
                const auto out = c.predict(pair.in);
                const bool fit = out && *out == pair.out;
                double same    = 0;
                if (out && out->w == pair.out.w && out->h == pair.out.h && !out->p.empty()) {
                    for (size_t q = 0; q < out->p.size(); ++q) same += out->p[q] == pair.out.p[q];
                    same /= static_cast<double>(out->p.size());
                }
                p.fits += fit;
                p.accuracy += same / static_cast<double>(train.size());
                p.outputs.push_back({ { "grid", optionalGrid(out) }, { "fits", fit }, { "accuracy", same } });
            }
            found.push_back(std::move(p));
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Partial& a, const Partial& b) {
        return std::tuple(-a.fits, -a.accuracy, a.c.complexity) < std::tuple(-b.fits, -b.accuracy, b.c.complexity);
    });
    nlohmann::json list = nlohmann::json::array();
    for (size_t i = 0; i < found.size() && i < topN; ++i) {
        const Partial& p = found[i];
        nlohmann::json j = { { "name", p.c.name }, { "detail", p.c.detail }, { "complexity", p.c.complexity }, { "learnedFrom", p.learnedFrom + 1 },
                             { "fits", p.fits }, { "accuracy", p.accuracy }, { "train", p.outputs }, { "test", nlohmann::json::array() } };
        for (const G& t : tests) j["test"].push_back(optionalGrid(p.c.predict(t)));
        list.push_back(j);
    }
    std::cout << nlohmann::json { { "partial", list }, { "limits", limitsJson() }, { "seconds", secondsSince(start) } }.dump() << std::endl;
    return EXIT_SUCCESS;
}

static int explainTask(const std::string& file, size_t topN)
{
    using namespace arc::search;
    const auto start          = std::chrono::steady_clock::now();
    const nlohmann::json task = nlohmann::json::parse(std::ifstream(file));

    nlohmann::json ret;
    std::vector<Pair> train;
    for (const auto& example : task.at("train")) {
        Pair p { taskGrid(example.at("input")), taskGrid(example.at("output")) };
        ret["train"].push_back({ { "input", gridJson(p.in) }, { "output", gridJson(p.out) } });
        train.push_back(std::move(p));
    }
    std::vector<G> testIn;
    std::vector<std::optional<G>> testOut;
    for (const auto& test : task.at("test")) {
        testIn.push_back(taskGrid(test.at("input")));
        testOut.push_back(test.contains("output") ? std::optional<G>(taskGrid(test.at("output"))) : std::nullopt);
    }

    const ProgramSearch programSearch;
    const std::vector<Candidate> ranked = programSearch.search(train);

    bool anyAttempt = false, allCorrect = true, allKnown = true;
    for (size_t i = 0; i < testIn.size(); ++i) {
        nlohmann::json t = { { "input", gridJson(testIn[i]) }, { "attempts", nlohmann::json::array() } };
        if (testOut[i]) t["output"] = gridJson(*testOut[i]);
        bool hit = false;
        for (const auto& [name, grid] : ProgramSearch::attempts(ranked, testIn[i])) {
            anyAttempt = true;
            hit        = hit || (testOut[i] && grid == *testOut[i]);
            t["attempts"].push_back({ { "name", name }, { "grid", gridJson(grid) }, { "correct", testOut[i] ? nlohmann::json(grid == *testOut[i]) : nlohmann::json() } });
        }
        allKnown   = allKnown && testOut[i].has_value();
        allCorrect = allCorrect && hit;
        ret["test"].push_back(t);
    }

    ret["candidates"] = nlohmann::json::array();
    for (size_t i = 0; i < ranked.size() && i < topN; ++i) {
        nlohmann::json c = { { "rank", i + 1 }, { "name", ranked[i].name }, { "complexity", ranked[i].complexity },
                             { "generalizes", ranked[i].generalizes }, { "detail", ranked[i].detail }, { "test", nlohmann::json::array() } };
        for (const G& in : testIn) c["test"].push_back(optionalGrid(ranked[i].predict(in)));
        ret["candidates"].push_back(c);
    }
    ret["programCount"] = ranked.size();
    ret["status"]       = !anyAttempt ? "no program" : (!allKnown ? "unknown" : (allCorrect ? "solved" : "wrong"));
    ret["limits"]       = limitsJson();
    ret["seconds"]      = secondsSince(start);
    std::cout << ret.dump() << std::endl;
    return EXIT_SUCCESS;
}

static int printVocabulary()
{
    using namespace arc::search;
    const auto ov = objectVocabulary();
    nlohmann::json solver;
    solver["objectAtoms"]     = ov.atoms;
    solver["objectRelations"] = ov.relations;
    solver["parses"]          = ov.parses;
    for (auto& [name, bits] : ov.formulas) solver["formulas"].push_back({ { "name", name }, { "bits", bits } });
    for (auto& [name, values] : ov.components) solver["fate"].push_back({ { "component", name }, { "values", values } });
    for (auto& [name, cost] : pixelFeatureVocabulary()) solver["pixelFeatures"].push_back({ { "name", name }, { "cost", cost } });
    solver["composeSteps"] = composeStepVocabulary();
    for (auto& [name, complexity] : ProgramSearch().families()) solver["families"].push_back({ { "name", name }, { "complexity", complexity } });
    std::cout << solver.dump() << std::endl;
    return EXIT_SUCCESS;
}

static int solveTasks(const std::string& pathStr, const std::string& taskId, bool verbose, const std::string& conceptsFile, const std::string& learnFile)
{
    if (!conceptsFile.empty()) {
        const size_t known = arc::search::loadConceptPrior(readConcepts(conceptsFile));
        std::cout << "Concept prior: " << known << " learned concepts from " << conceptsFile << std::endl;
    }
    using namespace arc::search;
    std::vector<fs::path> taskFiles;
    fs::path path(pathStr);
    if (fs::is_directory(path)) {
        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.path().extension() == ".json" && (taskId.empty() || entry.path().stem() == taskId)) {
                taskFiles.push_back(entry.path());
            }
        }
    } else {
        taskFiles.push_back(path);
    }
    std::sort(taskFiles.begin(), taskFiles.end());

    auto toGrid = [](const nlohmann::json& jsonGrid) {
        return G::fromNative(arc::native::Grid("grid", to_string(jsonGrid)));
    };

    const ProgramSearch programSearch;
    struct Result
    {
        std::string id;
        enum { Solved, Wrong, NoHypothesis } status = NoHypothesis;
        std::string winner;
        std::string bestGuess;
        size_t candidates = 0;
    };
    auto evaluate = [&](const fs::path& taskFile) {
        Result result;
        const nlohmann::json task = nlohmann::json::parse(std::ifstream(taskFile));
        result.id                 = taskFile.stem().string();

        std::vector<Pair> train;
        for (const auto& example : task.at("train")) {
            train.push_back({ toGrid(example.at("input")), toGrid(example.at("output")) });
        }
        const std::vector<Candidate> ranked = programSearch.search(train);
        result.bestGuess                    = ranked.empty() ? "-" : ranked[0].name;
        result.candidates                   = ranked.size();
        for (size_t i = 0; i < ranked.size() && i < 3; ++i) {
            result.bestGuess += (i ? " | " : ": ") + ranked[i].name;
        }

        bool anyAttempt = false, allCorrect = true;
        for (const auto& test : task.at("test")) {
            auto attempts = ProgramSearch::attempts(ranked, toGrid(test.at("input")));
            anyAttempt    = anyAttempt || !attempts.empty();
            if (!test.contains("output")) {
                continue;
            }
            const G expected = toGrid(test.at("output"));
            auto hit         = std::find_if(attempts.begin(), attempts.end(), [&](auto& a) { return a.second == expected; });
            if (hit == attempts.end()) {
                allCorrect = false;
            } else if (result.winner.empty()) {
                result.winner = hit->first;
            }
        }
        result.status = !anyAttempt ? Result::NoHypothesis : (allCorrect ? Result::Solved : Result::Wrong);
        if (verbose && result.status == Result::Wrong) {
            for (size_t i = 0; i < ranked.size(); ++i) {
                bool all = true;
                for (const auto& test : task.at("test")) {
                    auto out = ranked[i].predict(toGrid(test.at("input")));
                    all      = all && test.contains("output") && out && *out == toGrid(test.at("output"));
                }
                if (all) {
                    result.bestGuess += " [a correct program was at rank " + std::to_string(i + 1) + ": " + ranked[i].name + "]";
                    break;
                }
            }
        }
        return result;
    };
    std::vector<Result> results(taskFiles.size());
    std::atomic<size_t> next { 0 };
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); ++t) {
        workers.emplace_back([&]() {
            for (size_t i = next++; i < taskFiles.size(); i = next++) {
                results[i] = evaluate(taskFiles[i]);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    std::map<std::string, int> solvedBy;
    int solved = 0, wrong = 0, noHypothesis = 0;
    for (const Result& r : results) {
        if (r.status == Result::Solved) {
            ++solved;
            ++solvedBy[r.winner];
            std::cout << r.id << "  SOLVED by: " << r.winner << std::endl;
        } else if (r.status == Result::Wrong) {
            ++wrong;
            if (verbose) {
                std::cout << r.id << "  wrong, best guess: " << r.bestGuess << std::endl;
            }
        } else {
            ++noHypothesis;
            if (verbose) {
                std::cout << r.id << "  no hypothesis (" << r.candidates << " programs fit the training pairs, none answers the test) " << r.bestGuess << std::endl;
            }
        }
    }
    std::cout << "\nSolved by family:\n";
    for (const auto& [name, count] : solvedBy) {
        std::cout << "  " << count << "  " << name << "\n";
    }
    std::cout << "\nScore: " << solved << " / " << taskFiles.size() << " solved, "
              << wrong << " wrong, " << noHypothesis << " without hypothesis" << std::endl;
    if (!learnFile.empty()) {
        std::map<std::string, int> counts = readConcepts(learnFile);
        for (const Result& r : results) {
            if (r.status != Result::Solved) continue;
            for (const std::string& c : arc::search::conceptsIn(r.winner)) ++counts[c];
        }
        std::ofstream out(learnFile);
        for (const auto& [name, n] : counts) out << n << "\t" << name << "\n";
        std::cout << "Learned concepts written to " << learnFile << " (" << counts.size() << " concepts)" << std::endl;
    }
    return solved;
}

void addSolverCommands(CLI::App& app)
{
    static std::string solvePath = (fs::path(INFOCELL_ARC1_DATA_PATH) / "training").string();
    static std::string solveTaskId, solveConcepts, solveLearn;
    static bool solveVerbose = false;
    auto solve = app.add_subcommand("solve", "Learn symbolic rules from the training pairs and predict the test outputs");
    solve->add_option("-p,--path", solvePath, "Task .json file or folder of task files")->capture_default_str();
    solve->add_option("-i,--task-id", solveTaskId, "Only this task id (file name without .json)");
    solve->add_flag("-v,--verbose", solveVerbose, "Print unsolved tasks and their rules too");
    solve->add_option("-c,--concepts", solveConcepts, "Concept prior file learned from training tasks (see --learn-concepts)");
    solve->add_option("-l,--learn-concepts", solveLearn, "Add the formulas of the solved tasks to this file (training sets only)");
    solve->callback([]() { solveTasks(solvePath, solveTaskId, solveVerbose, solveConcepts, solveLearn); });

    static std::string grammarFile;
    auto grammar = app.add_subcommand("grammar", "Induce an axiom and generator schema from a file of valid strings (one per line)");
    static bool grammarJson = false;
    grammar->add_option("file", grammarFile, "Text file, one valid string per line ('#' starts a comment)")->required();
    grammar->add_flag("--json", grammarJson, "Print the result as JSON");
    grammar->callback([]() { inducePattern(grammarFile, grammarJson); });

    static std::string explainFile;
    static size_t explainTop       = 15;
    static bool explainPartialMode = false;
    static arc::search::SearchLimits limits = arc::search::searchLimits();
    auto explain = app.add_subcommand("explain", "Solve one task and print the programs and their test answers as JSON");
    explain->add_option("file", explainFile, "Task .json file")->required()->check(CLI::ExistingFile);
    explain->add_option("-n,--top", explainTop, "Number of ranked programs to include")->capture_default_str();
    explain->add_flag("--partial", explainPartialMode, "Programs learned from single training pairs, scored on all pairs");
    explain->add_option("--depth", limits.depth, "Composition: longest chain of steps")->check(CLI::Range(1, 50))->capture_default_str();
    explain->add_option("--beam", limits.beam, "Composition: states kept per level")->check(CLI::Range(1, 20000))->capture_default_str();
    explain->add_option("--finishers", limits.finishers, "Composition: closest states that get a learned last step")->check(CLI::Range(0, 100))->capture_default_str();
    explain->callback([]() {
        arc::search::setSearchLimits(limits);
        explainPartialMode ? explainPartial(explainFile, explainTop) : explainTask(explainFile, explainTop);
    });

    auto vocabulary = app.add_subcommand("vocabulary", "Print the solver's perception vocabulary as JSON");
    vocabulary->callback([]() { printVocabulary(); });
}

} // namespace cli
} // namespace infocell
