// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/BytecodeGraphVerification.h"
#include "Luau/Compiler.h"

#include "doctest.h"

#include <stdexcept>

using namespace Luau;
using namespace Luau::Bytecode;
using Status = BytecodeGraphVerificationStatus;
using VerifierKind = BytecodeGraphVerifierKind;

TEST_SUITE_BEGIN("BytecodeGraphVerification");

TEST_CASE("verifier_name_conversion")
{
    CHECK(parseBytecodeGraphVerifier("roundtrip") == VerifierKind::Roundtrip);
    CHECK(parseBytecodeGraphVerifier("use-consistency") == VerifierKind::UseConsistency);
    CHECK(parseBytecodeGraphVerifier("summary") == VerifierKind::Summary);
    CHECK(parseBytecodeGraphVerifier("all") == VerifierKind::All);
    CHECK_FALSE(parseBytecodeGraphVerifier("missing"));
    CHECK_FALSE(parseBytecodeGraphVerifier(""));
    CHECK_FALSE(parseBytecodeGraphVerifier("Roundtrip"));
    CHECK(std::string(bytecodeGraphVerifierName(VerifierKind::All)) == "all");
    for (const BytecodeGraphVerifier* verifier : getBytecodeGraphVerifiers())
    {
        CHECK(parseBytecodeGraphVerifier(bytecodeGraphVerifierName(verifier->kind)) == verifier->kind);
        CHECK(getBytecodeGraphVerifier(verifier->kind) == verifier);
    }
}

TEST_CASE("registry_selection")
{
    constexpr auto invalid = static_cast<VerifierKind>(-1);
    CHECK(getBytecodeGraphVerifier(invalid) == nullptr);
    CHECK(getBytecodeGraphVerifier(VerifierKind::All) == nullptr);
    CHECK_THROWS_AS(selectBytecodeGraphVerifiers({{invalid}, false}), std::invalid_argument);
    CHECK_THROWS_AS(bytecodeGraphVerifierName(invalid), std::invalid_argument);
    auto all = selectBytecodeGraphVerifiers({{VerifierKind::All}, false});
    REQUIRE(all.size() == 3);
    CHECK(all[0]->kind == VerifierKind::Roundtrip);
    CHECK(all[1]->kind == VerifierKind::UseConsistency);
    CHECK(all[2]->kind == VerifierKind::Summary);
    auto selected = selectBytecodeGraphVerifiers(
        {{VerifierKind::Summary, VerifierKind::UseConsistency, VerifierKind::Summary, VerifierKind::All, VerifierKind::Roundtrip}, false}
    );
    REQUIRE(selected.size() == 3);
    CHECK(selected[0]->kind == VerifierKind::Summary);
    CHECK(selected[1]->kind == VerifierKind::UseConsistency);
    CHECK(selected[2]->kind == VerifierKind::Roundtrip);
}

TEST_CASE("source_functions_and_order")
{
    BytecodeBuilder builder;
    compileOrThrow(builder, "local function choose(flag, a, b) if flag then return a else return b end end return choose(...)");
    auto results = verifyBytecodeGraphs(builder, {{VerifierKind::UseConsistency, VerifierKind::Roundtrip, VerifierKind::Summary}, true});
    REQUIRE(results.size() == builder.getFunctionCount());
    REQUIRE(results.size() == 2);
    CHECK(bytecodeGraphVerificationAccepted(results, true));
    for (size_t i = 0; i < results.size(); ++i)
    {
        CHECK(results[i].functionId == i);
        REQUIRE(results[i].results.size() == 3);
        CHECK(results[i].results[0].first == VerifierKind::UseConsistency);
        CHECK(results[i].results[1].first == VerifierKind::Roundtrip);
        CHECK(results[i].results[2].second.detail.find("reachable-blocks=") != std::string::npos);
    }
}

TEST_CASE("source_closure_dependencies")
{
    BytecodeBuilder builder;
    compileOrThrow(
        builder,
        "local function outer(a) return function(b) return function() return a + b end end end "
        "local function shared() return 1 end local value = 0 "
        "local function mutate() value += 1 return value end return outer, shared, mutate"
    );
    auto results = verifyBytecodeGraphs(builder, {{VerifierKind::Roundtrip, VerifierKind::UseConsistency}, true});
    CHECK(results.size() == builder.getFunctionCount());
    CHECK(bytecodeGraphVerificationAccepted(results, true));
}

TEST_CASE("graph_parse_failure_is_reported")
{
    std::string source = "local value = ...\n";
    for (size_t i = 0; i < 1100; ++i)
        source += "if value then print(value) end\n";
    BytecodeBuilder builder;
    compileOrThrow(builder, source);
    auto results = verifyBytecodeGraphs(builder, {{VerifierKind::Roundtrip, VerifierKind::UseConsistency}, false});
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].results.size() == 2);
    for (const auto& [kind, result] : results[0].results)
    {
        CHECK(result.status == Status::Fail);
        CHECK(result.reason == "parse-failed");
    }
    CHECK_FALSE(bytecodeGraphVerificationAccepted(results, false));
}

TEST_CASE("fresh_copy_for_every_verifier")
{
    BytecodeBuilder builder;
    compileOrThrow(builder, "return 'string constant'");
    auto strings = builder.getStringTable();
    auto function = fromFunctionBytecode(builder.getFunctionData(0), strings);
    REQUIRE(function);
    const BytecodeGraphVerifier mutate{
        VerifierKind::Roundtrip,
        "mutate",
        [](CompTimeBcFunction& fn) -> BytecodeGraphVerificationResult
        {
            fn.blocks.clear();
            fn.constants.clear();
            return {Status::Decline, "unsupported", ""};
        }
    };
    const BytecodeGraphVerifier inspect{
        VerifierKind::UseConsistency,
        "inspect",
        [](CompTimeBcFunction& fn) -> BytecodeGraphVerificationResult
        {
            return {!fn.blocks.empty() && !fn.constants.empty() ? Status::Pass : Status::Fail, "", ""};
        }
    };
    auto result = verifyBytecodeGraph(*function, 0, {&mutate, &inspect});
    CHECK(result.results[0].second.status == Status::Decline);
    CHECK(result.results[1].second.status == Status::Pass);
    CHECK_FALSE(function->blocks.empty());
    CHECK_FALSE(function->constants.empty());
}

TEST_CASE("policies")
{
    std::vector<BytecodeGraphFunctionVerification> results{{0, {{VerifierKind::Summary, {Status::Pass, "", ""}}}}};
    CHECK(bytecodeGraphVerificationAccepted(results, false));
    CHECK(bytecodeGraphVerificationAccepted(results, true));
    results[0].results[0].second.status = Status::Decline;
    CHECK(bytecodeGraphVerificationAccepted(results, false));
    CHECK_FALSE(bytecodeGraphVerificationAccepted(results, true));
    results[0].results[0].second.status = Status::Fail;
    CHECK_FALSE(bytecodeGraphVerificationAccepted(results, false));
    CHECK_FALSE(bytecodeGraphVerificationAccepted(results, true));
}

TEST_CASE("stable_output_and_escaping")
{
    std::vector<BytecodeGraphFunctionVerification> results{
        {0,
         {{VerifierKind::UseConsistency, {Status::Pass, "", ""}}, {VerifierKind::Roundtrip, {Status::Fail, "serialize-failed", "bad\n\"bytecode\""}}}}
    };
    CHECK(
        formatBytecodeGraphVerification("example.luau", results) ==
        "file: example.luau\nfunction 0:\n  use-consistency: pass\n  roundtrip: fail [serialize-failed] bad\n\"bytecode\"\n"
        "summary:\n  functions: 1\n  pass: 1\n  decline: 0\n  fail: 1\n"
    );
    CHECK(
        formatBytecodeGraphVerification("a\\b\t.luau", results, true) ==
        "{\"file\":\"a\\\\b\\u0009.luau\",\"functions\":[{\"id\":0,\"verifiers\":{\"use-consistency\":{\"status\":\"pass\"},"
        "\"roundtrip\":{\"status\":\"fail\",\"reason\":\"serialize-failed\",\"detail\":\"bad\\u000a\\\"bytecode\\\"\"}}}],"
        "\"summary\":{\"functions\":1,\"pass\":1,\"decline\":0,\"fail\":1}}\n"
    );
}

TEST_CASE("invalid_reverse_use_data")
{
    CompTimeBcFunction function;
    BcOp block = function.entryBlock = function.addBlock();
    BcOp value = function.addInst(LOP_LOADN, block, {function.addImmInt(1)});
    function.addInst(LOP_RETURN, block, {function.addImmInt(1), value});
    REQUIRE(getBytecodeGraphVerifier(VerifierKind::UseConsistency)->verify(function).status == Status::Pass);
    function.instOp(value).uses.clear();
    auto result = getBytecodeGraphVerifier(VerifierKind::UseConsistency)->verify(function);
    CHECK(result.status == Status::Fail);
    CHECK(result.reason == "use-inconsistency");
}

TEST_SUITE_END();
