#!/usr/bin/env python3
"""Framework integration and corpus tests; pass --luau-compile to test the CLI."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.bytecode_graph_corpus import discover_sources, failure_signatures, main, sweep

COMPILER: Path | None = None


class CorpusTests(unittest.TestCase):
    def test_discovery_exclusions_and_overlapping_roots(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "a.luau").touch()
            (root / "b.lua").touch()
            (root / "ignored.txt").touch()
            resource = root / "resource"
            (resource / "child").mkdir(parents=True)
            (resource / "bench_resource_directory").touch()
            (resource / "data.lua").touch()
            (resource / "child" / "nested.luau").touch()
            self.assertEqual([root / "a.luau", root / "b.lua"], discover_sources([root, root]))
            with self.assertRaises(ValueError):
                discover_sources([root / "missing"])

    def test_aggregation_and_policy(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "case.luau").touch()
            data = {"functions": [{"id": 0, "verifiers": {
                "first": {"status": "pass"},
                "second": {"status": "decline", "reason": "unsupported"},
                "third": {"status": "fail", "reason": "broken", "detail": "diagnostic"}}}]}
            with mock.patch("subprocess.run", return_value=subprocess.CompletedProcess([], 1, json.dumps(data), "")) as run:
                report = sweep(Path("compiler"), [root], ["all"], "allow-decline", 3)
            self.assertEqual(root, run.call_args.kwargs["cwd"])
            self.assertEqual(1, report["files_compiled"])
            self.assertEqual(1, report["functions_parsed"])
            self.assertEqual({"broken": 1}, report["failure_reasons"])
            self.assertEqual(1, report["verifiers"]["first"]["pass"])
            self.assertEqual("unsupported", report["declines"][0]["reason"])
            self.assertEqual("diagnostic", report["failures"][0]["detail"])
            self.assertEqual({((root / "case.luau").as_posix(), 0, "third", "broken")}, failure_signatures(report))

            data["functions"][0]["verifiers"] = {"second": {"status": "decline"}}
            for policy, expected in (("allow-decline", 0), ("require-pass", 1)):
                with mock.patch("subprocess.run", return_value=subprocess.CompletedProcess([], expected, json.dumps(data), "")), \
                     mock.patch("builtins.print"):
                    self.assertEqual(expected, main(["--luau-compile=compiler", "--verify-bytecode-graph=all",
                                                     f"--verify-bytecode-graph-policy={policy}", "--roots", str(root)]))

    def test_compile_errors_malformed_json_and_timeout(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "a.lua").touch()
            outcomes = [(subprocess.CompletedProcess([], 1, "", "SyntaxError"), "compile-failed"),
                        (subprocess.CompletedProcess([], 0, '{}', ""), "compile-failed"),
                        (subprocess.CompletedProcess([], 0, '{"functions": null}', ""), "compile-failed"),
                        (subprocess.TimeoutExpired([], 1), "process-failed")]
            for outcome, reason in outcomes:
                patch = mock.patch("subprocess.run", side_effect=outcome) if isinstance(outcome, Exception) else \
                    mock.patch("subprocess.run", return_value=outcome)
                with self.subTest(reason=reason), patch:
                    report = sweep(Path("compiler"), [root], ["all"], "allow-decline", 1)
                    self.assertEqual(0, report["files_compiled"])
                    self.assertEqual({reason: 1}, report["failure_reasons"])

    def test_parse_failure_counts_and_baseline_report(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "case.lua").touch()
            data = {"functions": [{"id": 0, "verifiers": {
                "roundtrip": {"status": "fail", "reason": "parse-failed"}}}]}
            with mock.patch("subprocess.run", return_value=subprocess.CompletedProcess([], 1, json.dumps(data), "")) as run:
                report = sweep(Path("compiler"), [root], ["roundtrip"], "allow-decline", 1, root)
            self.assertEqual(root, run.call_args.kwargs["cwd"])
            self.assertEqual(1, report["files_compiled"])
            self.assertEqual(0, report["functions_parsed"])
            baseline = root / "baseline.json"
            baseline.write_text(json.dumps(report), encoding="utf-8")
            target = root / "reports" / "report.json"
            with mock.patch("subprocess.run", return_value=subprocess.CompletedProcess([], 1, json.dumps(data), "")), \
                 mock.patch("builtins.print"):
                self.assertEqual(1, main(["--luau-compile=compiler", "--verify-bytecode-graph=roundtrip",
                                          "--roots", str(root), "--baseline", str(baseline), "--report", str(target)]))
            self.assertEqual({"added": 0, "resolved": 0}, json.loads(target.read_text(encoding="utf-8"))["baseline"])


class CliTests(unittest.TestCase):
    def setUp(self) -> None:
        if COMPILER is None:
            self.skipTest("--luau-compile was not supplied")
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.source = Path(self.temporary.name) / "case.luau"
        self.source.write_text("local function choose(flag, a, b) if flag then return a else return b end end return choose(...)", encoding="utf-8")

    def run_cli(self, *args: str) -> subprocess.CompletedProcess[str]:
        arguments = [self.source.name if value == str(self.source) else value for value in args]
        return subprocess.run([str(COMPILER), *arguments, self.source.name], cwd=self.source.parent,
                              capture_output=True, text=True, check=False, timeout=30)

    def test_json_order_deduplication_and_all(self) -> None:
        result = self.run_cli("--verify-bytecode-graph=summary", "--verify-bytecode-graph=use-consistency",
                              "--verify-bytecode-graph=summary", "--verify-bytecode-graph=all",
                              "--verify-bytecode-graph-output=json", "--verify-bytecode-graph-policy=require-pass")
        self.assertEqual(0, result.returncode, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual({"functions": 2, "pass": 6, "decline": 0, "fail": 0}, data["summary"])
        self.assertEqual([0, 1], [fn["id"] for fn in data["functions"]])
        for function in data["functions"]:
            self.assertEqual(["summary", "use-consistency", "roundtrip"], list(function["verifiers"]))

    def test_text_and_multiple_json_files(self) -> None:
        args = ("--verify-bytecode-graph=roundtrip", "--verify-bytecode-graph=use-consistency")
        result = self.run_cli(*args)
        self.assertEqual(0, result.returncode, result.stderr)
        expected = "file: ./case.luau\nfunction 0:\n  roundtrip: pass\n  use-consistency: pass\n" \
                   "function 1:\n  roundtrip: pass\n  use-consistency: pass\n" \
                   "summary:\n  functions: 2\n  pass: 4\n  decline: 0\n  fail: 0\n"
        self.assertEqual(expected, result.stdout)
        result = self.run_cli(*args, "--verify-bytecode-graph-output=json", str(self.source))
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(2, len([json.loads(line) for line in result.stdout.splitlines()]))

    def test_unknown_options_and_compile_failure(self) -> None:
        for option in ("--verify-bytecode-graph=unknown", "--verify-bytecode-graph=",
                       "--verify-bytecode-graph-output=yaml", "--verify-bytecode-graph-policy=unknown"):
            result = self.run_cli(option)
            self.assertEqual(1, result.returncode)
            self.assertEqual("", result.stdout)
        result = self.run_cli("--verify-bytecode-graph=all", "--only-parse")
        self.assertEqual(1, result.returncode)
        self.source.write_text("local =", encoding="utf-8")
        result = self.run_cli("--verify-bytecode-graph=roundtrip")
        self.assertEqual(1, result.returncode)
        self.assertIn("SyntaxError", result.stderr)

    def test_graph_parse_failure_is_nonzero_and_machine_readable(self) -> None:
        self.source.write_text("local value = ...\n" + "if value then print(value) end\n" * 1100, encoding="utf-8")
        result = self.run_cli("--verify-bytecode-graph=roundtrip", "--verify-bytecode-graph-output=json")
        self.assertEqual(1, result.returncode)
        data = json.loads(result.stdout)
        self.assertEqual("parse-failed", data["functions"][0]["verifiers"]["roundtrip"]["reason"])
        self.assertEqual(1, data["summary"]["fail"])

    def test_normal_mode_unchanged_without_verifiers(self) -> None:
        plain = self.run_cli()
        with_policy = self.run_cli("--verify-bytecode-graph-policy=require-pass", "--verify-bytecode-graph-output=json")
        self.assertEqual(0, plain.returncode)
        self.assertTrue(plain.stdout)
        self.assertNotIn("summary:", plain.stdout)
        self.assertEqual(plain.stdout, with_policy.stdout)
        self.assertEqual(plain.returncode, with_policy.returncode)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--luau-compile", type=Path)
    args, remaining = parser.parse_known_args()
    COMPILER = args.luau_compile.resolve() if args.luau_compile else None
    unittest.main(argv=[sys.argv[0], *remaining])
