--!golden status: ok
--!golden bytecode-graph.args: --verify-bytecode-graph=roundtrip --verify-bytecode-graph=use-consistency --verify-bytecode-graph-policy=require-pass
--!golden bytecode-graph.status: ok

-- Both branches return the selected value.
local function choose(flag: boolean, left: number, right: number): number
    if flag then return left else return right end
end
assert(choose(true, 1, 2) == 1)
assert(choose(false, 1, 2) == 2)
