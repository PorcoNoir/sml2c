# CHANGELOG

Per-version notes for sml2c.  Latest first.

The format follows roughly: what shipped, why it mattered, file-level
effect.  Earlier versions (v0.1–v0.12) are reconstructed from journal
summaries — the detail is lighter than for v0.13 onward, where I have
the working tree at the time of release.

---

## v0.25 — `--emit-fmu-c` foundation: FMI 3.0 project tree

First step of a new emission track that lives alongside the existing
`--emit-c`.  Where `--emit-c` writes a single self-contained C file
to a stream, `--emit-fmu-c` produces a complete FMU project tree on
disk — directory layout per the FMI 3.0 archive specification, ready
to build with cmake into a packaged `.fmu`.

```
$ ./bin/sml2c --emit-fmu-c test/FmuFoundation.fmu.sysml \
              --output-dir build/Foundation
$ cmake -S build/Foundation -B build/Foundation/_build
$ cmake --build build/Foundation/_build
$ ctest --test-dir build/Foundation/_build --output-on-failure
1/1 Test #1: smoke ......................... Passed
```

The emitter writes 11 files: top-level `CMakeLists.txt`, `include/`
with `model.h` plus the three vendored `fmi3*.h` headers, `src/fmu.c`
+ `src/model.c`, `src/resources/modelDescription.xml`, and a `test/`
subtree with its own CMakeLists + `test_fmu.c` smoke test.

**Standards stance: Path A.**  Per the design doc decision in this
turn's prior chat, the output is plain FMI 3.0 — no FMI-LS-STRUCT,
no FMI-LS-BUS, no layered standards.  LS-STRUCT v1.0.0-beta.1 is
narrowly map-shaped and doesn't fit general SysML port shapes;
forcing it into LS-STRUCT terminology would either misuse the
standard or invent kinds that aren't in the spec.  When LS-STRUCT
or another layered standard grows general structured-data support,
opt-in detection lands as a strict addition.

**Vendored FMI 3.0.2.**  `runtime/fmi3/headers/` carries the three
official FMI 3.0.2 C headers (fmi3PlatformTypes, fmi3FunctionTypes,
fmi3Functions); `runtime/fmi3/schema/` carries 12 official XSD
schemas; `runtime/fmi3/LICENSE.txt` is the BSD-2-Clause from
modelica/fmi-standard.  Vendored, not fetched at build time, so
generated FMU projects build offline and reproducibly.  Upgrading
is a `git clone --branch <new-tag>` + copy.

**v0.25 lowering scope** (per design/fmu-c-codegen.md §5):

| Construct                              | v0.25 status                |
|----------------------------------------|-----------------------------|
| Outer part def                         | ✓ becomes the FMU model     |
| Scalar attribute (Real / Integer / Boolean / String) | ✓ FMI parameter variable |
| Calc def                               | skip — v0.26                |
| Constraint                             | skip — v0.26                |
| Port usage                             | skip — v0.27                |
| Conjugate port (`~PortDef`)            | skip — v0.27                |
| Item def with attributes               | skip — v0.27                |
| Connections / interfaces / flows       | skip — v0.28                |
| State machines, actions                | skip — v0.30+               |

Each scalar attribute becomes one FMI variable with structured-naming
convention, `causality="parameter"`, `variability="tunable"`, and the
correct type tag per the SysML kernel: Real → `<Float64>`, Integer →
`<Int64>`, Boolean → `<Boolean>`, String → `<String>` (with the
required child `<Start value="..."/>` element per fmi3Variable.xsd
rather than the `start=` attribute used by numeric types).

**fmu.c surface.**  All 74 FMI 3.0 entry points are present so the
shared library is usable by any conformant importer.  Float64,
Int64, Boolean, and String getters/setters are routed through
ModelData by value reference using switch statements.  Float32,
Int8/16/32, UInt8/16/32/64, Binary, Clock, FMU-state save/restore,
discrete-state evaluation, continuous-state derivatives, event
indicators, intervals, and shifts all return `fmi3Error` via
`UNSUPPORTED_*` macros.  Co-Simulation lifecycle (Instantiate /
EnterInit / ExitInit / DoStep / Reset / Terminate / FreeInstance)
is fully wired; `fmi3DoStep` is a no-op at v0.25 (model is
stateless until v0.26 adds calc/check) and returns `fmi3OK`.

**Symbol prefixing.**  `FMI3_FUNCTION_PREFIX=<ModelName>_` is set
via the CMake target's compile definitions, so an FMU's exported
symbols are e.g. `Foundation_fmi3InstantiateCoSimulation` —
prevents collisions when an importer loads multiple FMUs into a
single process.

**Deterministic instantiation token.**  Generated at codegen time
via FNV-mixed bytes of the model name.  Same model → same token,
which lets re-emissions produce reproducible FMUs.

**New build gate `make test-fmu-c` (optional).**  For each
`test/*.fmu.sysml` fixture: `--emit-fmu-c` into a temp dir →
`xmllint --schema fmi3ModelDescription.xsd` validates the XML
output against the official FMI 3.0 schema → `cmake -S . -B _build`
configures → `cmake --build _build` builds the .so → `ctest`
runs the smoke test.  Skips silently with a one-line notice if
`cmake` isn't installed.  Wired into `make sweep`.

**Test fixture.**  `test/FmuFoundation.fmu.sysml` declares one
part def with one attribute of each kernel scalar type:

```sysml
package FmuFoundation {
    private import ScalarValues::*;
    part def Foundation {
        attribute mass     : Real    = 200.0;
        attribute capacity : Integer = 7;
        attribute label    : String  = "santa";
        attribute enabled  : Boolean = true;
    }
}
```

The generated `test/test_fmu.c` smoke test asserts:
1. `fmi3InstantiateCoSimulation` returns non-NULL
2. `fmi3EnterInitializationMode` + `fmi3ExitInitializationMode` succeed
3. `fmi3GetFloat64`/`Int64`/`String`/`Boolean` return the source-code defaults
4. `fmi3DoStep` returns `fmi3OK`
5. `fmi3Terminate` + `fmi3FreeInstance` cleanly tear down

Output:
```
mass = 200
capacity = 7
label = santa
enabled = 1
```

**Decisions made this turn (per user input):**

- **Optional `make test-fmu-c` gate** — skip silently if cmake
  missing.  Honest signal (we don't pretend to test what we can't),
  zero friction for users without cmake installed.
- **Vendored headers, not FetchContent** — full reproducibility,
  no network dependency at build time.
- **One FMU per SysML file** — pick the outermost part def.
  Multiple top-level part defs require explicit `--root NAME`.
  Inner part defs and parts are deferred handling (will become
  separate `.h` struct typedefs in a later version).
- **Conjugate detection** via `qualifiedName.isConjugated`
  (confirmed correct field; deferred to v0.27).

**Bugs caught & fixed during the turn:**
- `DEF_PACKAGE` doesn't exist — packages are their own NodeKind
  (`NODE_PACKAGE`), not a definition kind.
- `<String>` uses a child `<Start value="..."/>` element, not the
  `start=` attribute the numeric variable types accept.
- Boolean defaults need `fmi3True`/`fmi3False` macros from
  fmi3PlatformTypes.h, not bare `true`/`false`.
- The UUID-shaped instantiation token's last group needs a
  12-hex-digit mask (`0xffffffffffffuL`); my initial mask had 13.
- `<Terminals>` requires at least one `<Terminal>` child per the
  XSD, so v0.25 omits TerminalsAndIcons.xml entirely (FMI 3.0
  allows the file to be absent — it's optional metadata).
  v0.27 starts populating it from port usages.

**Verification.**  All sweep gates green: 76 strict, 49 graphsml,
49 test-c, 4 test-c-run (CalcEmit / InitEmit / Thermostat /
ConstraintEmit), **1 test-fmu-c (new — FmuFoundation)**, 132
verify-tokens.  PTC parser=0 / default=15.

**Files touched:**

- New: `runtime/fmi3/{headers/*.h,schema/*.xsd,LICENSE.txt,README.md}`,
  `include/codegen_fmu.h`, `src/codegen_fmu.c` (~900 lines),
  `test/FmuFoundation.fmu.sysml`.
- Modified: `main.c` (three new flags), `Makefile` (test-fmu-c
  target, sweep wiring).
- No changes to the existing `--emit-c` track.

**What's next (v0.26):** route calc defs and constraints through
the FMU lifecycle.  Calc def bodies into `src/model.c`, T_init
called from `fmi3EnterInitializationMode`, T_check called from
`fmi3ExitInitializationMode` (constraint failure → fmi3Error +
logger callback message).  Reuses the entire v0.22-v0.24 lowering
machinery from `--emit-c`; the new code is just the lifecycle
dispatch and the logger plumbing.

---

## v0.24 — Inline constraints to T_check predicates

Third step of the executable-target pivot.  Every part def with at
least one inline `assert constraint` member gets a generated
`Boolean T_check(const T* self)` that returns true iff every
constraint holds.  Each constraint contributes one early-return
branch with a stderr message naming the failure.

```sysml
part def Engine {
    attribute mass    : Real = 200.0;
    attribute torque  : Real = 350.0;

    assert constraint sane_mass {
        mass > 0.0 and mass < 10000.0
    }
    assert constraint {
        torque > 0.0
    }
}
```

emits

```c
Boolean Engine_check(const Engine* self) {
    if (!(((self->mass > 0.0) && (self->mass < 10000.0)))) {
        fprintf(stderr, "Engine.sane_mass failed: mass > 0.0 and mass < 10000.0\n");
        return false;
    }
    if (!((self->torque > 0.0))) {
        fprintf(stderr, "Engine constraint failed: torque > 0.0\n");
        return false;
    }
    return true;
}
```

**Failure message format** (per design doc Q5).  Named constraints:
`<TypeName>.<name> failed: <source-expr>`.  Anonymous: `<TypeName>
constraint failed: <source-expr>`.  The source expression is
recovered from the AST via a new `emitExprAsCString` helper that
mirrors `emitExpr` but writes SysML's surface syntax (`and` not
`&&`) and escapes for safe embedding in a `"..."` literal.

**Self-pointer rewriter reused.**  Inside `T_check`, bare-name
references to sibling attributes get the same `self->name`
substitution that `T_init` uses, via the shared `currentInitDef`
field on the codegen context.  No new infrastructure needed.

**Definition emittability extended.**  Until v0.24,
`definitionEmittable` rejected any def with a non-attribute,
non-(part/item-usage) member — including constraint usages.  A part
def with constraints would skip whole-cloth as "not C-emittable."
v0.24 accepts constraint usages as a third valid member kind: they
don't contribute to the struct shape but do contribute to T_check.
The struct-field emitter learned to skip them.

**Runtime header gains `<stdio.h>`.**  `T_check`'s failure messages
need `fprintf` and `stderr`.  Added to `runtime/sml2c-runtime.h`
alongside the existing `<stdbool.h>` / `<stdint.h>`.

**Verification.**  All sweep gates green: 76 strict, 49 graphsml,
49 test-c, **4 test-c-run** (CalcEmit + InitEmit + Thermostat +
new ConstraintEmit), 132 verify-tokens.  PTC parser=0 / default=15.

The new `ConstraintEmit` test exercises:
- A part def with three constraints (two named, one anonymous)
- A second part def with one constraint (cross-def isolation)
- Driver triggers each violation in turn, captures stderr + bool
  return through the existing `2>&1` merge in `make test-c-run`
- Golden file pins the exact failure messages so regressions show
  up as a diff

```
Engine defaults: pass
Battery defaults: pass
Engine.sane_mass failed: mass > 0.0 and mass < 10000.0
Engine after mass=-5: fail
…
```

**Thermostat status update.**  `Controller` (in `test/Thermostat.sysml`)
still skips because of its `exhibit state mode : Mode;` member.
v0.25 will lift state machines and the `exhibit state` field to a
struct member; at that point Controller will lower fully and we
get the design doc's running example all the way to a runnable
program.

**Files touched:** `src/codegen_c.c` (~+150 lines: T_check helpers,
source-text recovery, constraint-aware emittability,
struct-field skip);  `runtime/sml2c-runtime.h` (added stdio.h);
new `test/ConstraintEmit.sysml`, `test/ConstraintEmit.driver.c`,
`test/expected/ConstraintEmit.expect`.

---

## v0.23.1 — Thermostat as a real test fixture

User reported a parse error trying to compile the thermostat example
from `design/c-codegen.md` §2:

```
$ ./bin/sml2c --emit-c ./test/Thermostat.sysml
[line 8] Error at 'out': Expected ';' after return.
Parse failed.
```

Root cause: the example's calc def used `return out : Real = ...`,
but `out` is a direction-modifier keyword (TOKEN_OUT).  The parser's
return-feature-name production at `parser_decl.c:1856` only accepts
TOKEN_IDENTIFIER, so `out` falls through and the parser later
reports the missing semicolon.

Two fixes shipped:

- **Design doc**: renamed `out` → `power` in §2.  Power is the
  semantically right name anyway (the calc returns the power output
  computed from the temperature error).
- **Test corpus**: shipped `test/Thermostat.sysml` (and a small
  driver + golden file) so the running example is now exercised by
  every `make sweep`.  Future regressions are caught at CI.

Status of the thermostat in v0.23.1:

```
$ ./bin/sml2c --emit-c test/Thermostat.sysml
/* Generated by sml2c — do not edit by hand. */
#include "sml2c-runtime.h"
/* package Thermostat */

Real ResponsePower(Real error, Real gain);
Real ResponsePower(Real error, Real gain) {
    return (error * gain);
}
/* skipped: Mode (not C-emittable) */
/* skipped: TooCold (not C-emittable) */
/* skipped: TooHot (not C-emittable) */
/* skipped: InRange (not C-emittable) */
/* skipped: Controller (not C-emittable) */
```

Driver output (also covered by `test-c-run`):

```
ResponsePower(2, 100) = 200
ResponsePower(-1, 50) = -50
ResponsePower(0, 100) = 0
```

`Controller` skips because it has inline constraints (lift to
`T_check` in v0.24) and an `exhibit state` (lift to a state field
in v0.25).  `Mode` skips because state machines aren't lowered yet.
The skip behavior is the right behavior for the structural lowering
already shipped — no partial output, just clean comments showing
what's deferred.

`test-c-run` count: 3 (CalcEmit + InitEmit + Thermostat).

**Files touched:** `design/c-codegen.md` (§2 example renamed; v0.23.1
decision-log entry); new `test/Thermostat.sysml`,
`test/Thermostat.driver.c`, `test/expected/Thermostat.expect`.

**Not fixed (deferred):** the parser-level "direction keywords as
feature names" issue.  The SysML spec arguably allows this; our
parser doesn't.  None of the project's other test or reference
files trip it.  Would be a small fix in `parser_decl.c`
(accept TOKEN_IN/TOKEN_OUT/TOKEN_INOUT alongside TOKEN_IDENTIFIER
in the return-feature-name production), but introduces ambiguity
risk and isn't blocking anything.

---

## v0.23 — Init functions (T_init + __sml2c_init)

Second step of the executable-target pivot.  Defaults declared in
SysML now actually take effect — every part def with at least one
defaulted attribute gets a generated `T_init(T*)` function, and
top-level non-const attributes get hoisted into a generated
`__sml2c_init(void)`.

```sysml
part def Engine {
    attribute mass    : Real = 200.0;
    attribute torque  : Real = 350.0;
    attribute redline : Real = 7000.0;
    attribute massPlusTorque : Real = mass + torque;
    attribute headroom       : Real = redline - massPlusTorque;
}

attribute referenceSquare : Real = Square(7.0);     /* L2 — non-const */
```

emits

```c
void Engine_init(Engine* self) {
    self->mass = 200.0;
    self->torque = 350.0;
    self->redline = 7000.0;
    self->massPlusTorque = (self->mass + self->torque);
    self->headroom = (self->redline - self->massPlusTorque);
}

Real referenceSquare;     /* not static-const — initialized in __sml2c_init */

void __sml2c_init(void) {
    referenceSquare = Square(7.0);
}
```

**Bare-name rewriter.**  Inside `T_init`, any single-segment qname
that resolves to a sibling attribute of `T` rewrites to
`self->name`.  Scoped: only fires while `currentInitDef` is set in
the codegen context (i.e. during `T_init` body emission).  Calc-def
bodies don't get the rewrite — their parameters are local C
identifiers, not struct fields.

**Topological sort.**  Both `T_init` and `__sml2c_init` order their
assignments by dependency rather than source order.  An attribute X
depends on Y iff X's default expression contains a single-segment
qname resolving to Y.  DFS-with-three-colors detects cycles and
emits a codegen-error comment in place of the affected init body
rather than silently producing wrong code.

```
Engine_init in source order would assign massPlusTorque before
headroom (which depends on it), but if the user wrote them in the
opposite order, source-order would silently read uninitialized
memory.  Topo sort makes the order safe regardless of how the
user listed them.
```

**Cycle detection.**  When `attribute a = b + 1; attribute b = a - 1;`
appears in a part def, the generated init function comes out as

```c
void Bad_init(Bad* self) {
    /* codegen error: cyclic init dependency in Bad_init — skipped */
}
```

The C compiler accepts the empty body; the user sees the cycle
explicitly rather than getting a confusing runtime read of
uninitialized memory.

**Nested struct initialization.**  When a part def has a nested
part-def field whose type itself needs `T_init`, the outer init
chains:

```sysml
part def Vehicle {
    part wheels : Wheel [4];   /* Wheel has its own T_init */
}
```

→

```c
void Vehicle_init(Vehicle* self) {
    for (long _i = 0; _i < 4; _i++) {
        Wheel_init(&self->wheels[_i]);
    }
    /* … any Vehicle-level scalar defaults … */
}
```

The chain runs first (so nested values are settled before any
sibling reference reads from them).  Cross-level dependencies
(`attribute total = engine.power`) aren't supported in v0.23 — the
member-access lowering is still future work.

**Verification.**  All sweep gates green: 73 strict, 46 graphsml,
46 test-c, **2 test-c-run** (CalcEmit + new InitEmit), 132 verify-
tokens.  PTC parser=0 / default=15.

**Files touched:** `src/codegen_c.c` (~+250 lines: topo sort,
T_init emission, __sml2c_init emission, isMemberOf helper, self->
rewriter in emitQNameC, L2 hoisting in onAttribute);
`CHANGELOG.md`, `README.md`; new `test/InitEmit.sysml`,
`test/InitEmit.driver.c`, `test/expected/InitEmit.expect`.

---

## v0.22.1 — Runtime header for kernel + ISQ types

Refinement to v0.22 prompted by user clarification.  Until now,
`cFieldTypeFor` returned C-primitive names (`"double"`, `"long long"`,
`"bool"`, `"const char*"`) inline at the codegen level — the SysML
type names (`Real`, `Integer`, `Boolean`, `String`, `Number`)
dissolved at emit time and the generated C had no trace of the
kernel data type library.  ISQ engineering quantities
(`MassValue`, `TorqueValue`, …) didn't lower at all — any attribute
typed with one got skipped because `cFieldTypeFor` returned NULL.

v0.22.1 lifts the type catalog into a project-shipped runtime
header (`runtime/sml2c-runtime.h`):

```c
/* sml2c-runtime.h excerpt */
typedef double      Real;
typedef long long   Integer;
typedef bool        Boolean;
typedef const char* String;
typedef double      Number;

typedef Real LengthValue;
typedef Real MassValue;
typedef Real TorqueValue;
typedef Real ForceValue;
/* … */
```

Generated `.c` files now `#include "sml2c-runtime.h"` and refer to
SysML's type names directly:

```c
/* before v0.22.1 */                /* v0.22.1 onward */
typedef struct {                    typedef struct {
    double mass;                        Real mass;
    long long capacity;                 Integer capacity;
    const char* name;                   String name;
    bool electric;                      Boolean electric;
} Vehicle;                          } Vehicle;
```

The C compiler still treats them identically (typedef is purely a
naming layer), but the generated source preserves SysML's intent
and is much more self-documenting.

**ISQ.**  Engineering quantity types are listed in the header as
typedefs of `Real` — about 20 of them, covering the most common
quantities (length, mass, torque, force, energy, power, speed,
frequency, etc.).  Names match the SysML standard library.  This
positions us correctly for ISQ resolution work later: when
`attribute mass : ISQ::MassValue;` resolves at the typechecker
level, the codegen will already know what to lower it as.

**Trade-off explicit:** typedefs of `double` are interchangeable
at the C level, so dimensional errors (`Mass m = some_force_value;`)
won't be caught.  The header preserves *intent* without claiming
dimensional safety.  Future work could lift the typedefs into
tagged structs (`typedef struct { Real value; } MassValue;`) for
real dimensional safety, with that change confined to the header.

**Build change.**  `make test-c` and `make test-c-run` pass
`-I runtime` so cc finds the header.

**Files touched:** new `runtime/sml2c-runtime.h`; modified
`src/codegen_c.c` (cFieldTypeFor returns SysML names; emit
`#include "sml2c-runtime.h"` instead of stdbool/stdint), `Makefile`
(add `-I runtime`), `test/CalcEmit.driver.c` (uses Real now to
demonstrate interop), `design/c-codegen.md` (new §4 documenting
the runtime header; thermostat example updated).

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
