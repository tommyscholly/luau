# Luau C++ Benchmark Harness

This directory contains a [Google Benchmark](https://github.com/google/benchmark)–based C++ harness that measures Luau **parse**, **compile**, and **VM** performance on a set of Lua scripts. All timing is in-process (no process spawn), with state setup/teardown excluded from VM timings where appropriate.

## Building

- Enable benchmarks when configuring CMake:

  ```bash
  cmake -B build -DLUAU_BUILD_BENCHMARKS=ON
  cmake --build build
  ```

- Requires **CMake 3.14+** (for `FetchContent`). Google Benchmark is fetched automatically; no separate install is needed.

## Benchmark Phases

Each discovered script can be run through one or more phases:

| Phase       | What is timed                                                                                                                                                                |
| ----------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **parse**   | `Parser::parse()` only (AST construction).                                                                                                                                   |
| **compile** | Full `Luau::compile()` (parse + compilation to bytecode).                                                                                                                    |
| **vm**      | Load bytecode into a new state, optional CodeGen compile, then `lua_pcall()` to completion. State creation/destruction and library setup are excluded from the timed region. |

## Configuration Matrix

The harness runs a **Cartesian product** of:

- **Optimization level** (e.g. 2)
- **Debug level** (e.g. 1)
- **Codegen** (interpreter only, or with native codegen when supported)
- **Flag sets** (Luau fast flags; commas separate flags within a set, semicolons separate multiple sets; empty means default flags)

So you can compare, for example, codegen off vs on by running with one codegen mode per run and comparing results.

## Command-Line Options

All Luau-specific options use the `--luau_*` prefix. Any remaining arguments are passed through to Google Benchmark (e.g. `--benchmark_filter`, `--benchmark_min_time`).

| Option                       | Default            | Description                                                                                                                                                                                                    |
| ---------------------------- | ------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `--luau_scripts_root=PATH`   | `bench/tests`      | Root directory for discovering `.lua` and `.luau` scripts (recursive).                                                                                                                                         |
| `--luau_script_filter=REGEX` | *(none)*           | Only scripts whose relative path matches this regex are included.                                                                                                                                              |
| `--luau_phases=LIST`         | `parse,compile,vm` | Comma-separated phases to run.                                                                                                                                                                                 |
| `--luau_opt_levels=LIST`     | `2`                | Comma-separated optimization levels (e.g. `1,2`).                                                                                                                                                              |
| `--luau_debug_levels=LIST`   | `1`                | Comma-separated debug levels.                                                                                                                                                                                  |
| `--luau_codegen=MODE`        | `both`             | `off`, `on`, or `both`.                                                                                                                                                                                        |
| `--luau_flag_sets=LIST`      | *(default flags)*  | Comma-separated flags within a set (e.g. `Flag1=true,Flag2=false`); use semicolons only to separate multiple sets (e.g. `Flag1=true;Flag1=false`). Each set is applied for one dimension of the config matrix. |

Examples:

```bash
# Run only VM phase, codegen on, on scripts under shootout/
./luau-benchmark --luau_phases=vm --luau_codegen=on --luau_script_filter="shootout/.*"

# Run parse and compile with custom script root, output JSON
./luau-benchmark --luau_scripts_root=../tests --luau_phases=parse,compile --benchmark_out=results.json --benchmark_out_format=json
```

## Script Requirements

- Scripts are read from the scripts root; only **`.lua`** and **`.luau`** files are considered.
- For the **VM** phase, scripts must run to completion (no yielding). Scripts that call `require("bench_support")` or `require("../bench_support")` are supported: the harness installs a shim that provides a `bench` table with `runCode`. Any other `require` will fail unless you extend the shim.
- Scripts that fail to parse or compile, or that error at load/run time, are reported via `SkipWithError` and do not crash the run.

## Output

- Timings are reported in **microseconds** (Google Benchmark `kMicrosecond`).
- Use standard Google Benchmark flags for output, e.g.:
  - `--benchmark_out=path.json --benchmark_out_format=json`
  - `--benchmark_report_aggregates_only=true`
  - `--benchmark_min_time=0.2` (minimum seconds per benchmark before reporting)

## TOML Runner (Baseline vs Comparison)

The **`run_google_benchmark.py`** script in the parent `bench/` directory runs this harness twice (baseline and comparison) from a **TOML config**, then compares results and prints geometric-mean speedups. Use it for “codegen off vs on” or similar A/B comparisons:

```bash
python bench/run_google_benchmark.py <path-to-luau-benchmark> bench/google_benchmark_example.toml
```

Pass the path to the built `luau-benchmark` executable, the path to a TOML config (required), and optionally `--output-dir` for results. See `bench/google_benchmark_example.toml` and the docstring in `bench/run_google_benchmark.py` for TOML options (repetitions, min_time, scripts_root, phases, baseline/comparison opt_level, debug_level, codegen, fflags). The Python script requires **Python 3.11+** (for `tomllib`).
