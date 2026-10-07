--!golden status: ok
--!golden bytecode-graph.args: --verify-bytecode-graph=roundtrip --verify-bytecode-graph=use-consistency --verify-bytecode-graph-policy=require-pass
--!golden bytecode-graph.status: ok

-- Each nested function retains values from its outer scopes.
local function outer(a: number): (number) -> () -> number
    return function(b: number): () -> number
        return function(): number return a + b end
    end
end
assert(outer(2)(3)() == 5)
