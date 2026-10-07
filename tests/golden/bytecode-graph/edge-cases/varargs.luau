--!golden status: ok
--!golden bytecode-graph.args: --verify-bytecode-graph=roundtrip --verify-bytecode-graph=use-consistency --verify-bytecode-graph-policy=require-pass
--!golden bytecode-graph.status: ok

-- Variadic forwarding preserves the number and order of values.
local function forward(...: number): ...number return ... end
local a, b, c = forward(2, 3, 5)
assert(a == 2 and b == 3 and c == 5)
assert(select("#", forward(2, 3, 5)) == 3)
