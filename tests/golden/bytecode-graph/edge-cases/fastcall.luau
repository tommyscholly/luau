--!golden status: ok
--!golden bytecode-graph.args: --verify-bytecode-graph=roundtrip --verify-bytecode-graph=use-consistency --verify-bytecode-graph-policy=require-pass
--!golden bytecode-graph.status: ok

-- Builtin calls produce the same values through fastcall paths.
local function magnitude(value: number): number return math.abs(value) end
assert(magnitude(-7) == 7)
assert(math.max(3, 8) == 8)
assert(math.floor(2.8) == 2)
