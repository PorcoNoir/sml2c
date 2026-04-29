# sml2c — Project history

This document is a long-form narrative of how sml2c was built: the
original goal, the design choices that crystallized along the way,
the recurring patterns that worked, the debugging stories that
mattered.  It complements the `CHANGELOG.md` (which is per-version
and terse) and the `README.md` (which describes the current state).

The project was built collaboratively across roughly two days of
sessions starting 2026-04-26.  Code was written by Claude
(Anthropic) at the project author's direction, with design
questions surfaced and answered along the way.

---

## 1. Genesis

The opening prompt:

> Using craftinginterpreters.com and the SysML v2 Pilot
> Implementation library and examples, I want to create a C
> compiler for .sysml files.
>
> 1. Ask me design questions wherever applicable
> 2. Teach me core compiler constructs from the
>    craftinginterpreters website

Two things were ambiguous at the start.  First, "C compiler" — did
that mean a compiler *written in* C, a compiler that *emits* C, or
both?  Second, what should "compiling" SysML even produce?  SysML
is a modeling language, not a programming language — at the core
it describes systems, not algorithms.  So the output could plausibly
be a static analysis report, a code skeleton, a visualization, an
executable behavioral simulation, or something else entirely.

The decisions that came out of that first conversation:

- **Written in C, *Crafting Interpreters* clox-style.**  Single
  contiguous source buffer, tagged unions for AST, no GC.  This
  was the part the author wanted to learn from in any case.
- **A subset of SysML v2, validator-first.**  Don't try to cover
  every form in the spec; pick the structurally rich core
  (packages, definitions, usages, ports, connections, flows,
  attributes, constraints, requirements, actions, states) and
  build the validation passes the spec calls for in §8.3.
- **Multiple back-ends.**  `--emit-json` for downstream tooling,
  `--emit-sysml` for round-trip validation, eventually `--emit-c`
  for a C-header skeleton of the modeled structures.

The "validator-first" choice was the most consequential.  It
shapes everything downstream: the AST is the IR, every checker is
a separate pass over the typed AST, and back-ends consume that
AST without having to re-derive any structural facts.  The
alternative — fold validation into the parser, or into the
back-end — would have made the seven-pass pipeline impossible
without a major refactor later.

---

## 2. The pipeline takes shape

The first day of sessions covered, in order:

- **Scanner** (lazy, one-shot `scanToken()`, tokens as
  `(start, length)` pointers).  68 token types eventually.
  Context-sensitive lexing for doc-comment bodies (`/* ... */`
  attached to a `doc` keyword takes the body verbatim).
- **Parser** (hand-rolled recursive descent for declarations,
  Pratt parser for expressions).  Panic-mode error recovery —
  first error in a rule sets `panicMode`, the top-level loop
  synchronizes at safe restart points like `}` or `;`.
- **AST** (15 node kinds, tagged-union-style with a
  per-kind `as` member).  Anonymous nodes (no name) are normal —
  `connect a to b;` produces a nameless usage, for instance.
- **Resolver** (two-phase: declare members in scope, then walk
  references).  Synthetic stdlib (`ScalarValues`) injected at the
  program root so wildcard imports of `ScalarValues::*` find real
  symbols.  Aliases dereferenced at the lookup boundary, not
  eagerly rewritten.
- **Typechecker** (bottom-up; attribute defaults checked against
  declared types; enum-tag literals relaxed inside enum def
  bodies).
- **Redefchecker** (every `:>>` validated against the supertype
  chain of the redefining type's owner).
- **Connectchecker** (connection ends must be port-typed; flow
  direction must be source-`out` → target-`in`; conjugation flips
  the effective direction through `~Sensor`-typed features).
- **Referentialchecker** (six §8.3 inference rules: directed
  usages, end features, top-level usages, AttributeUsages,
  AttributeDefinition features, non-port nested usages of a port).
- **Back-ends**: `astPrint`, `--emit-json`, `--emit-sysml`
  (round-trip).

By the end of day 1, the parser had grown to ~1080 lines and the
resolver to ~600.  They were still in single files each, and the
author asked to be consulted if a more robust architecture was
identified.  The answer was: yes, split the parser into
`parser_common.c` (token machinery, multiplicity helpers),
`parser_expr.c` (Pratt expression parser), and `parser_decl.c`
(declaration grammar).  Split the resolver into `resolver_scope.c`
(scope/symbol primitives) and `resolver.c` (drivers and walker).
Both splits used a private internal header (`parser_internal.h`,
`resolver_internal.h`) so the public API stayed minimal.

That refactor was v0.1.

---

## 3. Behavioral features (v0.2 – v0.6)

After v0.1 the focus shifted to behavior — the parts of SysML
that describe what a system *does* rather than what it *is*.

**v0.2** added constraints and requirements.

```sysml
constraint def TorqueLimit { in t : Real; in m : Real; t <= m }
assert constraint c : TorqueLimit;
requirement def MaxTorque {
    subject e : Engine;
    require constraint c : TorqueLimit;
}
require requirement r : MaxTorque;
```

The validation rules here are subtle: constraint def bodies must
typecheck to Boolean, constraint *usages* must reference a
constraint *def* (not, say, a part def), and bare uses without an
`assert/assume/require` prefix are reserved.  These rules landed
across the typechecker and a new "constraint shape" check in the
referentialchecker.

**v0.3 – v0.5** added actions and successions.

```sysml
action def DeliverGifts {
    in route : Route; out delivered : Boolean;
    first start; then load; then ship; then done;
}
```

The succession syntax (`first X; then Y;`) is where the AST gained
a NODE_SUCCESSION kind separate from NODE_USAGE.  Successions
carry both `first` (a single-segment reference) and `targets` (an
array of names or inline action declarations).  This decision —
keep successions distinct from usages rather than unifying them —
turned out to matter much later, in v0.18, when the resolver had
to flatten succession targets back into member lookup.

**v0.6** added calc defs (functions with parameters and a return
type), function-call expressions, and member access on qualified
names.  This was the first time the typechecker had to reason
about expression *operations* and not just leaf types.

In retrospect, this is also where the project's "validator-first"
disposition crystallized.  Successions, in particular, have
ordering rules that were much easier to express as a separate pass
over the typed AST than as parser invariants.

---

## 4. Parser hardening (v0.7 – v0.12)

Several turns of incremental parser work followed.  The recurring
pattern, established here:

> **Run the user's *default* invocation on every test, not just
> the parser layer.**  Resolver-level errors caught a lot of
> parser holes that `--no-resolve` testing missed.

A typical example: a user wrote `attribute color : Colors;` where
`Colors` was an enum def declared elsewhere.  The parser produced
a valid USAGE node and `--no-resolve` testing was happy.  But the
resolver, which actually had to look `Colors` up in scope, was the
first pass to notice that the enum def was being parsed as a
PartDef instead.  The bug was in the parser, but only the resolver
could surface it.

This eventually became the v0.12 discipline: every shipped change
runs the full pipeline on every test — `make test-all`.

The state-machine grammar landed in v0.10:

```sysml
state def EngineStates {
    action initial : Initialize;
    state off; state starting;
    state on { entry MonitorTemp; do ProvidePower; exit ApplyBrake; }
    transition initial then off;
    transition off_to_on first off accept Start then on;
}
exhibit state engineStates : EngineStates;
```

With the state grammar in, the language subset was substantively
complete for structural and behavioral modeling.  The work after
v0.12 shifted to architectural cleanup and toward C codegen.

---

## 5. The visitor framework (v0.13)

By v0.12, four passes (resolver, typechecker, redefchecker,
connectchecker) each had their own per-NodeKind switch over the
AST.  Each switch had to know about every node kind, and adding a
new kind (or changing an existing one) meant editing all four.
The duplication wasn't huge but it was real.

v0.13 extracted the descent into `src/ast_walk.c` and
`include/ast_walk.h`.  Each pass now provides a struct of
function pointers (one per kind it cares about) and the walker
handles the recursion.  About 200 lines saved across the four
passes; the per-pass files stayed focused on the validation logic
rather than on tree-walking mechanics.

`-Wswitch` was preserved everywhere so a missed NodeKind in any
pass is a compile error, not a runtime no-op.  That has caught
real bugs at least three times since.

---

## 6. The C back-end (v0.14 – v0.17)

v0.14 was groundwork: `Node.inferredType` field, typeOf caches,
JSON `"type"` emission for the expression-shape nodes (Literal,
Binary, Unary, QualifiedName, Call, MemberAccess).  The inferred
types are what `--emit-c` reads to pick C primitive lowerings:
`Real → double`, `Integer → long long`, `Boolean → bool`,
`String → const char*`.

v0.15 was the first slice of `--emit-c`: top-level `static const`
declarations from primitive-typed attributes; simple struct
typedefs from primitive-only part defs.  A new `make test-c`
target piped each test's emitted C through `cc -fsyntax-only`,
catching invalid C generation independently of the SysML test
suite.  42 test-c pass at this point.

v0.16 added nested struct fields.  This was harder than it looks
because `part def Car { part engine : Engine; }` requires `Engine`
to be defined *before* `Car` in the emitted C.  The fix was an
`EmittableDefs` registry computed via a fixed-point pre-pass: a
definition is emittable iff every field's type is emittable.  This
gave dependency-ordered emission for free, plus a clean way to
reject defs that referenced unsupported features (e.g. ranges in
multiplicities).

v0.17 added fixed multiplicity → C arrays:

```sysml
part def Car {
    attribute scores : Real [3];
    part wheels      : Wheel [4];
}
```

→

```c
typedef struct {
    double scores[3];
    Wheel wheels[4];
} Car;
```

A `MultKind` classifier (NONE / FIXED / UNSUPPORTED) gates which
fields qualify; ranges, wildcards, and zero-bounds reject the
enclosing definition wholesale rather than silently dropping the
dimensionality.

After v0.17 the question was: extend the C back-end to function
emission and struct embedding (calc defs, specialization), or
pivot to fixing the resolver against a real industry-standard
SysML file?  The author chose the latter.

---

## 7. The PTC saga (v0.18 – v0.19)

The reference file was `ptc-25-04-31.sysml` — 1580 lines from the
SysML Practitioner Toolkit Companion, modeling a vehicle as a
hierarchy of part defs, ports, interfaces, actions, and
requirements.  Default-mode resolution failed with **61 errors**.
Parser-only succeeded (zero errors), so the file was being
accepted as syntactically valid SysML; the failures were all in
name resolution, type checking, or downstream passes.

The investigation took two full sessions and surfaced four
distinct bugs.

### Bug 1: wildcard imports weren't transitive

PTC chains its imports several levels deep:

```sysml
package SimpleVehicleModel {
    public import Definitions::*;
    package Definitions {
        public import PartDefinitions::*;
        package PartDefinitions {
            part def Vehicle { ... }
        }
    }
}
```

A reference to `Vehicle` from anywhere inside `SimpleVehicleModel`
should reach through `Definitions::*` → `PartDefinitions::*` →
`Vehicle`.  But the resolver's `searchWildcardImports` only
inspected *direct* members of an imported package; it ignored that
package's own re-exports.

The fix was a new `searchPackageReExports` helper with visited-set
cycle protection (capped at 32 packages).  Private imports are
deliberately not re-exported, matching SysML's visibility
semantics.

This single fix dropped PTC errors **61 → 40**.

### Bug 2: succession targets weren't visible as members

In a use-case body like:

```sysml
use case transportPassenger : TransportPassenger {
    first start;
    then action a { action driverGetInVehicle; }
    then action b;
    then done;
}
```

The names `a` and `b` are conceptually members of the use case,
but they live inside SUCCESSION nodes' targets, not directly in
`as.usage.members`.  Member lookup walked direct members and
missed them.  Fix: flatten `succession.targets` and
`succession.first` into the member-name search inside
`lookupMemberDepth`.

Dropped PTC **40 → 38** ish (this one moved fewer errors than
expected, because most failures were further down the chain).

### Bug 3: bare `perform <name>;` shadowed the referent

Most pernicious of the four.  PTC's mission context has:

```sysml
package TransportPassengerScenario {
    use case transportPassenger : TransportPassenger { ... }
    part missionContext {
        perform transportPassenger;                       // ← intended ref
        part driver {
            perform transportPassenger.a.driverGetIn;     // ← fails
        }
    }
}
```

`perform transportPassenger;` was being parsed as "declare a fresh
empty action usage *named* transportPassenger inside
missionContext."  That declaration shadowed the outer use case in
member lookup, so `transportPassenger.a` looked up `a` inside the
empty stub and got "no member 'a'".

The fix split `perform` into two cases:

| Source                            | Effect                          |
|-----------------------------------|---------------------------------|
| `perform action foo;`             | declares — `action` is explicit |
| `perform foo redefines bar;`      | declares — redefinition follows |
| `perform foo : T;`                | declares — typed                |
| `perform foo;`  (bare)            | references — anonymous          |
| `perform a.b.c;`                  | references — multi-segment      |

The bare form is the only one whose semantics changed.

### Bug 4: imports resolved against the wrong scope

The first three fixes wouldn't take effect because the *targets*
of the imports themselves weren't resolving.  PTC has:

```sysml
package Definitions {
    public import PartDefinitions::*;     // points to a sibling
    package PartDefinitions { ... }
}
```

The single-pass `declareMembers` resolved import targets against
the OUTER scope — the parent of `Definitions` — which couldn't
see `PartDefinitions` because that name only exists inside
`Definitions`.  Split into two passes: declare every named member
into the inner scope first, then resolve imports against the inner
scope.

After all four v0.18 fixes: PTC default-mode at **35 errors**.

### v0.19: the next two fixes

**Fix A: non-wildcard imports declare scope-local aliases.**  PTC
also drags individual definitions across sibling packages with
`public import ContextDefinitions::TransportPassenger;` followed
by unqualified use of `TransportPassenger`.  Pre-v0.19 this was a
no-op.  Now the trailing segment of the import target gets
declared in the local scope, pointing at the resolved decl.

**Fix B: anonymous `redefines` usages bind under the redefined
name.**  PTC does:

```sysml
part vehicle_b : Vehicle {
    perform ActionTree::providePower redefines providePower;
    part rearAxleAssembly {
        perform providePower.distributeTorque;
    }
}
```

The redefining `perform` is an anonymous usage with
`specializes = [ActionTree::providePower]` and
`redefines = [providePower]`.  The reference to `providePower`
inside `rearAxleAssembly` needs to bind to *this* redefining
usage — because that's the one with the link into the body where
`distributeTorque` lives.  Without the fix, lookup matched the
inherited `Vehicle::providePower` stub instead.

Both v0.19 fixes are name-into-scope: register a new local-scope
binding from a name to a decl.  Neither modifies member-lookup,
type inference, or any pass downstream of the resolver.

After v0.19: PTC default-mode at **15 errors** — a 75% cumulative
reduction from the original 61.

---

## 8. The honesty correction

The v0.18 ship report claimed "61 → 27 errors" based on an
intermediate measurement during the diagnostic phase.  When I
ran the actual stable count after all four fixes had landed and
the diagnostic instrumentation was stripped, the real number was
35.  That was a real mistake — claiming a bigger improvement
than the code actually delivered.

The README, ship message, and the project's running tally all got
corrected to 35.  The lesson: measurement during diagnosis is not
the same as measurement after ship.  Strip the diagnostics, do a
clean rebuild, *then* count.

This kind of correction is part of why the discipline matters.
The v0.20 work added a `make test-ptc` gate that records both
parser-strict (must be 0) and default-mode (tracked against
`PTC_BASELINE`).  The default-mode tracking specifically makes
honest reporting easier: if you change something and PTC errors
drop, you bump `PTC_BASELINE`.  If errors rise unexpectedly, the
gate fails.

---

## 9. The refactor pass (v0.20)

After two PTC turns, two duplications were obvious in
`declareMembers`:

1. Both v0.18 fix #4 and v0.19 fix A registered names from
   imports under "the last segment of a qname."
2. Both v0.19 fix A and v0.19 fix B used the same "declare in
   scope only if no real declaration is already there" guard.

Two helpers extracted to `resolver_scope.c`:

- `qnameLastSegment(qname)` — returns `C` from `A::B::C`, or empty
  for non-qnames.
- `declareNameIfFree(scope, name, decl)` — declare only if not
  already present, return `false` on conflict.

`declareMembers` collapsed to two short passes that share both
helpers.  About 30 lines of code gone, the intent of each pass
much clearer.

A third helper, `nodeNameMatches`, had been hanging around in the
resolver from the diagnostic phase — it collapsed the
`length-check / tokensEqual / cast-to-non-const` triplet that
appeared in three places.  Cleanup discipline caught it before
ship.

The parser's `perform` form also got a cleanup: the
multi-segment-reference and bare-name-reference branches were
identical except for whether the trailing while-loop ran.  They
collapsed into one branch.

### Test infrastructure

The v0.20 work also added `make sweep` — a single command that
runs every gate (verify-tokens, test-all, test-c, test-graphsml,
test-ptc) with a one-line summary per gate and a unified
"all gates green" or "FAIL" verdict.  This replaced the
five-separate-commands ritual that earlier turns had been
following.

```
$ make sweep
==> verify-tokens
OK: all 132 referenced tokens are declared.
==> test-all (strict)
  73 passed, 0 failed
==> test-c (cc -fsyntax-only)
  C codegen: 46 passed, 0 failed
==> test-graphsml
  graphsml adapter: 46 passed, 0 failed
==> test-ptc
  PTC: parser=0, default=15 (baseline 15)

sweep: all gates green
```

---

## 10. Documentation reorganization (v0.21)

After v0.20 the README had grown to ~600 lines, of which roughly
two-thirds was per-version detail accumulated turn-by-turn since
v0.15.  The current-state information at the top — what the
compiler does, how to build it, what the test gates are — was
buried.

v0.21 was a pure documentation reshape:

- **README slimmed to ~180 lines.**  Architectural overview
  (pipeline diagram, directory layout, conventions), build
  instructions, test gate reference, current sweep status.  Every
  line answers "what is this thing and how do I use it now."
  Per-version history is one link away in the CHANGELOG.
- **CHANGELOG.md introduced.**  ~370 lines covering v0.21 down
  through pre-v0.1, latest first.  Each entry: what shipped, why
  it mattered, file-level effect.  Earlier entries (v0.1 – v0.12)
  are reconstructed from journal summaries with lighter detail
  than the v0.13+ entries where I had the working tree at the
  time of release.
- **chat_history.md introduced** — this document.  A long-form
  narrative of the project's collaborative history rather than a
  per-version log.  Covers the design choices that crystallized,
  the recurring patterns that worked, and the debugging stories
  that mattered enough to retell.

Zero code changes.  All sweep gates remain green: 73 strict, 46
graphsml, 46 test-c, 132 verify-tokens, PTC parser=0 /
default=15.

The split is meant to scale: future per-version entries go in
CHANGELOG without touching README; future narrative threads
extend chat_history without crowding either of the others.

---

## 11. Recurring patterns

A few patterns came up enough to be worth naming.

### Validator-first

The compiler runs each spec rule as a separate pass over the typed
AST, rather than weaving validation into the parser or back-end.
Cost: a second walk of the tree per checker.  Benefit:

- Each pass is independently bypassable for debugging
  (`--no-typecheck`, `--no-redefcheck`, etc.).
- The AST is the IR — back-ends consume a guaranteed-validated
  tree without re-deriving anything.
- Adding a new validation rule is local: write a new pass.

### -Wswitch as safety net

Every per-pass switch over `NodeKind` is `-Wswitch`'d — adding a
new node kind anywhere produces compile errors in every pass that
hasn't been updated to handle it.  This has saved real bugs three
times that I recall.

### Source-vs-inferred flag separation

When a source-level flag and an inferred fact would naturally
share a slot, keep them separate.  Example: `isReferenceExplicit`
(user wrote `ref`) vs `isReference` (the resolver inferred it via
the §8.3 referential rules).  When we collapsed them once, the
referentialchecker's diagnostics got muddled — was a usage
flagged because it was source-tagged or because we inferred it?
Splitting them back fixed the diagnostics and the round-trip
emitter (which only wants the source flag).

### Ship discipline

The rule that emerged across v0.13–v0.20:

> A turn ends with sync (working tree → outputs/), sweep (all
> gates green), and tarball (verify rebuild from extracted
> archive), or it doesn't end.

This caught real problems several times:

- A `nodeNameMatches` helper that only existed in the working
  tree but not the outputs copy (the rebuild verification
  surfaced an unused-function warning).
- A README that claimed "61 → 27" when the actual count was 35
  (the sweep on a clean rebuild showed 35).
- A v0.19 sync that missed `resolver_scope.c` because the
  selective-cp list was incomplete (the diff after sync caught
  it).

The full sequence: edit → make → sweep → sync to outputs → diff
outputs vs working tree (must be empty) → tarball → extract
tarball to /tmp → make + sweep on the extracted tree → present.
About four minutes for the full sequence; cheap insurance.

### Mid-debug cleanup

Diagnostic code (e.g. `SML2C_TRACE_MEMBER` env-var shims) is
cheap to add when chasing a bug, and cheap to forget when the
bug is fixed.  Cleanup got formalized as a separate step before
ship — strip the diagnostic before tarball, even if it costs you
the ability to re-diagnose without re-instrumenting.  Two turns
shipped instrumentation by accident before this rule got
internalized; v0.19 explicitly called out "diagnostics scrubbed"
in the changelog.

### Honest measurement

If PTC has 15 errors today, the README says 15.  Not "about 15,"
not "around 15," not "down from 61."  When the count temporarily
drops mid-debug because a buggy fix accidentally suppresses some
errors and reintroduces others, the count after a clean rebuild
is what gets recorded — even if it's higher than the
mid-diagnostic peak.

This stuff is mundane but it adds up.  A README that tracks reality
makes the next turn's planning honest; a README that tracks
aspirations makes it confused.

---

## 12. What's open

PTC default-mode has 15 remaining errors clustered around four
patterns:

1. **Allocation-source dotted paths** (4 errors, lines 817–819)
2. **Send-action source/target ports** (4 errors, lines 829–851)
3. **Driver/vehicle scope refs** (5 errors — state-machine
   triggers across action and state boundaries)
4. **`vehicle_b_1` inherited ports** (2 errors, `:>` (subsets)
   inheritance not surfacing ports)

Each is likely tractable in the same vein as the v0.18/v0.19
fixes — minimal repro, focused fix, regression test, ship.

After PTC closes, the C-codegen extensions that v0.17 was about
to start on:

- **Calc defs to functions.**  `calc def square { in x : Real;
  return : Real = x*x; }` → `double square(double x) { return
  x*x; }`.
- **Specialization to struct embedding.**  `part def Truck :> Car`
  → `typedef struct { Car _base; ... } Truck;`.
- **Default values as initializers.**  Per-def `init` functions
  that assign attribute defaults; also unblocks tuple-literal
  lowering for top-level array statics.

---

## 13. Tarball lineage

Each shipped version is preserved as a tarball under
`/mnt/user-data/outputs/`:

```
sml2c-v0.1.tar.gz   ← first ship after parser/resolver split
sml2c-v0.2.tar.gz   ← constraints, requirements
sml2c-v0.3..v0.6    ← actions, calc defs, function calls
sml2c-v0.7..v0.12   ← parser hardening
sml2c-v0.13.tar.gz  ← visitor framework
sml2c-v0.14.tar.gz  ← typed-AST groundwork
sml2c-v0.15.tar.gz  ← --emit-c first slice
sml2c-v0.16.tar.gz  ← nested struct fields
sml2c-v0.17.tar.gz  ← fixed multiplicity → C arrays
sml2c-v0.18.tar.gz  ← PTC resolver fixes (61 → 35)
sml2c-v0.19.tar.gz  ← more PTC coverage (35 → 15)
sml2c-v0.20.tar.gz  ← refactor + test infra
sml2c-v0.21.tar.gz  ← docs reorganization (README, CHANGELOG, this doc)
```

Each tarball is self-contained and rebuilds clean from an empty
directory — the discipline of verifying that before each ship is
how we know.

---

## 14. Origin

Built collaboratively by Claude (Anthropic) and the project author
across roughly two days of sessions, 2026-04-26 through 2026-04-28.
The teaching framework was Bob Nystrom's *Crafting Interpreters*;
the language was OMG SysML v2 (textual subset).

Code style throughout: clox idioms.  Single contiguous source
buffer, lexemes as `(pointer, length)` pairs, tagged unions for
AST, no garbage collection.  C11.  No external dependencies.

The full transcripts of the sessions live in `/mnt/transcripts/`,
catalogued by `journal.txt`.  This document is the synthesis;
the transcripts are the source.
