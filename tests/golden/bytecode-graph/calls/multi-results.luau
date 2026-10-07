--!golden status: ok
--!golden bytecode-graph.args: --verify-bytecode-graph=roundtrip --verify-bytecode-graph=use-consistency --verify-bytecode-graph-policy=require-pass
--!golden bytecode-graph.status: ok

-- Calls preserve multiple results when returned and forwarded.
local function pair(): (number, number) return 4, 7 end
local function forward(): (number, number) return pair() end
local a, b = forward()
assert(a == 4 and b == 7)
local values = {pair()}
assert(#values == 2 and values[2] == 7)
