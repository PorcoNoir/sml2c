/* codegen_fmu.h — FMU project-tree emitter for `--emit-fmu-c`.
 *
 * Where `--emit-c` writes a single self-contained C file to a stream,
 * `--emit-fmu-c` produces a multi-file FMU project tree on disk:
 *
 *   <output-dir>/
 *   ├── CMakeLists.txt
 *   ├── include/
 *   │   ├── model.h                 (ModelData struct, value references)
 *   │   ├── fmi3PlatformTypes.h     (vendored FMI 3.0)
 *   │   ├── fmi3FunctionTypes.h     (vendored)
 *   │   └── fmi3Functions.h         (vendored)
 *   ├── src/
 *   │   ├── fmu.c                   (FMI 3.0 entry-point surface)
 *   │   ├── model.c                 (model defaults + body)
 *   │   └── resources/
 *   │       ├── modelDescription.xml
 *   │       └── terminalsAndIcons/TerminalsAndIcons.xml
 *   └── test/
 *       ├── CMakeLists.txt
 *       └── test_fmu.c              (Instantiate→GetFloat64→Free smoke)
 *
 * v0.25 (foundation): scalar attributes on the chosen outer part def
 * become FMI parameter variables.  Inner/sibling part defs, ports,
 * calc defs, and constraints all skip with a comment in v0.25;
 * v0.26+ wires them in.  See design/fmu-c-codegen.md for the roadmap. */

#ifndef CODEGEN_FMU_H
#define CODEGEN_FMU_H

#include "ast.h"

/* Emit an FMU project tree under `outputDir`.
 *
 *   outputDir   — the directory to populate.  Must be empty or not
 *                 yet exist.  Directory and any parents are created.
 *   program     — typed AST root from parse + resolve + typecheck.
 *   rootName    — optional name of the part def to use as the FMU root.
 *                 NULL means "auto-pick the single top-level part def
 *                 in the program; error if there's more than one."
 *   vendoredDir — path to the sml2c source tree's runtime/fmi3/
 *                 directory, so we can copy the FMI 3.0 headers into
 *                 <outputDir>/include/.  Typically "runtime/fmi3"
 *                 relative to where sml2c was built.
 *
 * Returns 0 on success, non-zero on any failure (writes diagnostic to
 * stderr).                                                          */
int emitFmuProject(const char* outputDir,
                   Node*       program,
                   const char* rootName,
                   const char* vendoredDir);

#endif /* CODEGEN_FMU_H */
