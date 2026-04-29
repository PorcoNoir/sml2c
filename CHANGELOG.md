# CHANGELOG

Per-version notes for sml2c.  Latest first.

The format follows roughly: what shipped, why it mattered, file-level
effect.  Earlier versions (v0.1–v0.12) are reconstructed from journal
summaries — the detail is lighter than for v0.13 onward, where I have
the working tree at the time of release.

---

## v0.22 — Calc defs to C functions

First step of the executable-target pivot from
`design/c-codegen.md`.  Every emittable `calc def F` lowers to a free
C function; calls to calc defs in expressions lower to bare-name C
function calls; intermediate `attribute` body members become local C
variables.

```sysml
calc def Scale {
    in mass : Real;
    in factor : Real;
    attribute scaled = mass * factor;
    return result : Real = scaled;
}
calc def SumOfSquares {
    in a : Real;
    in b : Real;
    return total : Real = Square(a) + Square(b);
}
```

emits

```c
double Scale(double mass, double factor);
double SumOfSquares(double a, double b);

double Scale(double mass, double factor) {
    double scaled = (mass * factor);
    return scaled;
}

double SumOfSquares(double a, double b) {
    return (Square(a) + Square(b));
}
```

Forward-declare-then-define structure means mutual recursion works.
Intermediate attributes without a declared type fall back to
`Node.inferredType` from the typechecker, which is what makes
`attribute scaled = mass * factor;` (no `: Real`) lower correctly.

**Side effect.**  Top-level `static const` attributes whose default
contains a function call now skip with
`/* skipped: NAME (non-const initializer — needs v0.23 init function) */`
because C doesn't allow function calls in const initializers.  v0.23
will lift these into a generated `__sml2c_init` function.

**New gate: `make test-c-run`.**  For each `test/<name>.sysml` with a
companion `test/<name>.driver.c` and `test/expected/<name>.expect`,
the gate compiles + links + runs + diffs.  This is the runtime-level
equivalent of `make test-c` (which only checks syntactic validity).
Wired into `make sweep`.

```
$ make sweep
==> verify-tokens
OK: all 132 referenced tokens are declared.
==> test-all (strict)
  73 passed, 0 failed
==> test-c (cc -fsyntax-only)
  C codegen: 46 passed, 0 failed
==> test-c-run (cc + ./binary + diff)
  C runtime: 1 passed, 0 failed
==> test-graphsml
  graphsml adapter: 46 passed, 0 failed
==> test-ptc
  PTC: parser=0, default=15 (baseline 15)

sweep: all gates green
```

**Files touched:** `src/codegen_c.c` (+~150 lines), `Makefile` (new
`test-c-run` target, sweep extended); new `test/CalcEmit.sysml`,
`test/CalcEmit.driver.c`, `test/expected/CalcEmit.expect`.

---

## v0.21 — Docs reorganization

Pure documentation churn.  README sliced down to the architectural
overview + current status; per-version detail moved into this
CHANGELOG; a long-form narrative of the project's collaborative
history added as `chat_history.md`.

No code changes.  No test changes.  All sweep gates remain green
(73 strict, 46 graphsml, 46 test-c, 132 verify-tokens, PTC
parser=0 / default=15).

**Files touched:** `README.md` (rewritten to ~180 lines from
~600), `CHANGELOG.md` (new), `chat_history.md` (new).

---

## v0.20 — Refactor + test infrastructure

Zero behavior change.  The diff consolidates code paths from
v0.18 and v0.19 plus adds an aggregate test target.

```
                          v0.18  v0.19  v0.20
PTC parser-only errors:       0      0      0
PTC default-mode errors:     35     15     15
strict tests:                72     73     73
test-c (cc -fsyntax-only):   45     46     46
graphsml round-trip:         45     46     46
```

**Refactor.**  Two helpers extracted to `resolver_scope.c`:

- `qnameLastSegment(qname)` — returns the trailing identifier of
  `A::B::C` (i.e. `C`), or an empty token for non-qnames.  Used
  by both v0.19 fixes and the v0.18 wildcard fix when they
  needed to derive the user-visible name of a re-exported decl.
- `declareNameIfFree(scope, name, decl)` — soft variant of
  `declareName`: returns false if `name` is already declared in
  `scope`, rather than raising a duplicate-name error.  Captures
  the "imports and aliases never shadow real decls" rule both
  v0.19 fixes had open-coded with a `lookupLocal` guard.

`declareMembers` now reads as two short passes (named-member
decls + redefines aliases; then imports + non-wildcard aliases)
that share both helpers.  About 30 lines gone, intent of the
two passes much clearer.

The parser's `perform` form also got a small cleanup — the
multi-segment-reference and bare-name-reference branches were
identical except for whether the trailing while-loop ran, so
they collapse to one branch with a 2-line preamble that picks
"declaration vs reference" once and routes accordingly.

**Test infrastructure.**  Two new Makefile targets:

- `test-ptc` runs the PTC reference file in two modes.
  Parser-strict mode is a hard gate: any error fails.  Default
  mode is tracked against a `PTC_BASELINE` variable (currently
  15) so a regression introduced by an unrelated change shows up
  as a test failure rather than slipping through silently.
  Override via `make test-ptc PTC_BASELINE=N` after a deliberate
  improvement.  Skipped silently if `$(PTC_FILE)` doesn't exist.
- `sweep` runs every gate in cheapest-first order (verify-tokens,
  test-all, test-c, test-graphsml, test-ptc) with a one-line
  summary per gate and a unified verdict.  Replaces the
  five-separate-commands ritual that earlier turns followed.

**Files touched:** `src/resolver.c`, `src/resolver_scope.c`,
`src/parser_decl.c`, `include/resolver_internal.h`, `Makefile`,
`README.md`.

---

## v0.19 — More PTC resolver coverage (35 → 15 errors, −57%)

Two name-into-scope fixes.  The member-walking machinery from
v0.18 is unchanged; both fixes register a name-to-decl binding
in the local scope.

**Fix A: non-wildcard imports bring single names into scope.**
PTC drags individual definitions across sibling packages with
`public import ContextDefinitions::TransportPassenger;` and then
uses the unqualified name later.  Pre-v0.19 this was a no-op;
the resolver only acted on wildcard imports.  Now non-wildcard
imports declare an alias in the current scope under the qname's
last segment, pointing at the resolved target.  Drops PTC errors
35 → 19.

**Fix B: anonymous `redefines` usages are visible by the
redefined name.**  PTC does

```sysml
part vehicle_b : Vehicle {
    perform ActionTree::providePower redefines providePower;
    part rearAxleAssembly {
        perform providePower.distributeTorque;     // ← needed this
    }
}
```

The `perform ActionTree::providePower redefines providePower`
parses as an anonymous USAGE.  The reference at
`providePower.distributeTorque` then needs `providePower` to
bind to *this* redefining usage — because that one carries
`:> ActionTree::providePower` which gets us into the body where
`distributeTorque` lives.  Without the fix, lookup matched the
inherited `Vehicle::providePower` stub instead.  Drops PTC
errors 19 → 15.

**Diagnostics scrubbed.**  Several earlier turns left
`SML2C_TRACE_MEMBER` env-var shims in `resolver_scope.c` and
`resolver.c`.  All gone now.  No runtime cost path remains.

**Files touched:** `src/resolver.c`, `README.md`; new
`test/SingleNameImports.sysml`.

---

## v0.18 — PTC resolver fixes (61 → 35 errors, −43%)

Pivots from C-codegen buildout to the structural problem
blocking PTC end-to-end: 61 default-mode resolution errors on a
1580-line industry-standard SysML file.

The 61 errors fell into four categories:

**1. Transitive wildcard imports.**  `searchWildcardImports`
only inspected direct members of an imported package; it
ignored that package's own `public import X::*` re-exports.
PTC chains imports several levels deep:

```sysml
package SimpleVehicleModel {
    public import Definitions::*;
    package Definitions {
        public import PartDefinitions::*;
        package PartDefinitions { part def Vehicle { ... } }
    }
}
```

`searchPackageReExports` follows public/default-visibility
wildcard imports transitively, with a visited-set capped at 32
packages.  Private imports are deliberately not re-exported.
Dropped PTC 61 → 40.

**2. Succession-target visibility.**  In SysML use-case bodies,
the targets of `then action <name>` clauses are members of the
enclosing use case.  Member lookup walked direct
`as.usage.members` but ignored succession targets.
`lookupMemberDepth` now flattens `succession.targets` and
`succession.first` into the member-name search.

**3. Bare `perform <name>;` no longer shadows the referent.**
Previously this produced a fresh empty action usage *named*
`<name>`, declared in the enclosing scope and shadowing any
outer use case of the same name.  v0.18 distinguishes:

| Source                            | Effect                                  |
|-----------------------------------|-----------------------------------------|
| `perform action foo;`             | declares — `action` keyword is explicit |
| `perform foo redefines bar;`      | declares — redefinition clause follows  |
| `perform foo : T;`                | declares — typed                        |
| `perform foo;`  (bare)            | references — anonymous usage with qname stored as a specialization |
| `perform a.b.c;`                  | references — multi-segment qname        |

Dropped PTC 40 → 35.

**4. Import targets resolve against the inner scope.**  PTC and
most real SysML files declare supporting packages and import
them in the same scope:

```sysml
package Definitions {
    public import PartDefinitions::*;     // points to a sibling
    package PartDefinitions { ... }
}
```

The old single-pass `declareMembers` resolved import targets
against the outer scope, which couldn't see `PartDefinitions`
because that name only exists inside `Definitions`.  Split into
two passes.

**Files touched:** `src/resolver_scope.c`, `src/resolver.c`,
`src/parser_decl.c`, `README.md`; new
`test/TransitiveImports.sysml`.

---

## v0.17 — Fixed multiplicity → C arrays

A struct member with a constant single-bound multiplicity now
lowers to a fixed-size C array.

```sysml
part def Wheel { attribute size : Real; }
part def Car {
    attribute mass   : Real;
    attribute scores : Real [3];
    part wheels      : Wheel [4];
}
```

emits

```c
typedef struct { double size; } Wheel;
typedef struct {
    double mass;
    double scores[3];
    Wheel wheels[4];
} Car;
```

Implementation: a small `MultKind` classifier with three states
(NONE / FIXED / UNSUPPORTED).  Both attribute fields and
part-usage fields go through the same classifier so the rule is
uniform.  Top-level `static const` attributes with multiplicity
are skipped with a comment for now (lowering them needs
tuple-literal initializers).

**Files touched:** `src/codegen_c.c`, `README.md`; new
`test/MultiplicityArrays.sysml`.

---

## v0.16 — Nested struct fields

`part def`s composed of other `part def`s now lower to nested C
struct fields rather than scalar-only structs.  Required adding
an `EmittableDefs` registry computed via fixed-point pre-pass —
a definition is emittable iff every field's type is
emittable — so generated C compiles in dependency order.

**Files touched:** `src/codegen_c.c`; new
`test/NestedFields.sysml`.

---

## v0.15 — `--emit-c` back-end

First slice of C codegen.  Top-level `static const` declarations
from primitive-typed attributes; simple struct typedefs from
primitive-only part defs.  New `make test-c` target that pipes
each test's emitted C through `cc -fsyntax-only`.  42 test-c
pass at this point.

**Files touched:** new `src/codegen_c.c`, `include/codegen_c.h`;
new `test/InferredTypes.sysml`, `test/EmitC.sysml`; `Makefile`
adds `test-c`.

---

## v0.14 — Typed-AST groundwork

Adds `Node.inferredType` field, typeOf caches, JSON `"type"`
emission for Literal / Binary / Unary / QualifiedName / Call /
MemberAccess.  Foundation for v0.15+ codegen — the inferred
types are what `--emit-c` reads to pick C primitive lowerings.

---

## v0.13 — Visitor framework

NodeKind tree-walk visitor extracted to `src/ast_walk.c` and
`include/ast_walk.h`.  All four passes (resolver, typechecker,
redefchecker, connectchecker) converted to use it.  Centralizes
the per-NodeKind switch that was duplicated four times before.

---

## v0.7 – v0.12 — Parser hardening

Multiple turns of incremental parser fixes after v0.6.  The
recurring discipline established here: run the user's *default*
invocation (full pipeline, including resolver) on every test
file, not just the parser layer.  Resolver-level errors caught a
lot of parser holes that `--no-resolve` testing missed.  By
v0.12 the strict / graphsml / round-trip test suites were all in
place and passing on a stable corpus.

Specific milestones I have notes on:
- **v0.10** added state-machine grammar — `state def`, `state
  s { entry…; do…; exit…; }`, `transition`, `exhibit state`.
- **v0.11–v0.12** centralized DefKind metadata so the parser's
  per-kind tables aren't duplicated across emitters.

---

## v0.2 – v0.6 — Behavioral features

Constraints, requirements, actions, then calc defs / function
calls / member access.

- **v0.2** added `constraint def`, `assert constraint`,
  `requirement def`, `require requirement`.
- **v0.3 – v0.5** added `action def` with successions:
  `first start; then load; then ship; then done;`.
- **v0.6** added `calc def`, function-call expressions, and
  member-access on qualified names (`engine.power`).

In hindsight this is where the "validator-first" disposition
crystallized — the actions work needed succession ordering rules
that were easier to express as a separate pass over the typed
AST than as parser invariants.

---

## v0.1 — First shipped release

The first tarball.  At this point the compiler had:

- Single contiguous source buffer; tokens are
  `(start, length)` pointers
- Hand-rolled recursive descent for declarations, Pratt parser
  for expressions
- 15-kind AST with an idiomatic Node arena
- Two-phase resolver (declare members in scope, then resolve
  references) with synthetic stdlib (`ScalarValues`)
- Bottom-up typechecker with enum semantics
- Redefchecker for `:>>` validation against supertype chains
- Connectchecker enforcing port-typing and direction with
  conjugation parity
- Referentialchecker for the six §8.3 inference rules
- `--emit-json` and `--emit-sysml` (round-trip) back-ends
- `tools/sml2c_to_graphsml.py` for drawio rendering

The parser had grown to ~1080 lines and resolver to ~600 in a
single file each before v0.1; the split into
`parser_common`/`parser_expr`/`parser_decl` and
`resolver_scope`/`resolver` happened as part of the v0.1
release.

---

## Pre-v0.1 — Genesis

Started 2026-04-26 with the prompt:

> Using craftinginterpreters.com and the SysML v2 Pilot
> Implementation library and examples, I want to create a C
> compiler for .sysml files. (1) Ask me design questions
> wherever applicable. (2) Teach me core compiler constructs.

Multi-day teaching session covering scanner, parser, AST, panic-
mode error recovery, factoring patterns, context-sensitive
lexing for doc comments, Pratt parsing, scoped resolver, member-
of-type lookup, typechecker, redefinition rules, connection
validation, JSON emission.

Working project landed at `/home/claude/sysmlc`, outputs at
`/mnt/user-data/outputs/sml2c-v0.{1..N}.tar.gz`.  Style follows
Bob Nystrom's *Crafting Interpreters* clox idioms (single-file
source buffer, tagged unions for AST, no GC).
