// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/BytecodeGraphVerification.h"
#include "Luau/BytecodeValidation.h"

#include <iterator>
#include <stdexcept>

namespace Luau::Bytecode
{

using Status = BytecodeGraphVerificationStatus;
using VerifierKind = BytecodeGraphVerifierKind;

static BytecodeGraphVerificationResult roundtrip(CompTimeBcFunction& function)
{
    BytecodeBuilder builder;

    // The serializer preserves global child-function IDs. Builder validation
    // needs those IDs and their capture counts, even when serializing only one
    // function. Seed metadata-only dependencies using existing builder APIs;
    // their bodies are not part of the returned function bytecode.
    size_t dependencyCount = 0;
    for (uint32_t id : function.protos)
        dependencyCount = std::max(dependencyCount, size_t(id) + 1);
    for (const BcVmConst& constant : function.constants)
        if (constant.kind == BcVmConstKind::Closure)
            dependencyCount = std::max(dependencyCount, size_t(constant.valueClosure) + 1);

    std::vector<uint8_t> captureCounts(dependencyCount);
    std::vector<bool> known(dependencyCount);
    for (const BcBlock& block : function.blocks)
    {
        if (block.flags & BcBlockFlag::Dead)
            continue;
        for (auto it = block.ops.begin(); it != block.ops.end(); ++it)
        {
            const BcInst& inst = function.instOp(*it);
            if (inst.op != LOP_NEWCLOSURE && inst.op != LOP_DUPCLOSURE)
                continue;
            uint32_t id = inst.op == LOP_NEWCLOSURE ? function.protos[inst.ops[0].index] : function.constants[inst.ops[0].index].valueClosure;
            size_t captures = 0;
            for (auto next = std::next(it); next != block.ops.end() && function.instOp(*next).op == LOP_CAPTURE; ++next)
                ++captures;
            if (captures > 255 || (known[id] && captureCounts[id] != captures))
                return {Status::Fail, "serialize-failed", "inconsistent closure capture counts"};
            known[id] = true;
            captureCounts[id] = uint8_t(captures);
        }
    }
    for (uint8_t captures : captureCounts)
    {
        builder.beginFunction(0);
        builder.emitABC(LOP_RETURN, 0, 1, 0);
        builder.endFunction(1, captures);
    }

    if (toFunctionBytecode(builder, function).empty())
        return {Status::Fail, "serialize-failed", ""};
    return {Status::Pass, "", ""};
}

static BytecodeGraphVerificationResult useConsistency(CompTimeBcFunction& function)
{
    if (!verifyUseConsistency(function))
        return {Status::Fail, "use-inconsistency", ""};
    return {Status::Pass, "", ""};
}

static BytecodeGraphVerificationResult summary(CompTimeBcFunction& function)
{
    std::vector<bool> seen(function.blocks.size());
    std::vector<BcOp> pending{function.entryBlock};
    size_t reachable = 0;
    while (!pending.empty())
    {
        BcOp block = pending.back();
        pending.pop_back();
        if (seen[block.index])
            continue;
        seen[block.index] = true;
        ++reachable;
        for (const BcBlockEdge& edge : function.blocks[block.index].successors)
            pending.push_back(edge.target);
    }
    std::string detail = "blocks=" + std::to_string(function.blocks.size()) + " reachable-blocks=" + std::to_string(reachable) +
                         " instructions=" + std::to_string(function.instructions.size()) + " phis=" + std::to_string(function.phis.size()) +
                         " projections=" + std::to_string(function.projections.size()) + " parameters=" + std::to_string(function.numparams) +
                         " max-stack-size=" + std::to_string(function.maxstacksize);
    return {Status::Pass, "", std::move(detail)};
}

static const BytecodeGraphVerifier registry[] = {
    {VerifierKind::Roundtrip, "Serialize the graph and require non-empty bytecode", roundtrip},
    {VerifierKind::UseConsistency, "Check reverse uses against graph operands", useConsistency},
    {VerifierKind::Summary, "Report stable graph statistics", summary},
};

const char* bytecodeGraphVerifierName(VerifierKind kind)
{
    switch (kind)
    {
    case VerifierKind::Roundtrip:
        return "roundtrip";
    case VerifierKind::UseConsistency:
        return "use-consistency";
    case VerifierKind::Summary:
        return "summary";
    case VerifierKind::All:
        return "all";
    }
    throw std::invalid_argument("Invalid bytecode graph verifier kind");
}

std::optional<VerifierKind> parseBytecodeGraphVerifier(std::string_view name)
{
    if (name == "all")
        return VerifierKind::All;
    for (const BytecodeGraphVerifier& verifier : registry)
        if (name == bytecodeGraphVerifierName(verifier.kind))
            return verifier.kind;
    return std::nullopt;
}

const BytecodeGraphVerifier* getBytecodeGraphVerifier(VerifierKind kind)
{
    for (const BytecodeGraphVerifier& verifier : registry)
        if (kind == verifier.kind)
            return &verifier;
    return nullptr;
}

std::vector<const BytecodeGraphVerifier*> getBytecodeGraphVerifiers()
{
    std::vector<const BytecodeGraphVerifier*> result;
    for (const BytecodeGraphVerifier& verifier : registry)
        result.push_back(&verifier);
    return result;
}

std::vector<const BytecodeGraphVerifier*> selectBytecodeGraphVerifiers(const BytecodeGraphVerificationOptions& options)
{
    std::vector<const BytecodeGraphVerifier*> result;
    auto append = [&](const BytecodeGraphVerifier* verifier)
    {
        if (std::find(result.begin(), result.end(), verifier) == result.end())
            result.push_back(verifier);
    };
    for (VerifierKind kind : options.verifiers)
    {
        if (kind == VerifierKind::All)
            for (const BytecodeGraphVerifier* verifier : getBytecodeGraphVerifiers())
                append(verifier);
        else if (const BytecodeGraphVerifier* verifier = getBytecodeGraphVerifier(kind))
            append(verifier);
        else
            throw std::invalid_argument("Invalid bytecode graph verifier kind");
    }
    return result;
}

BytecodeGraphFunctionVerification verifyBytecodeGraph(
    const CompTimeBcFunction& function,
    uint32_t functionId,
    const std::vector<const BytecodeGraphVerifier*>& verifiers
)
{
    BytecodeGraphFunctionVerification result{functionId, {}};
    for (const BytecodeGraphVerifier* verifier : verifiers)
    {
        CompTimeBcFunction copy = function;
        result.results.emplace_back(verifier->kind, verifier->verify(copy));
    }
    return result;
}

std::vector<BytecodeGraphFunctionVerification> verifyBytecodeGraphs(BytecodeBuilder& builder, const BytecodeGraphVerificationOptions& options)
{
    auto verifiers = selectBytecodeGraphVerifiers(options);
    std::vector<std::string_view> strings = builder.getStringTable();
    std::vector<BytecodeGraphFunctionVerification> results;
    for (uint32_t id = 0; id < builder.getFunctionCount(); ++id)
    {
        if (auto function = fromFunctionBytecode(builder.getFunctionData(id), strings))
            results.push_back(verifyBytecodeGraph(*function, id, verifiers));
        else
        {
            BytecodeGraphFunctionVerification failure{id, {}};
            for (const BytecodeGraphVerifier* verifier : verifiers)
                failure.results.emplace_back(verifier->kind, BytecodeGraphVerificationResult{Status::Fail, "parse-failed", ""});
            results.push_back(std::move(failure));
        }
    }
    return results;
}

bool bytecodeGraphVerificationAccepted(const std::vector<BytecodeGraphFunctionVerification>& functions, bool requirePass)
{
    for (const auto& function : functions)
        for (const auto& result : function.results)
            if (result.second.status == Status::Fail || (requirePass && result.second.status != Status::Pass))
                return false;
    return true;
}

const char* bytecodeGraphVerificationStatusName(Status status)
{
    switch (status)
    {
    case Status::Pass:
        return "pass";
    case Status::Decline:
        return "decline";
    case Status::Fail:
        return "fail";
    }
    LUAU_ASSERT(!"Invalid verification status");
    return "fail";
}

static std::string quoteJson(std::string_view value)
{
    std::string result = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char ch : value)
    {
        if (ch == '"' || ch == '\\')
        {
            result += '\\';
            result += char(ch);
        }
        else if (ch < 32)
        {
            result += "\\u00";
            result += hex[ch >> 4];
            result += hex[ch & 15];
        }
        else
            result += char(ch);
    }
    return result + '"';
}

std::string formatBytecodeGraphVerification(std::string_view file, const std::vector<BytecodeGraphFunctionVerification>& functions, bool json)
{
    size_t passes = 0, declines = 0, failures = 0;
    std::string output = json ? "{\"file\":" + quoteJson(file) + ",\"functions\":[" : "file: " + std::string(file) + "\n";
    bool firstFunction = true;
    for (const auto& function : functions)
    {
        if (json)
        {
            output += firstFunction ? "" : ",";
            output += "{\"id\":" + std::to_string(function.functionId) + ",\"verifiers\":{";
        }
        else
            output += "function " + std::to_string(function.functionId) + ":\n";
        firstFunction = false;
        bool firstVerifier = true;
        for (const auto& [kind, result] : function.results)
        {
            const char* name = bytecodeGraphVerifierName(kind);
            passes += result.status == Status::Pass;
            declines += result.status == Status::Decline;
            failures += result.status == Status::Fail;
            if (json)
            {
                output += firstVerifier ? "" : ",";
                output += quoteJson(name) + ":{\"status\":" + quoteJson(bytecodeGraphVerificationStatusName(result.status));
                if (!result.reason.empty())
                    output += ",\"reason\":" + quoteJson(result.reason);
                if (!result.detail.empty())
                    output += ",\"detail\":" + quoteJson(result.detail);
                output += "}";
            }
            else
            {
                output += std::string("  ") + name + ": " + bytecodeGraphVerificationStatusName(result.status);
                if (!result.reason.empty())
                    output += " [" + result.reason + "]";
                if (!result.detail.empty())
                    output += " " + result.detail;
                output += "\n";
            }
            firstVerifier = false;
        }
        if (json)
            output += "}}";
    }
    if (json)
        output += "],\"summary\":{\"functions\":" + std::to_string(functions.size()) + ",\"pass\":" + std::to_string(passes) +
                  ",\"decline\":" + std::to_string(declines) + ",\"fail\":" + std::to_string(failures) + "}}\n";
    else
        output += "summary:\n  functions: " + std::to_string(functions.size()) + "\n  pass: " + std::to_string(passes) +
                  "\n  decline: " + std::to_string(declines) + "\n  fail: " + std::to_string(failures) + "\n";
    return output;
}

} // namespace Luau::Bytecode
