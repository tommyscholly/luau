--!golden status: ok
--!golden bytecode-graph.args: --verify-bytecode-graph=roundtrip --verify-bytecode-graph=use-consistency --verify-bytecode-graph-policy=require-pass
--!golden bytecode-graph.status: ok

-- Nested choices preserve all three outcomes.
local function choose(a: boolean, b: boolean): number
    if a then
        if b then return 1 else return 2 end
    end
    return 3
end
assert(choose(true, true) == 1)
assert(choose(true, false) == 2)
assert(choose(false, true) == 3)
