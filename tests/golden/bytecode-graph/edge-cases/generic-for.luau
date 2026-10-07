--!golden status: ok
--!golden bytecode-graph.args: --verify-bytecode-graph=roundtrip --verify-bytecode-graph=use-consistency --verify-bytecode-graph-policy=require-pass
--!golden bytecode-graph.status: ok

-- Generic iteration visits every sequence element.
local sum = 0
for index, value in ipairs({2, 3, 5}) do sum += index * value end
assert(sum == 23)
