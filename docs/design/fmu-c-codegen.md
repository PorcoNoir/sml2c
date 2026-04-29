# `--emit-fmu-c` design: SysML → FMU project tree

Status: design (pre-implementation)
Target versions: v0.25 – v0.28
Author: 2026-04-29

## 1. Goal

Add a second C-emission back-end alongside the existing `--emit-c`.
Where `--emit-c` writes a single self-contained `.c` file (a
"validating library" that runs at startup and reports), `--emit-fmu-c`
produces a complete **FMU project tree** — a directory laid out per
the FMI 3.0 specification's archive structure, ready to build with
CMake into a packaged `.fmu`.

The output of `--emit-fmu-c <file.sysml> --output-dir <DIR>` is
everything an FMI 3.0 importer needs:

- `modelDescription.xml` describing the FMU's variables and structure
- `terminalsAndIcons/TerminalsAndIcons.xml` grouping variables into
  Terminals (FMI 3.0 core feature, no layered standard required)
- `sources/fmu.c` implementing every FMI 3.0 Co-Simulation entry
  point so the FMU can be loaded by any compliant importer
- `sources/model.c` and `include/model.h` carrying the model logic
  lowered from SysML (calc defs, init, check)
- `CMakeLists.txt` that fetches the FMI 3.0 headers from
  `github.com/modelica/fmi-standard` and builds the shared library
- `test/` with a smoke test that loads the binary and exercises the
  FMI lifecycle

## 2. Standards alignment

The output conforms to **FMI 3.0** as published at
<https://fmi-standard.org>.  Specifically:

- Co-Simulation interface (Model Exchange and Scheduled Execution
  are out of scope for v1 — see §10).
- Structured naming convention
  (`variableNamingConvention="structured"`) for hierarchical
  variable names like `eyelet.command`.
- Plain FMI 3.0 `<Terminal>` elements for grouping related
  variables.  Terminals are part of FMI 3.0 core, not a layered
  standard.

The output **does not** claim conformance to any FMI Layered
Standard.  In particular:

- **FMI-LS-STRUCT** v1.0.0-beta.1 is map-shaped — its terminal
  kinds (`…map.rectilinearGrid`, `…map.irregular`) and member
  kinds (`…map.domain`, `…map.codomainOutput`, etc.) describe
  lookup tables, not general structured data.  A general SysML
  `port def` like `HarnessConnectionPoint { out item command;
  in item feedback; }` is not map-shaped, so forcing it into
  LS-STRUCT terminology would either misuse the standard's
  vocabulary or invent kinds that aren't in the spec.  Neither
  serves users.  v1 of `--emit-fmu-c` does not ship the
  LS-STRUCT manifest at all.
- **FMI-LS-BUS** is for network communication (CAN, FlexRay,
  Ethernet, LIN) and is similarly out of scope.

We use custom `terminalKind` and `matchingRule` strings under our
own reverse-domain namespace (`dev.sml2c.port`, etc.) for any
metadata FMI 3.0 core doesn't define.  FMI 3.0 explicitly allows
arbitrary domain-namespaced strings in these slots; importers that
don't recognize them treat the Terminals as plain groupings, which
is the correct fallback.

If/when LS-STRUCT v2 (or a future layered standard) covers general
structured data, we add an opt-in detection layer that emits the
standard's terminal kinds for matching SysML shapes.  That's a
strict addition to v1, not a redesign.

## 3. Running example

The Sleigh ↔ Reindeer model from the SysML v2 spec's Connections,
Interfaces, and Flows tutorial.  Small enough to fit on a screen,
exercises every v0.25-v0.28 lowering:

```sysml
package L13_Connections_Interfaces_Flows {
    part def Sleigh {
        abstract ref part reindeer : Reindeer [9];
        port eyelet : HarnessConnectionPoint;
    }

    part def Reindeer {
        ref part pulling : Sleigh;
        port harnessLoop : ~HarnessConnectionPoint;
    }

    port def HarnessConnectionPoint {
        out item command  : Command;
        in  item feedback : Feedback;
    }

    item def Command;
    item def Feedback;

    /* connection / interface / flow definitions are about composing
     * Sleigh + Reindeer into an assembly.  At the FMU boundary they
     * don't generate code — they describe how two FMUs would wire
     * together in an importer.  See §6.5. */
    interface def HarnessInterface { ... }
    connection def Harness { ... }
}
```

Compiling with `--emit-fmu-c --output-dir build/Sleigh/` against the
outer part def `Sleigh` produces a Sleigh FMU project.  Compiling
against `Reindeer` produces a Reindeer FMU project.  Each is
buildable independently and the two interoperate through their
matching Terminals.

## 4. Output directory layout

Per FMI 3.0 §2.1.6 ("Distribution of an FMU") plus the conventions
established in our reference template at
`/home/claude/fmi-ls-struct-template/` (the FMI3 Project Setup
chat):

```
build/Sleigh/
├── CMakeLists.txt                — FetchContent for fmi3 headers; library target
├── include/
│   └── model.h                   — value references, ModelData struct
├── src/
│   ├── fmu.c                     — full FMI 3.0 entry-point surface
│   ├── model.c                   — calc defs, T_init, T_check, lowered SysML logic
│   └── resources/                — packaged into the .fmu zip's root
│       ├── modelDescription.xml
│       └── terminalsAndIcons/
│           └── TerminalsAndIcons.xml
└── test/
    ├── CMakeLists.txt
    └── test_fmu.c                — Instantiate → Init → DoStep → Terminate smoke
```

The user runs:

```
cmake -S build/Sleigh -B build/Sleigh/_build
cmake --build build/Sleigh/_build
ctest --test-dir build/Sleigh/_build
```

To package as an actual `.fmu` archive (which is just a zip), they
arrange the built `.so`/`.dll` into `binaries/<platform>/Sleigh.so`
alongside the contents of `src/resources/` per FMI 3.0 §2.1.6.
v1 does not generate the zip — that's an importer-side concern and
trivial to scriptify if needed.

## 5. Lowering catalog

Each SysML construct has a specific FMU lowering.  Status: ✓ ready
to implement, ◐ partial, ✗ deferred.

| ID  | SysML construct                          | FMU lowering                                              | Status |
|-----|------------------------------------------|-----------------------------------------------------------|--------|
| F1  | outer `part def T`                       | the FMU model itself: `modelName="T"`, modelIdentifier=T  | ✓      |
| F2  | scalar `attribute x : Real = expr;`      | `<Float64 name="x" causality="parameter" start="…"/>`     | ✓      |
| F3  | scalar `attribute x : Real;` (no default)| `<Float64 name="x" causality="parameter" start="0.0"/>`   | ✓      |
| F4  | `calc def F`                             | `R F(...)` in `model.c`; called from the FMU body         | ✓      |
| F5  | `assert constraint NAME { expr }`        | branch in `T_check`, called from `fmi3ExitInitializationMode` | ✓  |
| F6  | `port p : PortDef` on outer part def     | `<Terminal name="p">` grouping member variables           | ✓      |
| F7  | port-def member `out item x : T;`        | `<Float64 name="p.x" causality="output"/>` (or Binary)    | ✓      |
| F8  | port-def member `in item x : T;`         | `<Float64 name="p.x" causality="input"/>`                 | ✓      |
| F9  | `~PortDef` (conjugate)                   | flip causality on every port-def member                   | ✓      |
| F10 | item def with no attributes              | `<Binary name="…"/>` carrier (presence/opaque payload)    | ✓      |
| F11 | item def with scalar attributes          | one variable per attribute, dot-named                     | ◐      |
| F12 | nested `part def` field on outer         | flatten to dot-named variables grouped under a Terminal   | ◐      |
| F13 | `connection def`, `interface def`, flows | metadata only — Terminal `matchingRule` informs importer  | ✓      |
| F14 | state machines, actions, exhibit state   | deferred; need event-mode integration                     | ✗      |

### F1 – outer part def → FMU model

The user passes `--emit-fmu-c <DIR> --root T` (or sml2c picks the
single top-level part def if there's only one).  T's name becomes:

- `<fmiModelDescription modelName="T">`
- `<CoSimulation modelIdentifier="T">`
- the C library output name (`libT.so` / `T.dll`)
- the prefix on every exported FMI function when
  `FMI3_FUNCTION_PREFIX` is set (per the FMI3 reference template;
  prevents symbol collisions when multiple FMUs link into one
  importer)

The instantiation token is a fresh UUID generated at codegen time
and cached so re-emissions of the same SysML file produce the same
token (deterministic builds).  Cache lives at
`<output-dir>/.sml2c-instantiationToken` — checked in for the
build to be reproducible.

### F2 – F3: scalar attributes

The outer part def's direct attributes become FMI parameters:

```sysml
part def Sleigh {
    attribute massKg : Real = 250.0;
    attribute label  : String = "santa";
}
```

→

```xml
<Float64 name="massKg" valueReference="1" causality="parameter" start="250.0"/>
<String  name="label"  valueReference="2" causality="parameter" start="santa"/>
```

Causality choice:
- attribute with default → `parameter` (tunable)
- attribute without default → `parameter` with start="0"/false/empty
- attribute referenced by `T_check` body but not assigned by `T_init`
  → still `parameter` (constraints read parameters)
- in v0.25 there's no path for `causality="input"` from an
  attribute alone — only port members get input/output causality

Type mapping reuses the runtime header:

| SysML            | FMI 3.0 element |
|------------------|-----------------|
| `Real`           | `<Float64>`     |
| `Integer`        | `<Int64>`       |
| `Boolean`        | `<Boolean>`     |
| `String`         | `<String>`      |
| `Number`         | `<Float64>`     |
| `MassValue` etc. | `<Float64>` (ISQ types are aliases of Real) |

### F4 – F5: calc defs and constraints

Calc defs lower to free C functions in `src/model.c`, exactly as in
`--emit-c`.  No change needed except the file goes into the FMU
project tree instead of stdout.

Constraints lower to `T_check` in `src/model.c`, also exactly as in
`--emit-c`.  The fmi3 entry points call them at well-defined
lifecycle points:

```c
fmi3Status fmi3EnterInitializationMode(...) {
    /* ... */
    return fmi3OK;
}

fmi3Status fmi3ExitInitializationMode(fmi3Instance instance) {
    Component* c = (Component*)instance;
    T_init(&c->data);
    if (!T_check(&c->data)) {
        c->logMessage(c, fmi3Error, "logStatusError",
                      "constraint violation in initial state");
        return fmi3Error;
    }
    return fmi3OK;
}
```

The bool return from `T_check` plus the stderr message we already
emit (v0.24) tells the user *which* constraint failed.  The fmi3
return code tells the importer to halt.

### F6 – F9: ports and Terminals

This is where the SysML-to-FMU lowering gets interesting.

For each port usage on the outer part def, emit:

1. **One FMI variable per port-def member**, named with structured
   notation `<port>.<member>`.  Causality = the member's direction
   (`out item` → `output`, `in item` → `input`).
2. **One `<Terminal>` element** in TerminalsAndIcons.xml grouping
   those variables under the port's name.
3. **For `~PortDef`** (conjugate port type — Reindeer's
   `harnessLoop`), flip every member's causality.  An `out item
   command` in `HarnessConnectionPoint` becomes `causality="input"`
   on the Reindeer side.  This is direct mechanical translation of
   SysML's `~` operator.

For the Sleigh:

```sysml
part def Sleigh {
    port eyelet : HarnessConnectionPoint;   /* not conjugated */
}
port def HarnessConnectionPoint {
    out item command  : Command;
    in  item feedback : Feedback;
}
```

→

```xml
<!-- modelDescription.xml -->
<Float64 name="eyelet.command"  valueReference="1" causality="output"/>
<Float64 name="eyelet.feedback" valueReference="2" causality="input"/>
```

```xml
<!-- TerminalsAndIcons.xml -->
<Terminal name="eyelet"
          terminalKind="dev.sml2c.port"
          matchingRule="dev.sml2c.port.conjugate"
          description="Port of type HarnessConnectionPoint">
  <TerminalMemberVariable variableName="eyelet.command"
                          memberName="command"
                          variableKind="dev.sml2c.port.flow"/>
  <TerminalMemberVariable variableName="eyelet.feedback"
                          memberName="feedback"
                          variableKind="dev.sml2c.port.flow"/>
</Terminal>
```

For the Reindeer (with `~HarnessConnectionPoint`):

```xml
<!-- modelDescription.xml -->
<Float64 name="harnessLoop.command"  valueReference="1" causality="input"/>
<Float64 name="harnessLoop.feedback" valueReference="2" causality="output"/>
```

```xml
<!-- TerminalsAndIcons.xml — same structure, flipped causality -->
<Terminal name="harnessLoop"
          terminalKind="dev.sml2c.port"
          matchingRule="dev.sml2c.port.conjugate" ...>
  <!-- members same as Sleigh's; only causality on the variables flipped -->
</Terminal>
```

The `dev.sml2c.port.conjugate` matching rule encodes "two ports
match if their member names line up and their causalities are
mirror images."  Importers that recognize this rule (an importer
we ship later, or a hand-written tool) can auto-wire Sleigh.eyelet
to Reindeer.harnessLoop.  Importers that don't recognize it just
see the Terminals as named groupings.

### F7 – F10 detail: items as variable types

A SysML `item def Foo;` describes a kind of thing that flows
through a port.  In FMI terms that's a *value* the FMU receives
or produces.  Two cases:

- **Item with no attributes** (the Sleigh-Reindeer case:
  `item def Command;`) carries no scalar payload.  We lower it as
  `<Binary>` — an opaque byte sequence.  The Sleigh writes a
  trigger; the Reindeer reads it.  The FMI Binary type is the
  simplest carrier when we don't know what's inside.
- **Item with scalar attributes** (e.g. `item def Telemetry {
  attribute rpm : Real; attribute oilPressure : Real; }`)
  flattens: each attribute becomes its own FMI variable, named
  `<port>.<member>.<attr>`, with the type mapping from F2.  All
  variables for one item belong to the same Terminal.

v0.25 supports the no-attributes case (F10).  v0.27 adds attribute
flattening for items (F11).  Items with item-typed attributes
(structured items) are deferred — that's F12 territory.

### F13: connections, interfaces, flows

These are SysML's *system-level* composition primitives.  They
describe how two part defs (Sleigh and Reindeer) wire together
when both exist in an assembly.  At the FMU boundary, they don't
become code — each FMU is independent.

What they *do* generate is metadata in the Terminal's
`matchingRule`.  When the SysML model says

```sysml
interface def HarnessInterface {
    end port eyelet : HarnessConnectionPoint;
    end port harnessLoop : ~HarnessConnectionPoint;
    flow of Command from eyelet.command to harnessLoop.command;
    flow of Feedback from harnessLoop.feedback to eyelet.feedback;
}
```

the codegen sees that `eyelet` and `harnessLoop` are conjugate ports
participating in a flow-bearing interface.  Both Terminals get
`matchingRule="dev.sml2c.port.conjugate"`.  An importer that
loads both FMUs and recognizes this rule can present "wire eyelet
to harnessLoop" as a one-click action.

In v0.25 we emit only the Terminal grouping and the matching rule
string.  Recognizing the rule is an importer-side concern — out of
scope for the codegen.

## 6. Runtime model — explicit decisions

These are the design questions settled here so v0.25-v0.28 don't
re-litigate.

**1. Which FMI mode?**  Co-Simulation only in v1.  Model Exchange
needs derivative state and event indicators we don't have a SysML
mapping for yet.  Scheduled Execution is more involved.  Both can
land later as orthogonal additions.

**2. Variable naming?**  Structured (`variableNamingConvention="structured"`).
Port members are `<port>.<member>`; nested struct fields in items
are `<port>.<member>.<field>`.  Tools render them hierarchically.

**3. Value reference assignment?**  Sequential starting at 1, in
source-declaration order across the whole model.  Cached in
`<output-dir>/.sml2c-valuerefs` so re-runs assign the same VRs.
This matters because an importer keeping pointers to specific VRs
between FMU reloads needs them to be stable.

**4. Instantiation token?**  Generated as a UUIDv4 at first run,
cached with the value references.  Stable across re-runs; new
FMUs get new tokens.

**5. What happens on `fmi3DoStep`?**  v0.25 (foundation) does
nothing — the FMU is a stateless calculator at this point.  Once
v0.26 adds calc defs and constraints into the lifecycle, DoStep
runs T_check and returns Discard if any constraint now fails (the
FMU has detected an inconsistency mid-simulation).  Once state
machines land (post-v0.30), DoStep advances the state machine.

**6. Logging?**  Every fmi3 entry point checks the
`logger`/`logEvent` callback the importer passed in.  Errors
(constraint violations, invalid value-reference accesses) log
through the standard category strings (`logStatusError`,
`logStatusWarning`).  `stderr` from the existing `T_check`
becomes `logger(c, fmi3Error, ...)`.

**7. Memory?**  Each FMU instance is a heap-allocated `Component`
struct holding the `ModelData` plus FMI bookkeeping (state
pointers, allocator/logger callbacks, name).  `fmi3Instantiate`
allocates; `fmi3FreeInstance` frees.

**8. Threading?**  None.  The FMI 3.0 spec doesn't require thread
safety from the FMU; importers serialize calls per-instance.

**9. Build system?**  CMake.  The reference template fetches FMI
3.0 headers via `FetchContent_Declare` against
`github.com/modelica/fmi-standard` pinned to a specific tag
(currently `v3.0.2`).  No external dependencies on the SysML
side.  Output is a shared library that links to nothing beyond
libc.

**10. What if a SysML construct doesn't lower?**  Skip with a
`/* skipped: NAME (reason) */` line in the affected file (same
discipline as `--emit-c`) and emit a one-line warning to stderr
during codegen.  The FMU still builds — it just doesn't expose
that piece.

## 7. CLI shape

```
sml2c --emit-fmu-c <file.sysml> --output-dir <DIR> [--root <PartDef>]
```

- `--output-dir <DIR>` is required.  Fails loudly if missing.
- `<DIR>` must not exist or must be empty (don't clobber a user's
  existing tree silently; they can `rm -rf` if they want).
- `--root <PartDef>` picks which top-level `part def` becomes the
  FMU.  Optional if there's exactly one; required if multiple.
- Otherwise, `--emit-fmu-c` accepts every existing flag
  (`--no-typecheck` etc.) for layer isolation while debugging.

Composes naturally with `--emit-c`: a user can compile a SysML
file two ways depending on what they want.  Neither emitter
modifies the other.

## 8. Reuse from `--emit-c`

The existing codegen has machinery the FMU emitter wants
verbatim:

- **Type mapping** (`cFieldTypeFor` etc.) — Real→`Real`,
  Integer→`Integer`, etc.  Used in both modelDescription.xml
  type tag selection and in C struct fields.
- **Calc def lowering** — same C function, just lives in
  `src/model.c` instead of stdout.
- **`T_init` topo-sorted assignments** — same.  Called from
  `fmi3EnterInitializationMode`.
- **`T_check` constraint predicates** — same.  Called from
  `fmi3ExitInitializationMode`.
- **`emitExpr` and `self->` rewriter** — same.
- **Runtime header** (`runtime/sml2c-runtime.h`) — same.  The FMU's
  `model.c` includes it just like the standalone emitter does.

What's *new* in `--emit-fmu-c`:

- modelDescription.xml emitter (~120 lines)
- TerminalsAndIcons.xml emitter (~80 lines)
- fmu.c boilerplate emitter (~250 lines — most of it forwarding
  to model.c functions)
- CMakeLists.txt emitter (~50 lines, mostly stable boilerplate)
- Output-directory writer (filesystem mkdir/open/write — ~50 lines)

So the new code is around 600 lines of emitter logic plus the
~250 lines of boilerplate strings (CMakeLists template, fmu.c
template).  No changes to existing files except the main.c flag
parser.

## 9. Implementation roadmap

Each version is independently shippable with all sweep gates
green plus a new gate that builds the FMU output via cmake +
runs ctest on the smoke test.

### v0.25 — FMU foundation

- New `src/codegen_fmu.c` and header
- `--emit-fmu-c` flag in main.c with `--output-dir` validation
- Output: directory tree with CMakeLists.txt, include/model.h,
  src/fmu.c (full FMI 3.0 surface, all stubs), src/model.c
  (just the struct + a no-op T_init), src/resources/modelDescription.xml
  (just the outer part def's scalar attributes), and
  src/resources/terminalsAndIcons/TerminalsAndIcons.xml (empty
  Terminals element)
- New `make test-fmu-c` gate: for each test/*.fmu.sysml, run
  `sml2c --emit-fmu-c` into a temp dir, then cmake build, then
  ctest.  Failures fail the gate.
- Test fixture: `test/FmuFoundation.sysml` — one part def with a
  handful of scalar attributes, no ports.  Exit criterion: cmake
  build succeeds, ctest's smoke test loads the FMU and reads back
  the parameter values.

**Estimated:** ~400 lines new code, 1 new test fixture, 1 new gate.

### v0.26 — Calc defs and constraints in the FMU lifecycle

- Calc def emission into `src/model.c` (reuse existing logic)
- T_init in `fmi3EnterInitializationMode`
- T_check in `fmi3ExitInitializationMode`
- Constraint failures route through fmi3 logger callback (not
  stderr) and return fmi3Error
- Test fixture: `test/FmuConstraints.sysml` — part def with a
  calc, attributes with default values via the calc, and a
  constraint that the test driver violates after init to check
  that fmi3SetReal + the next DoStep returns fmi3Discard.

**Estimated:** ~150 lines new code, mostly templating the
existing calc/init/check emission into the model.c output.

### v0.27 — Ports and Terminals

- Port usages on outer part def → FMI variables with structured
  names + causality from member direction (out/in)
- Conjugate ports (`~PortDef`) flip every causality
- TerminalsAndIcons.xml gets one `<Terminal>` per port
- Items with no attributes lower as `<Binary>`
- Items with scalar attributes flatten to per-attribute FMI
  variables under the same Terminal
- Test fixture: `test/Sleigh.fmu.sysml` — the running example.
  Build the Sleigh FMU and the Reindeer FMU; ctest verifies that
  Sleigh exposes `eyelet.command` as output and `eyelet.feedback`
  as input, and that Reindeer is the conjugate.

**Estimated:** ~300 lines new code, 2 test fixtures.

### v0.28 — connection/interface awareness

- Connections and interfaces in the SysML source generate the
  matching rule attribute on the relevant Terminals
- Terminal-level annotations describe which interface a port
  participates in
- Test fixture extends Sleigh.fmu.sysml's importer-side smoke
  test to verify the matching-rule string is present and
  correctly identifies the conjugate pair.

**Estimated:** ~100 lines new code.

After v0.28: ports and items with general structure (F11, F12) +
ME/SE modes + state machines integrated as discrete events.  Each
of those is a follow-on design pass.

## 10. Out of scope for v1 (v0.25-v0.28)

- **Model Exchange and Scheduled Execution modes.**  Co-Simulation
  only.
- **Continuous-time state derivatives.**  No mapping from SysML
  to ME's derivative-of-state model yet.
- **Clocks.**  FMI 3.0's clock mechanism for periodic activations
  doesn't have a SysML equivalent in our subset.
- **State machines as discrete events.**  v0.25-v0.28 is about
  the structural lowering.  Once it's in place, designing how
  state machines map to fmi3UpdateDiscreteStates / event mode
  is a self-contained next pass.
- **Connections at runtime.**  We emit the matching-rule metadata
  for importers; we don't generate code that wires two FMUs
  together at runtime (that's what an importer does).
- **The .fmu zip itself.**  We emit the directory tree.  Zipping
  it into an actual `.fmu` archive is one shell command (`cd
  build/Sleigh && zip -r Sleigh.fmu modelDescription.xml
  terminalsAndIcons binaries`) and not worth automating in v1.
- **LS-STRUCT, LS-BUS, LS-XCP, any layered standard.**  Plain
  FMI 3.0 only.  Layered-standard support is opt-in additions
  later.

## 11. Composition with existing tracks

State machines and actions were originally v0.25 in
`design/c-codegen.md`.  The FMU emitter takes the v0.25-v0.28
slots; state machines defer to v0.30+.  The argument is that
state machines have meaningfully different lowerings in each
target — `--emit-c` wants a step function and a hardcoded
event sequence in main(), while `--emit-fmu-c` wants
fmi3UpdateDiscreteStates and event mode integration.
Designing both in one pass is cleaner than retrofitting one
to fit the other later.

The two emitters also unblock different use cases:

- `--emit-c`: small validating-library demos; "does my SysML
  model produce the values I expect for these inputs?"
- `--emit-fmu-c`: integration into existing FMI-aware
  simulation environments (Modelon, Dymola, OpenModelica,
  4DIAC, dSPACE, etc.); "ship my SysML model as an industry-
  standard component."

Both stay supported.  Neither replaces the other.

## 12. Decision log

(empty — to be populated as v0.25+ ships)
