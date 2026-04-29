# sml2c — a SysML v2 compiler in C

A compiler for a textual subset of OMG SysML v2, written in hand-rolled
C following the style of *Crafting Interpreters* (clox).  Single-file
source input, validator-first design, multiple back-ends.

For per-version notes see [CHANGELOG.md](CHANGELOG.md).
For a long-form narrative of how the project got here, see
[chat_history.md](chat_history.md).

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
        └────────────► back-end (AST print | JSON | SysML | C)
```

Every pass after the parser is independently bypassable via a `--no-*`
flag, so it's straightforward to inspect intermediate state when
debugging.

## Build and run

```
make                 # build bin/sml2c
make sweep           # full validation (verify-tokens, test-all,
                     # test-c, test-graphsml, test-ptc)

bin/sml2c file.sysml                # full pipeline, prints AST tree
bin/sml2c --tokens file.sysml       # token stream
bin/sml2c --emit-json file.sysml    # AST as JSON
bin/sml2c --emit-sysml file.sysml   # canonical SysML (round-trip)
bin/sml2c --emit-c file.sysml       # C header (typed-AST first slice)
bin/sml2c --no-typecheck file.sysml # skip the typechecker (any --no-*
                                    # flag isolates a layer for debug)
```

C11, no external dependencies.  Tested with both `gcc` and `clang`.

## Architecture

Validator-first, multi-backend.  The AST itself is the IR; passes
mutate or annotate it but don't rewrite shape.  This keeps `--emit-sysml`
round-trippable (the printer can recover what the user wrote) while
still letting later passes attach inferred information for the codegen
back-ends to read.

### Directory layout

```
.
├── main.c                    program entry point + flag parsing
├── include/
│   ├── ast.h                 Node tagged-union; NodeList; QualifiedName
│   ├── ast_walk.h            visitor framework (used by every pass)
│   ├── codegen_*.h           per-back-end interface
│   ├── parser_internal.h     parser/scanner shared state
│   ├── resolver.h            public resolver entry
│   ├── resolver_internal.h   scope/lookup primitives, used by passes
│   └── scanner.h             TOKEN_* enum, scanner state
├── src/
│   ├── scanner.c             single-pass lexer over a contiguous buffer
│   ├── parser_common.c       token machinery, qualifiedName helpers
│   ├── parser_decl.c         declaration grammar (the bulk)
│   ├── parser_expr.c         Pratt parser for expressions
│   ├── ast.c                 Node arena; print/free; visitor dispatch
│   ├── ast_walk.c            visitor framework body
│   ├── builtin.c             synthetic stdlib (ScalarValues etc.)
│   ├── resolver.c            walk + resolveQualifiedName
│   ├── resolver_scope.c      Scope/Symbol primitives + lookups
│   ├── typechecker.c         bottom-up type inference
│   ├── redefchecker.c        :>> validation against supertype chains
│   ├── connectchecker.c      port direction / conjugation parity
│   ├── referentialchecker.c  §8.3 reference-inference rules
│   ├── codegen_json.c        --emit-json
│   ├── codegen_sysml.c       --emit-sysml (round-trip)
│   └── codegen_c.c           --emit-c (typed-AST first slice)
├── test/
│   ├── *.sysml               positive tests (every file must parse and
│   │                         pass the full pipeline)
│   └── bad/*.sysml           negative tests (every file must be
│                             rejected somewhere in the pipeline)
├── tools/
│   └── sml2c_to_graphsml.py  drawio adapter over --emit-json output
├── verify-tokens.sh          parser ↔ scanner token-symbol drift check
├── Makefile                  build + sweep gates
├── CHANGELOG.md              per-version notes
├── chat_history.md           project narrative
└── README.md                 this file
```

### Conventions

- **Single contiguous source buffer.**  Tokens are
  `(start, length)` pointers into it; lexemes never get copied.
  Same idiom as clox's scanner.
- **AST as IR.**  No separate IR layer.  Passes attach inferred info
  to existing nodes (`Node.inferredType`, `qualifiedName.resolved`,
  `usage.isReference`) rather than rewriting tree shape.
- **Source-vs-inferred flag separation.**  Where a pass mutates a
  flag the source level cares about, we keep both:
  `usage.isReference` (post-inference) and
  `usage.isReferenceExplicit` (source-given).  The SysML emitter
  reads the latter so round-trip stays clean.
- **Skip-on-uncertainty for codegen.**  `--emit-c` skips any
  definition whose lowering isn't unambiguously correct, with a
  `/* skipped: NAME (reason) */` line so the gap is visible.
- **Visitor framework.**  `ast_walk.c` exposes a single per-NodeKind
  switch; the per-pass logic plugs in via callbacks.  Adding a new
  node kind means touching the visitor, not five duplicated
  switches.

## Test gates

`make sweep` runs all of these and reports a unified pass/fail:

| Gate              | What it checks                                         |
|-------------------|--------------------------------------------------------|
| `verify-tokens`   | every `TOKEN_*` referenced in `src/` is declared in `include/scanner.h` |
| `test-all`        | every `test/*.sysml` parses and passes the pipeline; every `test/bad/*.sysml` is rejected somewhere |
| `test-c`          | every test piped through `--emit-c \| cc -fsyntax-only` produces compilable C |
| `test-c-run`      | for tests with a companion `<name>.driver.c` and `expected/<name>.expect`: compile + link + run + diff stdout |
| `test-graphsml`   | every test's `--emit-json` output runs cleanly through the Python drawio adapter |
| `test-ptc`        | the OMG SysML PTC reference file: parser-only is a hard 0-error gate; default-mode is tracked against `PTC_BASELINE` (currently 15) |

The PTC reference file is the project's external quality bar — a
1580-line industry-standard SysML model.  Override:

```
make test-ptc PTC_FILE=/path/to/your.sysml PTC_BASELINE=N
```

to track a different reference, or to update the baseline after a
deliberate improvement / acceptable regression.

## Status

v0.22.  All sweep gates green:

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

The current focus is the executable-target pivot from
`design/c-codegen.md`: lift `--emit-c` from structural lowering to a
runnable C program.  v0.22 (calc defs → C functions) shipped; v0.23
(init functions for part defs + topo-sorted top-level inits) is next.

## License & origin

Written collaboratively by Claude (Anthropic) and the project author
over multiple sessions.  Style follows Bob Nystrom's
*Crafting Interpreters* clox idioms.
