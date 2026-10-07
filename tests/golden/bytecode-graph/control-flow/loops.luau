--!golden status: ok
--!golden bytecode-graph.args: --verify-bytecode-graph=roundtrip --verify-bytecode-graph=use-consistency --verify-bytecode-graph-policy=require-pass
--!golden bytecode-graph.status: ok

-- Numeric, while, and repeat loops accumulate their values.
local sum = 0
for i = 1, 5 do sum += i end
local i = 0
while i < 3 do i += 1; sum += i end
repeat sum -= 1 until sum == 20
assert(sum == 20)
