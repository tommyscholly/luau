#include "lua.h"
#include "lualib.h"

#include "Luau/Ast.h"
#include "Luau/CodeGen.h"
#include "Luau/Compiler.h"
#include "Luau/Flags.h"
#include "Luau/ParseOptions.h"
#include "Luau/Parser.h"
#include "Luau/ParseResult.h"

#include "benchmark/benchmark.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct BenchScript
{
    std::string relativePath;
    std::string source;
};

struct BenchConfig
{
    int optimizationLevel = 2;
    int debugLevel = 1;
    bool useCodegen = false;
    std::string flagSet;
    std::string label;
};

struct ProgramOptions
{
    fs::path scriptsRoot = fs::path("bench") / "tests";
    std::optional<std::regex> scriptFilter;
    std::vector<std::string> phases{"parse", "compile", "vm"};
    std::vector<int> optimizationLevels{2};
    std::vector<int> debugLevels{1};
    std::vector<bool> codegenModes{false, true};
    std::vector<std::string> flagSets{""};
} gProgramOptions;

static std::string trim(std::string value)
{
    const auto begin = std::find_if_not(
        value.begin(),
        value.end(),
        [](unsigned char c)
        {
            return std::isspace(c) != 0;
        }
    );
    const auto end = std::find_if_not(
                         value.rbegin(),
                         value.rend(),
                         [](unsigned char c)
                         {
                             return std::isspace(c) != 0;
                         }
    ).base();

    if (begin >= end)
        return "";

    return std::string(begin, end);
}

static std::vector<std::string> split(const std::string& value, char delimiter)
{
    std::vector<std::string> out;
    std::stringstream ss(value);
    std::string item;

    while (std::getline(ss, item, delimiter))
    {
        std::string cleaned = trim(item);
        if (!cleaned.empty())
            out.push_back(cleaned);
    }

    return out;
}

static std::vector<int> parseIntList(const std::string& value)
{
    std::vector<int> out;
    for (const std::string& token : split(value, ','))
        out.push_back(std::stoi(token));
    return out;
}

static std::vector<std::string> parseStringList(const std::string& value, char delimiter)
{
    return split(value, delimiter);
}

static std::vector<bool> parseCodegenModes(const std::string& value)
{
    if (value == "off")
        return {false};
    if (value == "on")
        return {true};
    if (value == "both")
        return {false, true};

    throw std::runtime_error("Invalid --luau_codegen value. Use off|on|both.");
}

static bool contains(const std::vector<std::string>& values, const std::string& wanted)
{
    return std::find(values.begin(), values.end(), wanted) != values.end();
}

static void parseCustomArguments(int& argc, char** argv)
{
    std::vector<char*> forwarded;
    forwarded.reserve(static_cast<size_t>(argc));
    forwarded.push_back(argv[0]);

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        auto parseValue = [&arg](const char* prefix) -> std::optional<std::string>
        {
            const std::string pfx(prefix);
            if (arg.rfind(pfx, 0) == 0)
                return arg.substr(pfx.size());
            return std::nullopt;
        };

        if (auto v = parseValue("--luau_scripts_root="))
        {
            gProgramOptions.scriptsRoot = *v;
            continue;
        }
        if (auto v = parseValue("--luau_script_filter="))
        {
            gProgramOptions.scriptFilter = std::regex(*v);
            continue;
        }
        if (auto v = parseValue("--luau_phases="))
        {
            gProgramOptions.phases = parseStringList(*v, ',');
            continue;
        }
        if (auto v = parseValue("--luau_opt_levels="))
        {
            gProgramOptions.optimizationLevels = parseIntList(*v);
            continue;
        }
        if (auto v = parseValue("--luau_debug_levels="))
        {
            gProgramOptions.debugLevels = parseIntList(*v);
            continue;
        }
        if (auto v = parseValue("--luau_codegen="))
        {
            gProgramOptions.codegenModes = parseCodegenModes(*v);
            continue;
        }
        if (auto v = parseValue("--luau_flag_sets="))
        {
            gProgramOptions.flagSets = parseStringList(*v, ';');
            if (gProgramOptions.flagSets.empty())
                gProgramOptions.flagSets = {""};
            continue;
        }

        forwarded.push_back(argv[i]);
    }

    for (size_t i = 0; i < forwarded.size(); ++i)
        argv[i] = forwarded[i];
    argc = static_cast<int>(forwarded.size());
}

static std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Failed to read script: " + path.string());

    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static std::vector<BenchScript> discoverScripts()
{
    std::vector<BenchScript> scripts;

    if (!fs::exists(gProgramOptions.scriptsRoot))
        throw std::runtime_error("Script root does not exist: " + gProgramOptions.scriptsRoot.string());

    for (const auto& entry : fs::recursive_directory_iterator(gProgramOptions.scriptsRoot))
    {
        if (!entry.is_regular_file())
            continue;
        const fs::path extension = entry.path().extension();
        if (extension != ".lua" && extension != ".luau")
            continue;

        const std::string relative = fs::relative(entry.path(), gProgramOptions.scriptsRoot).generic_string();

        if (gProgramOptions.scriptFilter && !std::regex_search(relative, *gProgramOptions.scriptFilter))
            continue;

        scripts.push_back({relative, readFile(entry.path())});
    }

    std::sort(
        scripts.begin(),
        scripts.end(),
        [](const BenchScript& lhs, const BenchScript& rhs)
        {
            return lhs.relativePath < rhs.relativePath;
        }
    );

    return scripts;
}

static std::vector<BenchConfig> buildConfigs()
{
    std::vector<BenchConfig> out;

    for (int opt : gProgramOptions.optimizationLevels)
    {
        for (int dbg : gProgramOptions.debugLevels)
        {
            for (bool codegen : gProgramOptions.codegenModes)
            {
                for (size_t flagIndex = 0; flagIndex < gProgramOptions.flagSets.size(); ++flagIndex)
                {
                    BenchConfig cfg;
                    cfg.optimizationLevel = opt;
                    cfg.debugLevel = dbg;
                    cfg.useCodegen = codegen;
                    const std::string& flags = gProgramOptions.flagSets[flagIndex];
                    cfg.flagSet = flags;
                    cfg.label =
                        "o" + std::to_string(opt) + "_d" + std::to_string(dbg) + "_cg" + (codegen ? "on" : "off") + "_f" + std::to_string(flagIndex);
                    out.push_back(cfg);
                }
            }
        }
    }

    return out;
}

static void applyFlagSet(const std::string& flags)
{
    setLuauFlagsDefault();
    if (!flags.empty())
        setLuauFlags(flags.c_str());
}

static Luau::CompileOptions makeCompileOptions(const BenchConfig& cfg)
{
    Luau::CompileOptions options;
    options.optimizationLevel = cfg.optimizationLevel;
    options.debugLevel = cfg.debugLevel;
    options.typeInfoLevel = 0;
    return options;
}

static int luaBenchRunCode(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    lua_call(L, 0, 0);
    return 0;
}

static int luaBenchRequire(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);

    if (strcmp(name, "bench_support") == 0 || strcmp(name, "../bench_support") == 0)
    {
        lua_getglobal(L, "bench");
        return 1;
    }

    luaL_error(L, "module not found: %s", name);
    return 0;
}

static void installBenchShim(lua_State* L)
{
    lua_newtable(L);
    lua_pushcfunction(L, luaBenchRunCode, "runCode");
    lua_setfield(L, -2, "runCode");
    lua_setglobal(L, "bench");

    lua_pushcfunction(L, luaBenchRequire, "require");
    lua_setglobal(L, "require");
}

static void registerParseBench(const BenchScript& script, const BenchConfig& cfg)
{
    const std::string name = "ScriptPipeline/parse/" + cfg.label + "/" + script.relativePath;

    benchmark::RegisterBenchmark(
        name.c_str(),
        [script, cfg](benchmark::State& state)
        {
            applyFlagSet(cfg.flagSet);

            for ([[maybe_unused]] auto _ : state)
            {
                Luau::Allocator allocator;
                Luau::AstNameTable names{allocator};
                Luau::ParseResult result = Luau::Parser::parse(script.source.data(), script.source.size(), names, allocator, Luau::ParseOptions{});

                if (!result.errors.empty())
                {
                    state.SkipWithError(result.errors.front().what());
                    return;
                }

                benchmark::DoNotOptimize(result.root);
            }
        }
    )->Unit(benchmark::kMicrosecond);
}

static void registerCompileBench(const BenchScript& script, const BenchConfig& cfg)
{
    const std::string name = "ScriptPipeline/compile/" + cfg.label + "/" + script.relativePath;

    benchmark::RegisterBenchmark(
        name.c_str(),
        [script, cfg](benchmark::State& state)
        {
            applyFlagSet(cfg.flagSet);
            const Luau::CompileOptions options = makeCompileOptions(cfg);

            for ([[maybe_unused]] auto _ : state)
            {
                std::string bytecode = Luau::compile(script.source, options, Luau::ParseOptions{});
                benchmark::DoNotOptimize(bytecode.data());
                benchmark::DoNotOptimize(bytecode.size());
            }
        }
    )->Unit(benchmark::kMicrosecond);
}

static void registerVmBench(const BenchScript& script, const BenchConfig& cfg)
{
    const std::string name = "ScriptPipeline/vm/" + cfg.label + "/" + script.relativePath;

    benchmark::RegisterBenchmark(
        name.c_str(),
        [script, cfg](benchmark::State& state)
        {
            applyFlagSet(cfg.flagSet);

            const Luau::CompileOptions options = makeCompileOptions(cfg);
            const std::string bytecode = Luau::compile(script.source, options, Luau::ParseOptions{});

            if (bytecode.empty())
            {
                state.SkipWithError("Compile returned empty bytecode");
                return;
            }

            for ([[maybe_unused]] auto _ : state)
            {
                state.PauseTiming();
                lua_State* L = luaL_newstate();
                if (!L)
                {
                    state.SkipWithError("luaL_newstate failed");
                    return;
                }

                luaL_openlibs(L);
                installBenchShim(L);

                if (cfg.useCodegen && Luau::CodeGen::isSupported())
                    Luau::CodeGen::create(L);

                state.ResumeTiming();

                const int loadStatus = luau_load(L, script.relativePath.c_str(), bytecode.data(), bytecode.size(), 0);
                if (loadStatus != 0)
                {
                    const char* err = lua_tostring(L, -1);
                    std::string message = "luau_load failed";
                    if (err)
                        message += std::string(": ") + err;
                    state.SkipWithError(message.c_str());
                    lua_close(L);
                    return;
                }

                if (cfg.useCodegen && Luau::CodeGen::isSupported())
                    Luau::CodeGen::compile(L, -1, Luau::CodeGen::CodeGen_ColdFunctions);

                const int runStatus = lua_pcall(L, 0, 0, 0);
                if (runStatus != 0)
                {
                    if (runStatus == LUA_YIELD)
                    {
                        state.SkipWithError("Script yielded; benchmarks expect scripts to run to completion");
                    }
                    else
                    {
                        const char* err = lua_tostring(L, -1);
                        std::string message = "lua_pcall failed";
                        if (err)
                            message += std::string(": ") + err;
                        state.SkipWithError(message.c_str());
                    }
                    lua_close(L);
                    return;
                }

                state.PauseTiming();
                lua_close(L);
                state.ResumeTiming();
            }
        }
    )->Unit(benchmark::kMicrosecond);
}

static void registerScriptBenchmarks()
{
    if (!contains(gProgramOptions.phases, "parse") && !contains(gProgramOptions.phases, "compile") && !contains(gProgramOptions.phases, "vm"))
        throw std::runtime_error("No valid phases selected. Use --luau_phases=parse,compile,vm.");

    const std::vector<BenchScript> scripts = discoverScripts();
    const std::vector<BenchConfig> configs = buildConfigs();

    if (scripts.empty())
        throw std::runtime_error("No scripts matched the configured root/filter.");

    for (const BenchScript& script : scripts)
    {
        for (const BenchConfig& cfg : configs)
        {
            if (contains(gProgramOptions.phases, "parse"))
                registerParseBench(script, cfg);
            if (contains(gProgramOptions.phases, "compile"))
                registerCompileBench(script, cfg);
            if (contains(gProgramOptions.phases, "vm"))
                registerVmBench(script, cfg);
        }
    }

    benchmark::AddCustomContext("luau_scripts_root", gProgramOptions.scriptsRoot.generic_string());
    benchmark::AddCustomContext("luau_script_count", std::to_string(scripts.size()));
    benchmark::AddCustomContext("luau_config_count", std::to_string(configs.size()));
}

int main(int argc, char** argv)
{
    try
    {
        parseCustomArguments(argc, argv);
        registerScriptBenchmarks();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Benchmark setup error: " << e.what() << "\n";
        return 1;
    }

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
