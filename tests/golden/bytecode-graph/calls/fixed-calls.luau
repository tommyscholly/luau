--!golden status: ok
--!golden bytecode-graph.args: --verify-bytecode-graph=roundtrip --verify-bytecode-graph=use-consistency --verify-bytecode-graph-policy=require-pass
--!golden bytecode-graph.status: ok

-- Fixed arity calls can be composed.
local function add(a: number, b: number): number return a + b end
assert(add(add(1, 2), 3) == 6)
