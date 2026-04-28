# sml2c — a SysML v2 compiler in C

A compiler for a textual subset of OMG SysML v2, written in hand-rolled C
following the style of *Crafting Interpreters* (clox).  Single-file source
input, validator-first design, multiple back-ends.

## What it does

Scans, parses, resolves names, type-checks, validates structure, and
emits.  The pipeline runs as seven passes on an AST:

```
source ─┬─► scanner ─► parser ─► resolver ─► typechecker
        │                                         │
        │                                         ▼
        │                                    redefchecker
        │                                         │
        │                                         ▼
        │                                    connectchecker
        │                                         │
        │                                         ▼
        │                                    referentialchecker
        │                                         │
        │                                         ▼
        └────────────► back-end (AST print | JSON | SysML)
```

Every pass after the parser is independently bypassable via a `--no-*`
flag, so it's straightforward to inspect intermediate state when debugging.

## Build and run

```
make                # builds bin/sml2c
make test-all       # 36 strict pass/fail tests
make test-graphsml  # 19 graphsml adapter conversions
./verify-tokens.sh  # sanity-check parser ↔ scanner token consistency

bin/sml2c file.sysml                # full pipeline, prints AST tree
bin/sml2c --tokens file.sysml       # token stream
bin/sml2c --emit-json file.sysml    # AST as JSON
bin/sml2c --emit-sysml file.sysml   # canonical SysML (round-trip)
bin/sml2c --no-typecheck file.sysml # skip the typechecker
```

C11, no external dependencies.  Tested with both `gcc` and `clang`.

## Language subset

Supported syntactic forms:

```
package P { ... }
import A::B::C;            import A::B::*;
part def Engine { ... }    part myEngine : Engine;
port def Sensor { ... }    port iface : Sensor;     port iface : ~Sensor;  // conjugated
interface def Bus { ... }
item def Signal { ... }
connection def Wire { ... }
flow def DataFlow { ... }
attribute name : Type = expr;
ref name : Type;           // bare ReferenceUsage
end name : Type;           // inside connection/flow defs
enum def Color { red; green; blue; }
alias E for A::B::C;
comment N about X /* body */
dependency d from A to B;
doc /* description */
connect a.x to b.y;        flow from a.x to b.y;

constraint def TorqueLimit { in t : Real; in m : Real; t <= m }
assert constraint c : TorqueLimit;
assert constraint { x > 0 and x < 100 }   // anonymous inline
requirement def MaxTorque {
    subject e : Engine;
    require constraint c : TorqueLimit;
}
require requirement r : MaxTorque;

action def DeliverGifts {
    in route : Route; out delivered : Boolean;
    first start; then load; then ship; then done;
}
action def Process {
    first start;
    then action prep { in ready : Boolean; }
    then action ship : Travel;
    then done;
}
succession s1 first a then b;

state def EngineStates {
    action initial : Initialize;
    state off; state starting;
    state on { entry MonitorTemp; do ProvidePower; exit ApplyBrake; }
    transition initial then off;
    transition off_to_on first off accept Start then on;
}
exhibit state engineStates : EngineStates;
```

Modifiers: `public` / `private` / `protected` (visibility), `in` / `out` /
`inout` (direction), `derived` / `abstract` / `constant` / `ref` (feature).

Relationships: `: T` (typing), `:>` or `specializes` (specialization),
`:>>` or `redefines` (redefinition), `[lo..hi]` (multiplicity).

Expressions: integer / real / string / boolean literals, qualified-name
references, `+` `-` `*` `/`, `==` `!=` `<` `<=` `>` `>=`, `and` / `or`
(SysML keyword forms), prefix `-` and `!`, parenthesized grouping.

Spec-derived validation rules (§8.3 of the SysML v2 spec):

- Every `:>>` redefinition must point at a real feature in the
  redefining type's supertype chain
- Connection ends must reference port-typed features
- Flow direction must be source-out → target-in (with conjugation flip)
- Flow definitions may have at most two ends
- Six referential rules from §8.3.6.4, §8.3.7.2, §8.3.7.3, §8.3.12.6,
  §8.3.16.2 — directed/end/top-level usages and PortUsages outside
  port contexts are inferred referential; AttributeUsages and
  AttributeDefinition features are unconditionally referential
- Constant / derived attributes must have an initializer / derivation
- Enum value initializers are tags within an enum def; user-written
  enum-typed attributes still require type-compatible default values
- Constraint def bodies must be Boolean expressions (typechecker)
- Constraint usages (`assert/assume/require constraint`) must reference
  a constraint def; requirement usages (`require requirement`) must
  reference a requirement def
- Constraint and requirement usages require an explicit assertion
  prefix (`assert` / `assume` / `require`); bare usages are reserved

## Directory layout

```
main.c                      CLI entry point and flag dispatch
include/                    public + private headers
  ast.h                       Node tree definition (15 kinds)
  scanner.h                   tokens, scanner API
  parser.h                    public parse() entry
  parser_internal.h           shared between the three parser files
  resolver.h                  public resolveProgram() entry
  resolver_internal.h         shared between the two resolver files
  typechecker.h               typecheckProgram() entry
  redefchecker.h              checkRedefinitions() entry
  connectchecker.h            checkConnections() entry
  referentialchecker.h        checkReferential() entry
  builtin.h                   synthetic ScalarValues stdlib
  codegen_json.h              --emit-json back-end
  codegen_sysml.h             --emit-sysml back-end (round-trip)
src/
  scanner.c                   lexer, 68 token types
  parser_common.c             Parser state, token machinery, multiplicity
  parser_expr.c               Pratt expression parser
  parser_decl.c               declaration grammar, dispatch
  ast.c                       node construction, list helpers, astPrint
  resolver_scope.c            Scope/Symbol primitives, lookups
  resolver.c                  resolution drivers, walker
  typechecker.c               expression typing + relationship validation
  redefchecker.c              redef target chain validation
  connectchecker.c            connection/flow direction rules
  referentialchecker.c        the six referential-inference rules
  builtin.c                   ScalarValues package construction
  codegen_json.c              JSON emission
  codegen_sysml.c             canonical SysML pretty-printer
test/
  *.sysml                     positive tests (must compile cleanly)
  bad/*.sysml                 negative tests (must report ≥1 error)
tools/
  sml2c_to_graphsml.py        Python adapter: --emit-json → graphsml2 elements
  render_drawio.py            wires the adapter to drawpyo + graphsml2
verify-tokens.sh              checks every TOKEN_X used in src/ is declared
Makefile                      build, test, test-graphsml targets
```

## Pipeline detail

**Scanner** (clox-style): one-shot `scanToken()` pulled lazily by the
parser, tokens hold `(start, length)` pointers into the source buffer
with no allocation.  68 token types covering punctuation, keywords,
literals, doc-comment bodies, and a stash for block-comment bodies
(consumed by `comment` declarations).

**Parser**: hand-written recursive descent for declarations, Pratt
parser for expressions.  Panic-mode error recovery — first error in
a rule sets `panicMode`, the top-level loop synchronizes at safe
restart points.  Split across three files with a private internal
header.

**Resolver**: two-phase scoped symbol table.  Phase 1 declares every
named child member, phase 2 walks references and resolves them through
the scope chain.  Multi-segment names walk member-of-type with a
synthetic stdlib (ScalarValues) injected at the root.  Aliases are
dereferenced at the lookup boundary so the AST keeps the alias node
but consumers see through to the target.  Conjugation parity is
tracked along multi-segment chains so the connection checker can flip
port directions correctly through `~Sensor`-typed features.

**Typechecker**: every expression typed bottom-up; attribute defaults
checked against declared types; enum-tag literals relaxed only when
inside an enum def body.  `constant` and `derived` modifiers required
to have a value / derivation expression.

**Redefchecker**: every `:>>` validated against the supertype chain
of the redefining type's owner.  Multi-segment qualifiers must match
a real path; type narrowing checked when the qualifier is fully
validated.

**Connectchecker**: connection ends must be port-typed; flow source
must be `out`/`inout` and target `in`/`inout`; conjugation flips the
effective direction.  `connect a to b` and `flow from a to b` both
go through this pass.

**Referentialchecker**: six rules from the spec — directed usages,
end features, top-level usages, AttributeUsages, AttributeDefinition
features, and non-port nested usages of a port — are inferred
referential.  Flow definitions may have at most two ends.

**Back-ends**: `astPrint` emits a tree-shaped human-readable summary;
`emitJson` emits the AST as JSON; `emitSysml` round-trips canonical
SysML source.  All 19 positive tests round-trip to byte-identical
JSON ASTs through the SysML emitter.

## Tests

There are three test layers:

1. **Strict pass/fail** (`make test-all`): every `test/*.sysml` must
   compile cleanly; every `test/bad/*.sysml` must report at least one
   error.  42 tests.

2. **graphsml adapter** (`make test-graphsml`): every `test/*.sysml`
   converts cleanly through the Python adapter.  21 tests.

3. **Round-trip** (manual via shell): every `test/*.sysml` emits SysML
   via `--emit-sysml`, re-parses, and the two ASTs are byte-identical
   in JSON.  21 tests.

## Drawio integration

`tools/sml2c_to_graphsml.py` converts `--emit-json` output to wrapper
objects that match the protocol `graphsml2.Classifier` expects.  Zero
runtime dependencies for the adapter itself; the optional render
script (`tools/render_drawio.py`) imports `drawpyo` and `graphsml2`
to produce a `.drawio` file.

```
sml2c --emit-json file.sysml | python tools/sml2c_to_graphsml.py - | head
sml2c --emit-json file.sysml | python tools/render_drawio.py - out.drawio
```

## Status

v0.6.  Adds calc def (`calc def F { in p : T; return r : T = expr; }`),
function-call expressions (`Compute(mass, factor)`), and member-access
expressions (`engine.mass`, `vehicle.engine.cost`).  Together these
complete the core *expression* grammar — every initializer expression
in real SysML is now parseable, and the AST distinguishes namespace
lookup (`A::B`) from runtime member access (`a.b`).  The
referentialchecker no longer pollutes round-trip emit (v0.5 fix);
calls and member access live in expression position only.

The big design pivot of v0.6: `.` is now strictly the member-access
operator in expression contexts, distinct from `::` which remains the
qualified-name separator.  Pre-v0.6, `engine.mass` parsed as the
single qualified name `engine::mass`; post-v0.6 it produces a
NODE_MEMBER_ACCESS that the typechecker (v0.10) can resolve through
the engine's type to find `mass`.

## License & origin

Written collaboratively by Claude (Anthropic) and the project author over
multiple sessions.  Style follows Bob Nystrom's *Crafting Interpreters*
clox idioms.
