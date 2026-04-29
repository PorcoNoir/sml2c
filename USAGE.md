# sml2c — Usage Reference

Command-line interface for the `sml2c` SysML v2 → C compiler.  Pair with
the design docs under `design/` for what each emitter actually produces.

## Synopsis

```
./bin/sml2c [MODE] [OPTIONS] [FILE]
```

If `FILE` is omitted, sml2c uses a small built-in `SAMPLE` source so the
binary has something to chew on for smoke testing.

## Modes

Choose at most one.  The default mode (no flag) parses, runs every
checker, and pretty-prints the typed AST.

| Flag             | Output                                              | Stream / Destination |
|------------------|-----------------------------------------------------|----------------------|
| *(none)*         | Pretty-printed typed AST                            | stdout               |
| `--tokens`       | One line per token from the scanner; no parse       | stdout               |
| `--emit-json`    | AST serialized as JSON                              | stdout               |
| `--emit-sysml`   | Canonical SysML v2 (round-trip-safe re-print)       | stdout               |
| `--emit-c`       | Single self-contained C file (validating library)   | stdout               |
| `--emit-fmu-c`   | Multi-file FMU project tree (FMI 3.0)               | `--output-dir <DIR>` |

## Options

### Global

| Flag       | Argument    | Description                                    |
|------------|-------------|------------------------------------------------|
| *(positional)* | `FILE`  | SysML source path.  Optional; built-in sample used if omitted. |

### Pipeline gates (skip a pass)

Each `--no-*` flag turns off the named pass and every pass after it.
Useful for layer-isolated debugging — e.g., `--no-typecheck` lets you
inspect resolver output without typechecker errors masking the issue.

| Flag                | Skips                                                |
|---------------------|------------------------------------------------------|
| `--no-resolve`      | Resolver + every later pass                          |
| `--no-typecheck`    | Typechecker + every later pass                       |
| `--no-redefcheck`   | Redefinition checker + every later pass              |
| `--no-connectcheck` | Connection checker + every later pass                |
| `--no-refcheck`     | Referential checker (last pass, so just this one)    |

Pipeline order, top-down: parse → resolve → typecheck → redefcheck →
connectcheck → refcheck → emit.

### `--emit-fmu-c` only

| Flag             | Argument          | Required | Description                                                              |
|------------------|-------------------|----------|--------------------------------------------------------------------------|
| `--output-dir`   | `<DIR>`           | yes      | Destination directory for the FMU project tree.  Created if it doesn't exist. |
| `--root`         | `<PartDefName>`   | only when ambiguous | Name of the outer part def to use as the FMU model.  Auto-picked when there's exactly one top-level part def. |

Generated tree layout:

```
<output-dir>/
├── CMakeLists.txt
├── include/
│   ├── model.h
│   ├── fmi3PlatformTypes.h
│   ├── fmi3FunctionTypes.h
│   └── fmi3Functions.h
├── src/
│   ├── fmu.c
│   ├── model.c
│   └── resources/
│       └── modelDescription.xml
└── test/
    ├── CMakeLists.txt
    └── test_fmu.c
```

## Environment variables

| Variable          | Default          | Description                                              |
|-------------------|------------------|----------------------------------------------------------|
| `SML2C_FMI3_DIR`  | `runtime/fmi3`   | Path to the vendored FMI 3.0 directory (headers + schemas).  Resolved relative to the working directory.  Override when running sml2c from a different cwd than the source tree. |

## Exit codes

| Code | Meaning                                                  |
|------|----------------------------------------------------------|
| `0`  | Success.                                                 |
| `64` | Bad CLI usage (missing argument to `--output-dir` etc.). |
| `65` | Source-level error (parse / resolve / typecheck / etc.). |
| `74` | I/O error (couldn't open source file or write output).   |
| `1`  | FMU emission failure (e.g. mkdir / fopen / cmake-side).  |

## Examples

### Inspect the parser's view

```bash
./bin/sml2c --tokens test/Thermostat.sysml
./bin/sml2c --emit-json test/Thermostat.sysml | jq '.members[0].name'
./bin/sml2c --emit-sysml test/Thermostat.sysml > /tmp/round-trip.sysml
diff test/Thermostat.sysml /tmp/round-trip.sysml
```

### Build a single-file C library from a SysML model

```bash
./bin/sml2c --emit-c test/ConstraintEmit.sysml > /tmp/ce.c
cc -std=c11 -I runtime -o /tmp/ce /tmp/ce.c test/ConstraintEmit.driver.c
/tmp/ce
```

### Build an FMU project tree, then compile it

```bash
./bin/sml2c --emit-fmu-c test/FmuFoundation.fmu.sysml \
            --output-dir /tmp/Foundation
cmake -S /tmp/Foundation -B /tmp/Foundation/_build
cmake --build /tmp/Foundation/_build
ctest --test-dir /tmp/Foundation/_build --output-on-failure
```

### Disambiguate between multiple top-level part defs

```bash
# Sleigh-Reindeer file: has two top-level part defs, sml2c errors
# without --root.
./bin/sml2c --emit-fmu-c L13.sysml --output-dir /tmp/Sleigh   --root Sleigh
./bin/sml2c --emit-fmu-c L13.sysml --output-dir /tmp/Reindeer --root Reindeer
```

### Run only through the parser (debug a typecheck issue)

```bash
./bin/sml2c --no-typecheck --emit-json brokenfile.sysml | less
```

## Make targets (development workflow)

These run against the test corpus, not user input.  Source: top-level
`Makefile`.

| Target           | What it does                                                              |
|------------------|---------------------------------------------------------------------------|
| `make`           | Build `bin/sml2c`.                                                        |
| `make clean`     | Remove `bin/`, `obj/`, `build/`.                                          |
| `make run`       | Build, then run the binary on the built-in sample.                        |
| `make tokens`    | Build, then dump tokens for the built-in sample.                          |
| `make test`      | Pretty-print AST for every `test/*.sysml`.                                |
| `make test-all`  | Same, in strict mode (any error fails the gate).                          |
| `make test-c`    | Run `--emit-c` against every test fixture, syntax-check the output with `cc -fsyntax-only`. |
| `make test-c-run`| Build + run every `test/*.driver.c`, diff against `test/expected/*.expect`. |
| `make test-fmu-c`| Run `--emit-fmu-c` against every `test/*.fmu.sysml`, validate XML against the official FMI 3.0 XSD, build with cmake, run ctest.  *Skips silently if cmake isn't installed.* |
| `make test-graphsml` | Run the GraphSysML adapter test corpus.                               |
| `make test-ptc`  | Run the PTC parser baseline (regression detection on the full PTC suite).|
| `make sweep`     | All gates above, in order.  Exit 0 iff every gate passes.                 |

A clean status line:

```
$ make sweep
==> verify-tokens
OK: all 132 referenced tokens are declared.
==> test-all (strict)
==> test-c (cc -fsyntax-only)
==> test-c-run (cc + ./binary + diff)
==> test-fmu-c (--emit-fmu-c + cmake build + ctest)
==> test-graphsml
==> test-ptc
sweep: all gates green
```

## See also

- `design/c-codegen.md` — the `--emit-c` design (typed-AST → standalone C library)
- `design/fmu-c-codegen.md` — the `--emit-fmu-c` design (typed-AST → FMI 3.0 FMU project)
- `runtime/sml2c-runtime.h` — the kernel + ISQ typedefs that `--emit-c` output depends on
- `runtime/fmi3/README.md` — vendoring notes for FMI 3.0.2 headers
- `CHANGELOG.md` — per-version history
