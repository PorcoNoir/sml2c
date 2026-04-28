Proposed roadmap
I'd group the rest of the road into ~15 turns:

v0.4 — states + transitions (the biggest T1 hole)
v0.5 — calc def, function-call expressions, power, units suffix
v0.6 — member access, null/xor, redefines-with-init, subsets
v0.7 — perform, bind, named flows, recursive imports **
v0.8 — architecture refactor (visitor pass abstraction) — by here the cross-cutting tax will hurt
v0.9 – v0.13 — Tier 2 parse-and-skip features in batches (annotations, use cases, allocations, viewpoints, individuals)
v0.14 — full file parses + resolves cleanly
v0.15 — typed AST: every expression gets inferredType, every qname is bound or rejected
v0.16 — design + emit the lowered IR
v0.17 – v0.19 — C codegen, by tier (parts/attrs first, then actions, then states)