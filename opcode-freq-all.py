#!/usr/bin/env python3
"""
Usage: ./opcode-freq-all.py [--show-asm] <pattern> [<pattern>...]
Analyzes opcode frequencies across multiple luau/lua files.

Patterns can be globs (e.g., "tests/**/*.luau") or directories.
Options:
  --show-asm    Show disassembly for each file before the summary
  --json        Output results as JSON
"""

import subprocess
import sys
import glob as glob_module
import os
import re
from collections import Counter, defaultdict
from typing import Dict, List, Tuple, Optional

def find_files(patterns: List[str]) -> List[str]:
    """Find all .luau and .lua files matching the given patterns."""
    files = set()

    for pattern in patterns:
        if os.path.isdir(pattern):
            # Directory: recurse and find all lua files
            for root, _, filenames in os.walk(pattern):
                for f in filenames:
                    if f.endswith(('.luau', '.lua')):
                        files.add(os.path.join(root, f))
        elif '*' in pattern or '?' in pattern:
            # Glob pattern
            for f in glob_module.glob(pattern, recursive=True):
                if os.path.isfile(f) and f.endswith(('.luau', '.lua')):
                    files.add(f)
        elif os.path.isfile(pattern) and pattern.endswith(('.luau', '.lua')):
            # Direct file
            files.add(pattern)

    return sorted(files)

def compile_and_extract(filepath: str) -> Tuple[bool, Dict[str, int]]:
    """Run luau-compile --text and extract opcodes with counts."""
    result = subprocess.run(
        ["./luau-compile", "--text", filepath],
        capture_output=True
    )

    if result.returncode != 0:
        return False, {}

    output = result.stdout.decode('utf-8', errors='replace')
    label_pattern = re.compile(r'^L\d+:\s*')
    opcode_counts = Counter()

    for line in output.splitlines():
        line = label_pattern.sub('', line)
        if not line:
            continue
        match = re.match(r'^([A-Z][A-Z_0-9]*)\b', line)
        if match:
            opcode_counts[match.group(1)] += 1

    return True, opcode_counts

def print_frequency_table(counter: Dict[str, int], total: int, file_count: int,
                         title: str = "OPCODE FREQUENCY TABLE (AGGREGATE)"):
    """Print a sorted frequency table."""
    print(title)
    print("=" * 42)
    print(f"  Analyzed {file_count} files")
    print()

    sorted_items = sorted(counter.items(), key=lambda x: (-x[1], x[0]))

    for opcode, count in sorted_items:
        pct = (count / total * 100) if total > 0 else 0.0
        print(f"{opcode:<20} {count:>5}  {pct:>6.1f}%")

    print()
    print(f"{'TOTAL':<20} {total:>5}")

def print_json_output(aggregate: Dict[str, int], per_file: Dict[str, Dict[str, int]], file_count: int):
    """Output results as JSON."""
    import json

    output = {
        "file_count": file_count,
        "aggregate": aggregate,
        "total_opcodes": sum(aggregate.values()),
        "per_file": per_file
    }

    print(json.dumps(output, indent=2))

def main():
    show_asm = False
    output_json = False

    if len(sys.argv) > 1:
        if sys.argv[1] == "--show-asm":
            show_asm = True
            sys.argv.pop(1)
        elif sys.argv[1] == "--json":
            output_json = True
            sys.argv.pop(1)

    if len(sys.argv) < 2:
        print("Usage: ./opcode-freq-all.py [--show-asm] [--json] <pattern> [<pattern>...]")
        print("  Patterns are globs or directories, e.g.:")
        print("    ./opcode-freq-all.py tests/conformance/*.luau")
        print("    ./opcode-freq-all.py 'tests/**/*.luau'")
        print("    ./opcode-freq-all.py /path/to/project/")
        sys.exit(1)

    patterns = sys.argv[1:]
    files = find_files(patterns)

    if not files:
        print("No .luau/.lua files found matching the given patterns.")
        sys.exit(1)

    aggregate_counter: Counter = Counter()
    per_file: Dict[str, Dict[str, int]] = {}
    assembly_parts = []
    file_count = 0
    failed = 0

    for filepath in files:
        success, opcode_counts = compile_and_extract(filepath)
        if success:
            file_count += 1
            aggregate_counter.update(opcode_counts)
            per_file[filepath] = dict(opcode_counts)
            assembly_parts.append(f"=== {filepath} ===\n")
            asm_result = subprocess.run(
                ["./luau-compile", "--text", filepath],
                capture_output=True
            )
            assembly_parts.append(asm_result.stdout.decode('utf-8', errors='replace'))
        else:
            failed += 1

    if file_count == 0:
        print("No files compiled successfully.")
        sys.exit(1)

    if output_json:
        print_json_output(dict(aggregate_counter), per_file, file_count)
    elif show_asm:
        print("=== FULL BYTECODE DISASSEMBLY (all files) ===")
        print("".join(assembly_parts))
        print()
        print_frequency_table(dict(aggregate_counter), sum(aggregate_counter.values()), file_count,
                             "=== AGGREGATE OPCODE FREQUENCY TABLE ===")
    else:
        print_frequency_table(dict(aggregate_counter), sum(aggregate_counter.values()), file_count)

    if failed > 0:
        print(f"\nWarning: {failed} file(s) failed to compile", file=sys.stderr)

if __name__ == "__main__":
    main()
