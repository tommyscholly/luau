# Bytecode graph verification

`luau-compile --verify-bytecode-graph=roundtrip source.luau` compiles source
normally, parses every generated function, and runs named verifier callbacks.
No verification runs unless at least one verifier name is supplied. Verification
output replaces the normal bytecode or assembly dump. `--only-parse` cannot be
combined with verification.

Repeat `--verify-bytecode-graph=<name>` to select multiple verifiers. Names are
case-sensitive. `all` expands to the registry order: `roundtrip`,
`use-consistency`, `summary`. Duplicates are removed at their first occurrence,
including overlaps with `all`. Function IDs are ascending and verifier results
retain the selected order.

- `roundtrip` serializes through a fresh builder and requires non-empty output.
  It seeds placeholder child functions with capture counts inferred from graph
  closure instructions, as required by the existing builder's validation. Only
  the supplied graph's function bytecode is checked; placeholder bodies are not
  returned. It does not require byte-for-byte equivalence.
- `use-consistency` uses the existing reverse-use consistency check.
- `summary` passes and emits block, reachable block, instruction, phi,
  projection, parameter, and maximum stack size counts in its diagnostic detail.
  Reachability starts at the entry and follows successors, including synthetic
  entry and exit blocks where present.

`--verify-bytecode-graph-policy=allow-decline` is the default. A failure or graph
parse failure rejects the command. `require-pass` also rejects declines. Unknown
names and invalid policies or output formats return exit code 1 before compiling.

`--verify-bytecode-graph-output=text` is the default. JSON output contains
`file`, `functions` (an array of `id` and `verifiers`), and `summary` (counts for
`functions`, `pass`, `decline`, `fail`). Each named verifier result contains
`status` and optionally `reason` and `detail`. No plugin internals are included.
The compiler emits one complete JSON object per line per successfully compiled
file, including graph failures. Multiple input files therefore produce JSON
Lines. Source compilation and file-open errors retain the normal stderr
diagnostics and exit code 1; they do not produce a graph report.

The public API is `Luau/BytecodeGraphVerification.h` in `Luau.Bytecode`.
`verifyBytecodeGraphs` keeps the builder string table alive through graph use
and copies each graph for every callback. It returns results without printing or
exiting; callers apply `bytecodeGraphVerificationAccepted` with their policy.
Verifier identities in options, registry entries, and results use the
`BytecodeGraphVerifierKind` enum (`Roundtrip`, `UseConsistency`, `Summary`,
`All`). `parseBytecodeGraphVerifier` converts CLI strings to enum values and
returns `std::nullopt` for unknown names. `bytecodeGraphVerifierName` converts
an enum value to its stable output name. `selectBytecodeGraphVerifiers` expands
`All`, deduplicates selections, and rejects invalid enum values with
`std::invalid_argument`. Future built-in plugins add an enum value, name
mapping, and registry entry without changing the CLI or harness. Caller-owned plugins can use `verifyBytecodeGraph` directly. A plugin
returns `Pass`, intentional `Decline`, or contract `Fail`, with stable reasons
and optional diagnostic detail. The framework has no allocator dependency.

## Benchmark source sweeps

```sh
python3 tools/bytecode_graph_corpus.py \
  --luau-compile build/debug/luau-compile \
  --verify-bytecode-graph=roundtrip \
  --verify-bytecode-graph=use-consistency \
  --compiler-arg=--fflags=DebugLuauUserDefinedClasses=true \
  --roots bench/tests bench/micro_tests \
  --output json --report build/bytecode-graph-corpus.json
```

The wrapper recursively discovers `.lua` and `.luau`, deduplicates overlapping
roots, and excludes directories marked `bench_resource_directory` and their
children. It runs each compiler process from the source directory, matching the
benchmark harness; `--source-root` overrides the working directory when needed.
`--timeout` controls each process timeout. Sources are compiled, not executed.
Repeat `--compiler-arg=--option` to supply flags or optimization settings. The
class feature flag in the example is required by class benchmark sources; the
CMake corpus target also supplies it.

Reports have `schema_version: 1`, roots, discovered and compiled file counts,
parsed function counts, per-verifier pass/decline/fail counts, failure reason
counts, and full file/function/verifier diagnostics for failures and declines.
Compilation, process, timeout, and invalid-report failures also reject the sweep.
`--verify-bytecode-graph-policy=require-pass` rejects declines. A report file is
always JSON, independently of the selected console format.

`--baseline <report.json>` adds counts of new and resolved failure signatures
(file, function, verifier, reason). It does not waive existing failures. Compare
reports made with the same source-root paths. `Luau.BytecodeGraphCorpus` is a
separate CMake target; benchmark sources are not part of normal golden runs.

Run framework and corpus tests with:

```sh
python3 -m tools.bytecode_graph_selftest --luau-compile build/debug/luau-compile
python3 -m tools.golden.selftest
```

The conformance helper uses the shared roundtrip driver and preserves its prior
skip for functions exceeding the existing graph parser's CFG limit. The CLI
reports these as `parse-failed` failures for every selected verifier.

C++ tests are in the `BytecodeGraphVerification` test suite. Golden snapshots
and normal language behavior tests cover `tests/golden/bytecode-graph`.
