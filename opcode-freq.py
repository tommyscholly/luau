#!/usr/bin/env python3
"""
Usage: ./opcode-freq.py [--show-asm] <luau-file>
Dumps bytecode opcodes and shows frequency table for a single file.

Options:
  --show-asm    Show the full disassembly before the frequency table
"""

import subprocess
import sys
import re
from collections import Counter
from typing import Tuple, Dict, List

def compile_and_extract(filepath: str) -> Tuple[str, Counter]:
    """Run luau-compile --text and extract opcodes with counts."""
    result = subprocess.run(
        ["./luau-compile", "--text", filepath],
        capture_output=True
    )

    if result.returncode != 0:
        print(f"Error compiling {filepath}: {result.stderr.decode('utf-8', errors='replace')}", file=sys.stderr)
        return "", Counter()

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

    return output, opcode_counts

def print_frequency_table(counter: Counter, total: int, title: str = "OPCODE FREQUENCY TABLE"):
    """Print a sorted frequency table."""
    print(title)
    print("=" * 40)
    print()

    sorted_items = sorted(counter.items(), key=lambda x: (-x[1], x[0]))

    for opcode, count in sorted_items:
        pct = (count / total * 100) if total > 0 else 0.0
        print(f"{opcode:<20} {count:>5}  {pct:>6.1f}%")

    print()
    print(f"{'TOTAL':<20} {total:>5}")

def main():
    show_asm = False
    if len(sys.argv) > 1 and sys.argv[1] == "--show-asm":
        show_asm = True
        sys.argv.pop(1)

    if len(sys.argv) != 2:
        print("Usage: ./opcode-freq.py [--show-asm] <luau-file>")
        sys.exit(1)

    filepath = sys.argv[1]

    if not filepath.endswith(('.luau', '.lua')):
        print(f"Warning: File may not be a luau script: {filepath}")

    assembly, opcode_counts = compile_and_extract(filepath)

    if not opcode_counts:
        print("No opcodes found (compilation may have failed)")
        sys.exit(1)

    if show_asm:
        print("=== FULL BYTECODE DISASSEMBLY ===")
        print(assembly)
        print()
        print_frequency_table(opcode_counts, sum(opcode_counts.values()), "OPCODE FREQUENCY TABLE")
    else:
        print_frequency_table(opcode_counts, sum(opcode_counts.values()))

if __name__ == "__main__":
    main()
