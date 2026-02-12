#!/bin/bash
# Script to analyze MOVE+CALL optimization opportunities across all Luau benchmarks

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LUAU_BIN="${SCRIPT_DIR}/build/luau"
BENCH_DIR="${SCRIPT_DIR}/bench/tests"

if [ ! -f "$LUAU_BIN" ]; then
    echo "Error: luau binary not found at $LUAU_BIN"
    echo "Please build first: cmake --build build --target Luau.Repl.CLI"
    exit 1
fi

if [ ! -d "$BENCH_DIR" ]; then
    echo "Error: benchmark directory not found at $BENCH_DIR"
    exit 1
fi

# Temporary file for raw results
TMP_FILE=$(mktemp)
trap "rm -f $TMP_FILE" EXIT

echo "Analyzing MOVE+CALL optimization opportunities..."
echo "=================================================="
echo ""

# Find all .lua files in bench/tests
find "$BENCH_DIR" -name "*.lua" -type f | sort | while read -r lua_file; do
    # Get relative path for cleaner output
    rel_path="${lua_file#$BENCH_DIR/}"
    
    # Run the benchmark and capture stderr (which has our compiler stats)
    # Timeout after 30 seconds in case of infinite loops
    output=$(timeout 30 "$LUAU_BIN" "$lua_file" 2>&1) || {
        echo "$rel_path|ERROR|0|0|0|0|0" >> "$TMP_FILE"
        continue
    }
    
    # Extract the compiler stats from stderr
    # Format: [Compiler] Total CALLs: N, MOVE before CALL: M (X%), local func: L, optimizable: O (Y% of local)
    stats=$(echo "$output" | grep "\[Compiler\]" | head -1)
    
    if [ -n "$stats" ]; then
        # Parse the stats line
        # Example: [Compiler] Total CALLs: 77, MOVE before CALL: 12 (15.6%), local func: 24, optimizable: 12 (50.0% of local)
        total_calls=$(echo "$stats" | grep -o "Total CALLs: [0-9]*" | cut -d' ' -f3)
        move_before=$(echo "$stats" | grep -o "MOVE before CALL: [0-9]*" | cut -d' ' -f4)
        move_pct=$(echo "$stats" | grep -o "MOVE before CALL: [0-9]* ([0-9.]*%)" | grep -o "([0-9.]*%)" | tr -d '()%')
        local_func=$(echo "$stats" | grep -o "local func: [0-9]*" | cut -d' ' -f3)
        optimizable=$(echo "$stats" | grep -o "optimizable: [0-9]*" | cut -d' ' -f2)
        opt_pct=$(echo "$stats" | grep -o "optimizable: [0-9]* ([0-9.]*%" | grep -o "([0-9.]*%" | tr -d '(%' || echo "0")
        
        echo "$rel_path|$total_calls|$move_before|$move_pct|$local_func|$optimizable|$opt_pct" >> "$TMP_FILE"
    else
        echo "$rel_path|NO_STATS|0|0|0|0|0" >> "$TMP_FILE"
    fi
done

# Now output formatted results
echo ""
echo "Results Summary"
echo "=================================================="
printf "%-40s %10s %10s %10s %10s %10s %10s\n" "Benchmark" "Total" "MOVE" "MOVE%" "Local" "Optim" "Opt%"
printf "%-40s %10s %10s %10s %10s %10s %10s\n" "--------" "-----" "----" "-----" "-----" "-----" "----"

# Calculate totals
total_calls_sum=0
total_move_sum=0
total_local_sum=0
total_optimizable_sum=0

while IFS='|' read -r name total move move_pct local optim opt_pct; do
    if [ "$total" != "ERROR" ] && [ "$total" != "NO_STATS" ]; then
        printf "%-40s %10s %10s %9s%% %10s %10s %9s%%\n" "$name" "$total" "$move" "$move_pct" "$local" "$optim" "$opt_pct"
        
        # Sum for totals
        total_calls_sum=$((total_calls_sum + total))
        total_move_sum=$((total_move_sum + move))
        total_local_sum=$((total_local_sum + local))
        total_optimizable_sum=$((total_optimizable_sum + optim))
    fi
done < "$TMP_FILE"

echo ""
printf "%-40s %10s %10s %10s %10s %10s\n" "--------" "-----" "----" "-----" "-----" "-----"

# Calculate percentages
if [ $total_calls_sum -gt 0 ]; then
    total_move_pct=$(echo "scale=1; 100 * $total_move_sum / $total_calls_sum" | bc)
else
    total_move_pct="0.0"
fi

if [ $total_local_sum -gt 0 ]; then
    total_opt_pct=$(echo "scale=1; 100 * $total_optimizable_sum / $total_local_sum" | bc)
else
    total_opt_pct="0.0"
fi

printf "%-40s %10s %10s %9s%% %10s %10s %9s%%\n" "TOTALS" "$total_calls_sum" "$total_move_sum" "$total_move_pct" "$total_local_sum" "$total_optimizable_sum" "$total_opt_pct"

echo ""
echo "Analysis:"
echo "- Total CALLs analyzed: $total_calls_sum"
echo "- MOVE before CALL: $total_move_sum ($total_move_pct%)"
echo "- Local function CALLs: $total_local_sum"
echo "- Optimizable (no conflict): $total_optimizable_sum ($total_opt_pct% of local)"
echo ""
echo "Potential impact: Eliminating $total_move_sum MOVE instructions"
echo "would save approximately $total_move_sum dispatch cycles per run."

# Show top candidates
echo ""
echo "Top 10 candidates by optimizable count:"
echo "=================================================="
sort -t'|' -k6 -n -r "$TMP_FILE" | head -10 | while IFS='|' read -r name total move move_pct local optim opt_pct; do
    if [ "$total" != "ERROR" ] && [ "$total" != "NO_STATS" ]; then
        printf "%-40s %10s optimizable\n" "$name" "$optim"
    fi
done
