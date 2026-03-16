#!/usr/bin/env python3
# This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
"""
Run the Luau Google Benchmark C++ harness from a TOML config.
Compares baseline vs comparison and reports speedup percentages.
See bench/google_benchmark_example.toml for config format.
"""
import argparse
import json
import math
import os
import platform
import re
import statistics
import subprocess
import sys

try:
    import tomllib
except ModuleNotFoundError:
    tomllib = None


def run_command(command, cwd):
    r = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
    return {"command": command, "exit_code": r.returncode, "stdout": r.stdout, "stderr": r.stderr}


def parse_benchmark_name(name):
    parts = name.split("/", 3)
    if len(parts) != 4 or parts[0] != "ScriptPipeline":
        return None
    _, phase, config, script = parts
    return {"phase": phase, "config": config, "script": script}


def load_spec(path):
    if tomllib is None:
        raise RuntimeError("This script requires Python 3.11+ (tomllib). Upgrade Python.")
    if not path.lower().endswith(".toml"):
        raise ValueError("Config file must be .toml")
    with open(path, "rb") as f:
        return tomllib.loads(f.read())


def fflags_to_string(fflags):
    if not fflags:
        return ""
    if isinstance(fflags, str):
        return fflags
    if isinstance(fflags, dict):
        parts = []
        for k in sorted(fflags.keys()):
            v = fflags[k]
            s = str(v).lower() if isinstance(v, bool) else str(v)
            parts.append(f"{k}={s}")
        return ",".join(parts)
    raise ValueError("fflags must be a dict or string")


def tests_to_filter(tests):
    if tests in (None, "all"):
        return None
    if isinstance(tests, str):
        return tests
    if isinstance(tests, list):
        escaped = [re.escape(str(t).replace("\\", "/")) for t in tests]
        return "(" + "|".join(escaped) + ")$"
    raise ValueError("tests must be 'all', a regex string, or a list of paths")


def parse_config_label(config_label, flag_sets):
    m = re.fullmatch(r"o(\d+)_d(\d+)_cg(on|off)_f(\d+)", config_label)
    if not m:
        return {"pretty": config_label, "flags_value": config_label}
    opt, debug, codegen, idx = m.group(1), m.group(2), m.group(3), int(m.group(4))
    flag_val = (flag_sets[idx] or "default") if 0 <= idx < len(flag_sets) else "default"
    return {"pretty": f"O{opt} D{debug} CG={codegen}", "flags_value": flag_val}


def to_microseconds(value, unit):
    return {"ns": value / 1000, "us": value, "ms": value * 1000, "s": value * 1e6}.get(unit, value)


def parse_benchmark_json(path):
    with open(path, "r", encoding="utf-8") as f:
        payload = json.load(f)
    by_name = {}
    for row in payload.get("benchmarks", []):
        name = row.get("name")
        if name:
            by_name.setdefault(name, []).append(row)
    out = {}
    for name, rows in by_name.items():
        info = parse_benchmark_name(name)
        if not info:
            continue
        selected = next((r for r in rows if r.get("aggregate_name") == "mean"), rows[0])
        rt = selected.get("real_time")
        if rt is None:
            continue
        unit = selected.get("time_unit", "us")
        out[name] = {**info, "time_us": to_microseconds(float(rt), unit)}
    return out


def format_us(value_us):
    return f"{value_us / 1000:.2f}ms" if value_us >= 1000 else f"{value_us:.2f}us"


def truncate(s, width=36):
    return s if len(s) <= width else "..." + s[-(width - 3):]


def print_summary(runs, output_dir, flag_sets):
    all_values = {}
    for run in runs:
        path = run.get("json_output")
        if not path or not os.path.isfile(path):
            continue
        data = parse_benchmark_json(path)
        role = run.get("role")
        for _, row in data.items():
            key = (row["script"], row["phase"], row["config"], role)
            all_values.setdefault(key, []).append(row["time_us"])

    rows = []
    for (script, phase, config, role), values in sorted(all_values.items()):
        parsed = parse_config_label(config, flag_sets)
        rows.append({
            "script": script, "phase": phase, "config": config, "role": role,
            "config_pretty": parsed["pretty"], "flags_value": parsed["flags_value"],
            "runs": len(values), "avg_us": statistics.mean(values),
            "min_us": min(values), "max_us": max(values),
        })

    baseline = {(r["script"], r["phase"]): r for r in rows if r["role"] == "baseline"}
    comparison = {(r["script"], r["phase"]): r for r in rows if r["role"] == "comparison"}
    cmp_rows = []
    cmp_totals = []
    for key in sorted(set(baseline) & set(comparison)):
        b, c = baseline[key], comparison[key]
        if b["avg_us"] <= 0 or c["avg_us"] <= 0:
            continue
        ratio = b["avg_us"] / c["avg_us"]
        cmp_rows.append({
            "script": b["script"], "phase": b["phase"],
            "avg_us": b["avg_us"], "comparison_us": c["avg_us"],
            "speedup_pct": (ratio - 1) * 100, "ratio": ratio,
        })
        cmp_totals.append((b["phase"], ratio))

    compact = {
        "rows": rows,
        "comparison": {"rows": cmp_rows, "totals": []},
    }
    by_phase = {}
    for phase, r in cmp_totals:
        by_phase.setdefault(phase, []).append(r)
    for phase, ratios in sorted(by_phase.items()):
        gmean = math.exp(statistics.mean(math.log(r) for r in ratios))
        compact["comparison"]["totals"].append({"phase": phase, "pairs": len(ratios), "geomean_ratio": gmean, "geomean_speedup_pct": (gmean - 1) * 100})
    if cmp_totals:
        gmean = math.exp(statistics.mean(math.log(r) for _, r in cmp_totals))
        compact["comparison"]["totals"].append({"phase": "ALL", "pairs": len(cmp_totals), "geomean_ratio": gmean, "geomean_speedup_pct": (gmean - 1) * 100})

    compact_path = os.path.join(output_dir, "summary_compact.json")
    with open(compact_path, "w", encoding="utf-8") as f:
        json.dump(compact, f, indent=2)

    if not rows:
        print("\nNo benchmark rows found.")
        return compact_path

    print("\nCompact benchmark summary:")
    h = f"{'script':36} {'role':10} {'phase':8} {'config':24} {'runs':4} {'avg':10} {'min':10} {'max':10}"
    print(h)
    print("-" * len(h))
    for r in rows:
        print(f"{truncate(r['script']):36} {r['role']:10} {r['phase']:8} {str(r['flags_value'])[:24]:24} {r['runs']:4d} {format_us(r['avg_us']):10} {format_us(r['min_us']):10} {format_us(r['max_us']):10}")

    if cmp_rows:
        print("\nBaseline vs comparison:")
        h2 = f"{'script':36} {'phase':8} {'baseline':10} {'comparison':10} {'speedup %':10}"
        print(h2)
        print("-" * len(h2))
        for r in cmp_rows:
            print(f"{truncate(r['script']):36} {r['phase']:8} {format_us(r['avg_us']):10} {format_us(r['comparison_us']):10} {r['speedup_pct']:+9.2f}%")
        print("\nGeometric mean speedup:")
        for t in compact["comparison"]["totals"]:
            print(f"  {t['phase']:8} {t['pairs']:5d} pairs  {t['geomean_ratio']:.4f}x  ({t['geomean_speedup_pct']:+.2f}%)")

    return compact_path


def main():
    parser = argparse.ArgumentParser(
        description="Run Luau Google Benchmark harness from a TOML config. Compares baseline vs comparison.",
        epilog="TOML must define [baseline] and [comparison] with opt_level, debug_level, codegen, fflags.",
    )
    parser.add_argument("binary", help="Path to luau-benchmark executable")
    parser.add_argument("config", help="Path to .toml configuration file")
    parser.add_argument("--output-dir", default=None, help="Output directory for results (default: bench/local_results)")
    args = parser.parse_args()

    if not os.path.isfile(args.binary):
        raise FileNotFoundError(f"Benchmark binary not found: {args.binary}")

    spec = load_spec(args.config)
    baseline = spec.get("baseline")
    comparison = spec.get("comparison")
    if not baseline or not comparison:
        raise ValueError("TOML must define [baseline] and [comparison] sections")

    def norm(c):
        return {
            "opt": int(c["opt_level"]),
            "debug": int(c.get("debug_level", 1)),
            "codegen": str(c.get("codegen", "off")),
            "flags": fflags_to_string(c.get("fflags", {})),
        }

    baseline_case = norm(baseline)
    comparison_case = norm(comparison)
    flag_sets = [baseline_case["flags"], comparison_case["flags"]]

    repetitions = int(spec.get("repetitions", 3))
    min_time = float(spec.get("min_time", 0.2))
    scripts_root = spec.get("scripts_root", "bench/tests")
    phases = spec.get("phases", ["parse", "compile", "vm"])
    phases_str = ",".join(phases) if isinstance(phases, list) else phases
    script_filter = tests_to_filter(spec.get("tests", "all"))

    repo_root = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
    output_dir = args.output_dir or spec.get("output_dir", "bench/local_results")
    output_dir = os.path.join(repo_root, output_dir) if not os.path.isabs(output_dir) else output_dir
    os.makedirs(output_dir, exist_ok=True)

    executable = os.path.abspath(args.binary)

    print("Run recommendations: Release build, close background apps, stable thermal state.\n")

    summary = {"metadata": {"platform": platform.platform(), "config_file": args.config, "repetitions": repetitions}, "runs": []}
    run_cases = [("baseline", baseline_case), ("comparison", comparison_case)]

    for i in range(repetitions):
        for role, case in run_cases:
            json_out = os.path.join(output_dir, f"google-benchmark-{role}-run-{i + 1}.json")
            cmd = [
                executable,
                f"--benchmark_out={json_out}",
                "--benchmark_out_format=json",
                "--benchmark_report_aggregates_only=true",
                f"--benchmark_min_time={min_time}s",
                f"--luau_scripts_root={scripts_root}",
                f"--luau_phases={phases_str}",
                f"--luau_opt_levels={case['opt']}",
                f"--luau_debug_levels={case['debug']}",
                f"--luau_codegen={case['codegen']}",
            ]
            if case["flags"]:
                cmd.append(f"--luau_flag_sets={case['flags']}")
            if script_filter:
                cmd.append(f"--luau_script_filter={script_filter}")

            print(f"[{role}] run {i + 1}/{repetitions}")
            result = run_command(cmd, cwd=repo_root)
            result["json_output"] = json_out
            result["role"] = role
            summary["runs"].append(result)

            log_path = os.path.join(output_dir, f"{role}-run-{i + 1}.log")
            with open(log_path, "w", encoding="utf-8") as f:
                f.write(result["stdout"] + ("\n--- stderr ---\n" + result["stderr"] if result["stderr"] else ""))

    with open(os.path.join(output_dir, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)

    compact_path = print_summary(summary["runs"], output_dir, flag_sets)
    print(f"\nArtifacts: {output_dir}")
    print(f"Compact: {compact_path}")

    if any(r["exit_code"] != 0 for r in summary["runs"]):
        sys.exit(1)


if __name__ == "__main__":
    main()
