--!golden status: ok
--!golden bytecode-graph.args: --verify-bytecode-graph=roundtrip --verify-bytecode-graph=use-consistency --verify-bytecode-graph-policy=require-pass
--!golden bytecode-graph.status: ok

-- Closures retain the captured value after the factory returns.
local function capture(value: number): () -> number
    return function(): number return value end
end
assert(capture(12)() == 12)
