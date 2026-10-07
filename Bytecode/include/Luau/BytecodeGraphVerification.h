// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/BytecodeGraph.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Luau::Bytecode
{

enum class BytecodeGraphVerifierKind
{
    Roundtrip,
    UseConsistency,
    Summary,
    All,
};

std::optional<BytecodeGraphVerifierKind> parseBytecodeGraphVerifier(std::string_view name);
const char* bytecodeGraphVerifierName(BytecodeGraphVerifierKind kind);

enum class BytecodeGraphVerificationStatus
{
    Pass,
    Decline,
    Fail,
};

struct BytecodeGraphVerificationResult
{
    BytecodeGraphVerificationStatus status;
    std::string reason;
    std::string detail;
};

struct BytecodeGraphVerifier
{
    BytecodeGraphVerifierKind kind;
    const char* description;
    BytecodeGraphVerificationResult (*verify)(CompTimeBcFunction& function);
};

const BytecodeGraphVerifier* getBytecodeGraphVerifier(BytecodeGraphVerifierKind kind);
std::vector<const BytecodeGraphVerifier*> getBytecodeGraphVerifiers();

struct BytecodeGraphVerificationOptions
{
    std::vector<BytecodeGraphVerifierKind> verifiers;
    bool requirePass = false;
};

struct BytecodeGraphFunctionVerification
{
    uint32_t functionId;
    std::vector<std::pair<BytecodeGraphVerifierKind, BytecodeGraphVerificationResult>> results;
};

// Expands All in registry order, deduplicating at first occurrence; throws std::invalid_argument for invalid kinds.
std::vector<const BytecodeGraphVerifier*> selectBytecodeGraphVerifiers(const BytecodeGraphVerificationOptions& options);

// Each callback receives its own graph copy. Also supports caller-owned verifier plugins.
BytecodeGraphFunctionVerification verifyBytecodeGraph(
    const CompTimeBcFunction& function,
    uint32_t functionId,
    const std::vector<const BytecodeGraphVerifier*>& verifiers
);

// The builder and its string storage must remain alive for the duration of this call.
std::vector<BytecodeGraphFunctionVerification> verifyBytecodeGraphs(BytecodeBuilder& builder, const BytecodeGraphVerificationOptions& options);

bool bytecodeGraphVerificationAccepted(const std::vector<BytecodeGraphFunctionVerification>& functions, bool requirePass);
const char* bytecodeGraphVerificationStatusName(BytecodeGraphVerificationStatus status);
std::string formatBytecodeGraphVerification(
    std::string_view file,
    const std::vector<BytecodeGraphFunctionVerification>& functions,
    bool json = false
);

} // namespace Luau::Bytecode
