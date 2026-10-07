#!/usr/bin/env python3
"""Sweep source corpora through luau-compile's generic graph verifiers."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any


def discover_sources(roots: list[Path]) -> list[Path]:
    sources: set[Path] = set()
    for root in roots:
        if not root.is_dir():
            raise ValueError(f"source root is not a directory: {root}")
        for directory, children, files in os.walk(root):
            if "bench_resource_directory" in files:
                children[:] = []
                continue
            sources.update((Path(directory) / name).resolve() for name in files
                           if Path(name).suffix in (".lua", ".luau"))
    return sorted(sources, key=lambda path: path.as_posix())


def sweep(compiler: Path, roots: list[Path], verifiers: list[str], policy: str,
          timeout: float, source_root: Path | None = None,
          compiler_args: list[str] | None = None) -> dict[str, Any]:
    sources = discover_sources(roots)
    report: dict[str, Any] = {
        "schema_version": 1,
        "roots": [root.as_posix() for root in roots],
        "files_discovered": len(sources),
        "files_compiled": 0,
        "functions_parsed": 0,
        "verifiers": {},
        "failure_reasons": {},
        "failures": [],
        "declines": [],
    }

    def failure(file: str, reason: str, detail: str, function: int | None = None,
                verifier: str | None = None) -> None:
        reasons = report["failure_reasons"]
        reasons[reason] = reasons.get(reason, 0) + 1
        report["failures"].append({"file": file, "function": function,
                                   "verifier": verifier, "reason": reason, "detail": detail})

    for source in sources:
        file = source.as_posix()
        working_directory = source_root or source.parent
        try:
            source_argument = Path(os.path.relpath(source, working_directory)).as_posix()
        except ValueError:
            # Windows paths on different drives cannot be made relative.
            source_argument = source.as_posix()
        argv = [str(compiler), *(compiler_args or []), *(f"--verify-bytecode-graph={name}" for name in verifiers),
                f"--verify-bytecode-graph-policy={policy}", "--verify-bytecode-graph-output=json", source_argument]
        # Benchmark harnesses run from the source directory. An explicit root
        # supports corpora that resolve imports or resources from a common root.
        try:
            process = subprocess.run(argv, cwd=working_directory, capture_output=True,
                                     text=True, encoding="utf-8", errors="replace", timeout=timeout, check=False)
        except (OSError, subprocess.TimeoutExpired) as exc:
            failure(file, "process-failed", str(exc))
            continue
        try:
            data = json.loads(process.stdout)
            functions = data["functions"]
            if not isinstance(functions, list):
                raise ValueError("functions must be an array")
            records = [(fn["id"], fn["verifiers"]) for fn in functions]
            for _, results in records:
                for result in results.values():
                    if result["status"] not in ("pass", "decline", "fail"):
                        raise ValueError("unknown verifier status")
        except (ValueError, KeyError, TypeError, AttributeError) as exc:
            failure(file, "compile-failed", process.stderr.strip() or process.stdout.strip() or str(exc))
            continue

        report["files_compiled"] += 1
        had_rejection = False
        for function_id, results in records:
            if not any(result.get("reason") == "parse-failed" for result in results.values()):
                report["functions_parsed"] += 1
            for name, result in results.items():
                counts = report["verifiers"].setdefault(name, {"pass": 0, "decline": 0, "fail": 0})
                status = result["status"]
                counts[status] += 1
                if status == "fail":
                    had_rejection = True
                    failure(file, result.get("reason") or "verifier-failed", result.get("detail", ""), function_id, name)
                elif status == "decline":
                    report["declines"].append({"file": file, "function": function_id, "verifier": name,
                                               "reason": result.get("reason", ""), "detail": result.get("detail", "")})
                    had_rejection |= policy == "require-pass"
        if process.returncode != 0 and not had_rejection:
            failure(file, "process-failed", process.stderr.strip() or f"compiler exit code {process.returncode}")
    return report


def failure_signatures(report: dict[str, Any]) -> set[tuple[str, int | None, str | None, str]]:
    return {(entry["file"], entry["function"], entry["verifier"], entry["reason"])
            for entry in report["failures"]}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--luau-compile", required=True, type=Path)
    parser.add_argument("--verify-bytecode-graph", action="append", dest="verifiers", required=True)
    parser.add_argument("--verify-bytecode-graph-policy", choices=("allow-decline", "require-pass"), default="allow-decline")
    parser.add_argument("--roots", nargs="+", required=True, type=Path)
    parser.add_argument("--source-root", type=Path, help="working directory override; defaults to each source's directory")
    parser.add_argument("--compiler-arg", action="append", default=[], help="additional compiler option (repeatable; use --compiler-arg=--option)")
    parser.add_argument("--output", choices=("text", "json"), default="text")
    parser.add_argument("--report", type=Path, help="write the full JSON report")
    parser.add_argument("--baseline", type=Path, help="report added and resolved failure signatures; failures still reject the sweep")
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        report = sweep(args.luau_compile.resolve(), args.roots, args.verifiers,
                       args.verify_bytecode_graph_policy, args.timeout,
                       args.source_root.resolve() if args.source_root else None, args.compiler_arg)
        if args.baseline:
            baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
            current, previous = failure_signatures(report), failure_signatures(baseline)
            report["baseline"] = {"added": len(current - previous), "resolved": len(previous - current)}
        rendered = json.dumps(report, indent=2, ensure_ascii=True) + "\n"
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(rendered, encoding="utf-8")
        if args.output == "json":
            print(rendered, end="")
        else:
            print(f"corpus: {', '.join(report['roots'])}")
            print(f"files: {report['files_discovered']}\ncompiled: {report['files_compiled']}\nfunctions: {report['functions_parsed']}")
            for name, counts in report["verifiers"].items():
                print(f"{name}:")
                for status, count in counts.items():
                    print(f"  {status}: {count}")
            for reason, count in report["failure_reasons"].items():
                print(f"{reason}: {count}")
        return int(bool(report["failures"]) or
                   (args.verify_bytecode_graph_policy == "require-pass" and bool(report["declines"])))
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"bytecode-graph-corpus: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
