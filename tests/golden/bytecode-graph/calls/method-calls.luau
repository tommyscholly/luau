--!golden status: ok
--!golden bytecode-graph.args: --verify-bytecode-graph=roundtrip --verify-bytecode-graph=use-consistency --verify-bytecode-graph-policy=require-pass
--!golden bytecode-graph.status: ok

-- Method syntax supplies the receiver before explicit arguments.
local object = {value = 4}
function object.add(self: {value: number}, delta: number): number return self.value + delta end
assert(object:add(3) == 7)
