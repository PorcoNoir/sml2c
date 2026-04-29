/* codegen_fmu.c — FMU project-tree emitter (v0.25 foundation).
 *
 * Walks a typed SysML AST and writes an FMI 3.0-compliant FMU project
 * tree to disk.  Output is buildable with cmake against the vendored
 * FMI 3.0.2 headers in runtime/fmi3/.  See design/fmu-c-codegen.md.
 *
 * v0.25 scope: outer part def's scalar attributes become FMI parameter
 * variables.  Calc defs, constraints, ports, items, conjugate ports,
 * connections all skip in v0.25 with a comment.  v0.26 adds calc/check;
 * v0.27 adds ports and Terminals; v0.28 adds connection metadata.
 *
 * File layout (every file written by this emitter):
 *
 *   <output-dir>/CMakeLists.txt
 *   <output-dir>/include/model.h
 *   <output-dir>/include/fmi3{PlatformTypes,FunctionTypes,Functions}.h  (vendored copy)
 *   <output-dir>/src/fmu.c
 *   <output-dir>/src/model.c
 *   <output-dir>/src/resources/modelDescription.xml
 *   <output-dir>/src/resources/terminalsAndIcons/TerminalsAndIcons.xml
 *   <output-dir>/test/CMakeLists.txt
 *   <output-dir>/test/test_fmu.c                                                  */

#include "codegen_fmu.h"
#include "ast.h"
#include "builtin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

/* The FMI 3.0 entry-point template literals exceed ISO C99's 4095-
 * char minimum for supported string literals.  Both gcc and clang
 * handle multi-kilobyte literals without trouble; the -Wpedantic
 * warning is informational, not a real portability issue.  Suppress
 * for this file so `make` stays warning-free.                      */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

/* ===================================================================
 * Type mapping
 * =================================================================== */

/* SysML scalar type → FMI 3.0 ModelDescription element name.
 * Returns NULL if there's no clean mapping (the attribute should be
 * skipped with a comment in modelDescription.xml).                 */
static const char* fmiTypeTag(const Node* type) {
    if (!type) return NULL;
    if (type == builtinReal())    return "Float64";
    if (type == builtinNumber())  return "Float64";
    if (type == builtinInteger()) return "Int64";
    if (type == builtinBoolean()) return "Boolean";
    if (type == builtinString())  return "String";
    return NULL;
}

/* SysML scalar type → C type name in the generated model.h.  We use
 * the FMI 3.0 platform types (fmi3Float64 etc.) directly so the
 * struct fields integrate cleanly with the FMI getters/setters.   */
static const char* cFmiTypeName(const Node* type) {
    if (!type) return NULL;
    if (type == builtinReal())    return "fmi3Float64";
    if (type == builtinNumber())  return "fmi3Float64";
    if (type == builtinInteger()) return "fmi3Int64";
    if (type == builtinBoolean()) return "fmi3Boolean";
    if (type == builtinString())  return "fmi3String";
    return NULL;
}

/* Resolved type of an attribute, or NULL.                          */
static const Node* attrResolvedType(const Node* attr) {
    if (!attr || attr->kind != NODE_ATTRIBUTE) return NULL;
    const NodeList* types = &attr->as.attribute.types;
    if (types->count == 0) return NULL;
    const Node* tref = types->items[0];
    if (!tref || tref->kind != NODE_QUALIFIED_NAME) return NULL;
    return tref->as.qualifiedName.resolved;
}

/* Is this attribute v0.25-emittable?  (Scalar of a kernel kind, no
 * multiplicity, has a type.)                                       */
static bool attrEmittable(const Node* attr) {
    if (!attr || attr->kind != NODE_ATTRIBUTE) return false;
    if (attr->as.attribute.name.length == 0) return false;
    const Node* t = attrResolvedType(attr);
    if (!fmiTypeTag(t)) return false;
    /* TODO: handle multiplicity (arrays) in v0.27+; for v0.25 skip. */
    return true;
}

/* ===================================================================
 * Filesystem helpers
 * =================================================================== */

/* Recursive mkdir: ensure every component of `path` exists as a
 * directory.  Permissions 0755.  Returns 0 on success.            */
static int mkdirs(const char* path) {
    char buf[1024];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) {
        fprintf(stderr, "fmu: path too long: %s\n", path);
        return 1;
    }
    memcpy(buf, path, len + 1);
    /* Walk forward; at each '/', null-terminate, mkdir, restore.   */
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
                fprintf(stderr, "fmu: mkdir(%s): %s\n", buf, strerror(errno));
                return 1;
            }
            buf[i] = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "fmu: mkdir(%s): %s\n", buf, strerror(errno));
        return 1;
    }
    return 0;
}

/* Open a file for writing under outputDir/relPath, creating any
 * intermediate directories.  Returns NULL on error.               */
static FILE* openOutput(const char* outputDir, const char* relPath) {
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s", outputDir, relPath);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        fprintf(stderr, "fmu: path too long: %s/%s\n", outputDir, relPath);
        return NULL;
    }
    /* Ensure parent dir exists. */
    char dir[1024];
    memcpy(dir, path, (size_t)n + 1);
    char* slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (mkdirs(dir) != 0) return NULL;
    }
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "fmu: fopen(%s): %s\n", path, strerror(errno));
        return NULL;
    }
    return f;
}

/* Copy `src` to `dst` byte-for-byte, ensuring dst's parent dir exists.
 * Returns 0 on success.                                            */
static int copyFile(const char* src, const char* dst) {
    /* Make sure the destination's parent directory exists. */
    char dir[1024];
    size_t len = strlen(dst);
    if (len >= sizeof(dir)) {
        fprintf(stderr, "fmu: dst path too long: %s\n", dst);
        return 1;
    }
    memcpy(dir, dst, len + 1);
    char* slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (mkdirs(dir) != 0) return 1;
    }

    FILE* in = fopen(src, "rb");
    if (!in) {
        fprintf(stderr, "fmu: fopen(%s): %s\n", src, strerror(errno));
        return 1;
    }
    FILE* out = fopen(dst, "wb");
    if (!out) {
        fprintf(stderr, "fmu: fopen(%s): %s\n", dst, strerror(errno));
        fclose(in);
        return 1;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fprintf(stderr, "fmu: fwrite(%s): %s\n", dst, strerror(errno));
            fclose(in);
            fclose(out);
            return 1;
        }
    }
    fclose(in);
    fclose(out);
    return 0;
}

/* Copy the three vendored FMI 3.0 C headers into <outputDir>/include/. */
static int copyVendoredHeaders(const char* outputDir, const char* vendoredDir) {
    const char* names[] = {
        "fmi3PlatformTypes.h",
        "fmi3FunctionTypes.h",
        "fmi3Functions.h",
        NULL
    };
    char src[1024], dst[1024];
    for (int i = 0; names[i]; i++) {
        if (snprintf(src, sizeof(src), "%s/headers/%s", vendoredDir, names[i]) < 0 ||
            snprintf(dst, sizeof(dst), "%s/include/%s", outputDir, names[i]) < 0) {
            fprintf(stderr, "fmu: header path too long\n");
            return 1;
        }
        if (copyFile(src, dst) != 0) return 1;
    }
    return 0;
}

/* ===================================================================
 * Root part-def selection
 * =================================================================== */

/* The FMU's outer part def is selected per the v0.25 rule:
 *   1. If the user passed --root NAME, use the part def with that name.
 *   2. Otherwise, if there's exactly one top-level part def in the
 *      program, use it.
 *   3. Otherwise, error.
 *
 * "Top-level" means: directly contained in the program root, OR
 * directly contained in a top-level package.  Inner part defs nested
 * inside other part defs are NOT candidates.                       */
static bool isTopLevelPartDef(const Node* def) {
    if (!def || def->kind != NODE_DEFINITION) return false;
    return def->as.scope.defKind == DEF_PART;
}

static void collectTopLevelPartDefs(const Node* program,
                                    const Node** out, int* count, int max) {
    if (!program) return;
    /* Walk program-level children, plus children of top-level packages. */
    for (int i = 0; i < program->as.scope.memberCount; i++) {
        const Node* m = program->as.scope.members[i];
        if (!m) continue;
        if (isTopLevelPartDef(m)) {
            if (*count < max) out[(*count)++] = m;
        } else if (m->kind == NODE_PACKAGE) {
            for (int j = 0; j < m->as.scope.memberCount; j++) {
                const Node* mm = m->as.scope.members[j];
                if (isTopLevelPartDef(mm) && *count < max) {
                    out[(*count)++] = mm;
                }
            }
        }
    }
}

static const Node* findOuterPartDef(const Node* program, const char* rootName) {
    const Node* candidates[64];
    int count = 0;
    collectTopLevelPartDefs(program, candidates, &count, 64);
    if (count == 0) {
        fprintf(stderr, "fmu: no top-level part def found\n");
        return NULL;
    }
    if (rootName) {
        for (int i = 0; i < count; i++) {
            const Node* d = candidates[i];
            if ((int)strlen(rootName) == d->as.scope.name.length
             && memcmp(rootName, d->as.scope.name.start,
                       (size_t)d->as.scope.name.length) == 0) {
                return d;
            }
        }
        fprintf(stderr, "fmu: --root %s not found among top-level part defs\n",
                rootName);
        return NULL;
    }
    if (count > 1) {
        fprintf(stderr,
                "fmu: %d top-level part defs found; pass --root <name> "
                "to pick one. Candidates:\n", count);
        for (int i = 0; i < count; i++) {
            const Node* d = candidates[i];
            fprintf(stderr, "    %.*s\n",
                    d->as.scope.name.length, d->as.scope.name.start);
        }
        return NULL;
    }
    return candidates[0];
}

/* ===================================================================
 * Default-value rendering
 * =================================================================== */

/* Render an attribute's default literal as a C-language expression
 * for model.c initialization.  Falls back to a type-appropriate zero
 * if there's no default.  String defaults get quoted.              */
static void emitDefaultC(FILE* out, const Node* attr) {
    const Node* t = attrResolvedType(attr);
    const Node* def = attr->as.attribute.defaultValue;
    if (def && def->kind == NODE_LITERAL) {
        Token tk = def->as.literal.token;
        /* Booleans need the FMI 3.0 macros, not the SysML keywords. */
        if (def->as.literal.litKind == LIT_BOOL) {
            if (tk.length == 4 && memcmp(tk.start, "true", 4) == 0) {
                fputs("fmi3True", out);
            } else {
                fputs("fmi3False", out);
            }
            return;
        }
        /* Strings come tokenized with surrounding quotes already. */
        fwrite(tk.start, 1, (size_t)tk.length, out);
        return;
    }
    /* No-default fallback: type-appropriate zero. */
    if (t == builtinString())  { fputs("\"\"",       out); return; }
    if (t == builtinBoolean()) { fputs("fmi3False",  out); return; }
    if (t == builtinInteger()) { fputs("0",          out); return; }
    fputs("0.0", out);
}

/* Render an attribute's default value as the value of the `start`
 * attribute in modelDescription.xml.  XML differs from C in two
 * ways: booleans are "true"/"false" (not fmi3True), and strings
 * have no surrounding quotes (the XML attribute provides them). */
static void emitDefaultXml(FILE* out, const Node* attr) {
    const Node* t = attrResolvedType(attr);
    const Node* def = attr->as.attribute.defaultValue;
    if (def && def->kind == NODE_LITERAL) {
        Token tk = def->as.literal.token;
        if (def->as.literal.litKind == LIT_BOOL) {
            /* `true` / `false` already match XML schema literals. */
            fwrite(tk.start, 1, (size_t)tk.length, out);
            return;
        }
        /* String literal in source has surrounding quotes; strip them. */
        if (t == builtinString() && tk.length >= 2
                && tk.start[0] == '"' && tk.start[tk.length-1] == '"') {
            fwrite(tk.start + 1, 1, (size_t)tk.length - 2, out);
        } else {
            fwrite(tk.start, 1, (size_t)tk.length, out);
        }
        return;
    }
    if (t == builtinString())  { /* empty string, no chars */     return; }
    if (t == builtinBoolean()) { fputs("false", out);             return; }
    if (t == builtinInteger()) { fputs("0",     out);             return; }
    fputs("0.0", out);
}

/* ===================================================================
 * Per-file emitters
 * =================================================================== */

/* --- CMakeLists.txt (top-level) --- */

static void emitCMakeLists(FILE* out, const char* modelName) {
    fprintf(out,
        "# Generated by sml2c for FMU '%s'.  Do not edit by hand.\n"
        "cmake_minimum_required(VERSION 3.16)\n"
        "project(%s C)\n"
        "\n"
        "# Vendored FMI 3.0 headers live in include/ alongside model.h.\n"
        "# No FetchContent / network access needed at build time.\n"
        "set(CMAKE_C_STANDARD 11)\n"
        "set(CMAKE_C_STANDARD_REQUIRED ON)\n"
        "set(CMAKE_POSITION_INDEPENDENT_CODE ON)\n"
        "\n"
        "add_library(%s SHARED\n"
        "    src/fmu.c\n"
        "    src/model.c\n"
        ")\n"
        "target_include_directories(%s PRIVATE include)\n"
        "target_compile_definitions(%s PRIVATE FMI3_FUNCTION_PREFIX=%s_)\n"
        "\n"
        "if (CMAKE_C_COMPILER_ID MATCHES \"GNU|Clang\")\n"
        "    target_compile_options(%s PRIVATE -Wall -Wextra)\n"
        "endif()\n"
        "\n"
        "enable_testing()\n"
        "add_subdirectory(test)\n",
        modelName, modelName, modelName, modelName, modelName, modelName, modelName);
}

/* --- include/model.h --- */

static void emitModelH(FILE* out, const char* modelName, const Node* def) {
    fprintf(out,
        "/* model.h — generated by sml2c for FMU '%s'.\n"
        " * Do not edit by hand.\n"
        " *\n"
        " * Mirrors the variables in src/resources/modelDescription.xml.\n"
        " * Keep VR_* macros in sync with the valueReference attributes\n"
        " * in the model description (sml2c assigns them sequentially in\n"
        " * source-declaration order). */\n"
        "#ifndef MODEL_H\n"
        "#define MODEL_H\n"
        "\n"
        "#include \"fmi3PlatformTypes.h\"\n"
        "\n",
        modelName);

    /* Value-reference macros in source order. */
    int vr = 1;
    fputs("/* Value references (1-indexed, source-declaration order). */\n", out);
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_ATTRIBUTE) continue;
        if (!attrEmittable(m)) continue;
        fprintf(out, "#define VR_%.*s %d\n",
                m->as.attribute.name.length,
                m->as.attribute.name.start, vr++);
    }
    fputs("\n", out);

    /* ModelData struct. */
    fprintf(out, "typedef struct {\n");
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_ATTRIBUTE) continue;
        if (!attrEmittable(m)) continue;
        const char* ct = cFmiTypeName(attrResolvedType(m));
        fprintf(out, "    %-12s %.*s;\n",
                ct,
                m->as.attribute.name.length, m->as.attribute.name.start);
    }
    fputs("} ModelData;\n\n", out);

    fputs(
        "/* Set every field to its modelDescription start value.\n"
        " * Called from fmi3InstantiateCoSimulation. */\n"
        "void model_setDefaults(ModelData* m);\n"
        "\n"
        "#endif /* MODEL_H */\n", out);
}

/* --- src/model.c --- */

static void emitModelC(FILE* out, const char* modelName, const Node* def) {
    fprintf(out,
        "/* model.c — generated by sml2c for FMU '%s'.\n"
        " * Do not edit by hand. */\n"
        "#include \"model.h\"\n"
        "\n"
        "void model_setDefaults(ModelData* m) {\n",
        modelName);
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_ATTRIBUTE) continue;
        if (!attrEmittable(m)) continue;
        fprintf(out, "    m->%.*s = ",
                m->as.attribute.name.length, m->as.attribute.name.start);
        emitDefaultC(out, m);
        fputs(";\n", out);
    }
    fputs("}\n", out);
}

/* --- src/resources/modelDescription.xml --- */

/* Deterministic instantiation token: a UUID-shaped string derived
 * from the model name.  v0.25 doesn't try for full UUIDv4 randomness;
 * a stable token across re-runs is more important than uniqueness
 * across distinct FMUs (which the model name itself already gives).  */
static void emitInstantiationToken(FILE* out, const char* modelName) {
    /* Format: 8-4-4-4-12 hex.  We seed a small mixer with the model
     * name's bytes and emit 32 hex digits, properly hyphenated.    */
    unsigned long h = 0xcbf29ce484222325UL; /* FNV offset */
    for (const char* p = modelName; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 0x100000001b3UL;
    }
    /* Generate 4 32-bit chunks by re-mixing. */
    unsigned long parts[4];
    for (int i = 0; i < 4; i++) {
        h *= 0x100000001b3UL;
        h ^= (h >> 33);
        parts[i] = h;
    }
    fprintf(out, "{%08lx-%04lx-%04lx-%04lx-%012lx}",
            parts[0]        & 0xfffffffful,
            ((parts[1] >> 16) & 0x0ffful) | 0x4000ul,    /* version-4 nibble */
            (parts[1]         & 0x3ffful) | 0x8000ul,    /* variant 10xx */
            parts[2]        & 0xfffful,
            parts[3]        & 0xffffffffffffuL);         /* 12 hex = 48 bits */
}

static void emitModelDescription(FILE* out, const char* modelName, const Node* def) {
    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", out);
    fprintf(out,
        "<fmiModelDescription\n"
        "    fmiVersion=\"3.0\"\n"
        "    modelName=\"%s\"\n"
        "    instantiationToken=\"",
        modelName);
    emitInstantiationToken(out, modelName);
    fprintf(out,
        "\"\n"
        "    description=\"Generated by sml2c from SysML v2 source.\"\n"
        "    generationTool=\"sml2c\"\n"
        "    variableNamingConvention=\"structured\">\n"
        "\n"
        "  <CoSimulation\n"
        "      modelIdentifier=\"%s\"\n"
        "      canHandleVariableCommunicationStepSize=\"true\"\n"
        "      canGetAndSetFMUState=\"false\"\n"
        "      canSerializeFMUState=\"false\"\n"
        "      providesIntermediateUpdate=\"false\"\n"
        "      hasEventMode=\"false\"/>\n"
        "\n"
        "  <ModelVariables>\n",
        modelName);

    int vr = 1;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m) continue;
        if (m->kind != NODE_ATTRIBUTE) {
            /* Skipped non-attribute member, with a comment for visibility. */
            const char* what = "member";
            if (m->kind == NODE_USAGE)      what = "usage";
            if (m->kind == NODE_DEFINITION) what = "nested def";
            fprintf(out, "    <!-- skipped: %s (deferred to v0.26+) -->\n", what);
            continue;
        }
        if (!attrEmittable(m)) {
            fprintf(out, "    <!-- skipped: attribute %.*s (no kernel type / multiplicity) -->\n",
                    m->as.attribute.name.length, m->as.attribute.name.start);
            continue;
        }
        const char* tag = fmiTypeTag(attrResolvedType(m));
        if (strcmp(tag, "String") == 0) {
            /* Strings have a child <Start value="..."/> per fmi3Variable.xsd —
             * not a `start=` attribute like the numeric types.            */
            fprintf(out,
                "    <String name=\"%.*s\" valueReference=\"%d\" "
                "causality=\"parameter\" variability=\"tunable\">\n"
                "      <Start value=\"",
                m->as.attribute.name.length, m->as.attribute.name.start, vr++);
            emitDefaultXml(out, m);
            fputs("\"/>\n    </String>\n", out);
        } else {
            fprintf(out,
                "    <%s name=\"%.*s\" valueReference=\"%d\" "
                "causality=\"parameter\" variability=\"tunable\" start=\"",
                tag,
                m->as.attribute.name.length, m->as.attribute.name.start,
                vr++);
            emitDefaultXml(out, m);
            fputs("\"/>\n", out);
        }
    }

    fputs(
        "  </ModelVariables>\n"
        "\n"
        "  <ModelStructure/>\n"
        "</fmiModelDescription>\n", out);
}

/* --- src/resources/terminalsAndIcons/TerminalsAndIcons.xml ---
 * Reserved for v0.27+ when port usages start lowering to Terminals.
 * For v0.25 the orchestrator skips this file entirely (the schema
 * requires at least one <Terminal> child of <Terminals>), so this
 * function is currently unused.                                  */
__attribute__((unused))
static void emitTerminalsAndIcons(FILE* out, const char* modelName) {
    fprintf(out,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!-- Empty Terminals for %s — v0.25 has no port lowering yet.\n"
        "     v0.27 populates one <Terminal> per port usage on the\n"
        "     outer part def, with structured-named member variables\n"
        "     and a custom matchingRule for conjugate-pair detection. -->\n"
        "<fmiTerminalsAndIcons fmiVersion=\"3.0\">\n"
        "  <Terminals/>\n"
        "</fmiTerminalsAndIcons>\n",
        modelName);
}

/* --- src/fmu.c --- */
/* The FMI 3.0 entry-point surface.  Most are stubs returning fmi3Error
 * for unsupported feature classes; the lifecycle and Float64/Int64/
 * Boolean/String getters/setters are wired through ModelData.        */

static void emitFmuC_Stubs(FILE* out, const Node* def) {
    /* For each VR, route Get/Set through the ModelData struct. */
    fputs(
        "/* ---- Float64 getter / setter -------------------------------- */\n"
        "fmi3Status fmi3GetFloat64(fmi3Instance instance,\n"
        "                          const fmi3ValueReference vr[], size_t nvr,\n"
        "                          fmi3Float64 values[], size_t nValues) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    Component* c = (Component*)instance;\n"
        "    if (nValues < nvr) return fmi3Error;\n"
        "    for (size_t i = 0; i < nvr; i++) {\n"
        "        switch (vr[i]) {\n", out);
    int v = 1;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_ATTRIBUTE || !attrEmittable(m)) continue;
        const Node* t = attrResolvedType(m);
        const char* tag = fmiTypeTag(t);
        if (strcmp(tag, "Float64") == 0) {
            fprintf(out, "        case %d: values[i] = c->data.%.*s; break;\n",
                    v, m->as.attribute.name.length, m->as.attribute.name.start);
        }
        v++;
    }
    fputs(
        "        default: return fmi3Error;\n"
        "        }\n"
        "    }\n"
        "    return fmi3OK;\n"
        "}\n"
        "\n"
        "fmi3Status fmi3SetFloat64(fmi3Instance instance,\n"
        "                          const fmi3ValueReference vr[], size_t nvr,\n"
        "                          const fmi3Float64 values[], size_t nValues) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    Component* c = (Component*)instance;\n"
        "    if (nValues < nvr) return fmi3Error;\n"
        "    for (size_t i = 0; i < nvr; i++) {\n"
        "        switch (vr[i]) {\n", out);
    v = 1;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_ATTRIBUTE || !attrEmittable(m)) continue;
        const Node* t = attrResolvedType(m);
        const char* tag = fmiTypeTag(t);
        if (strcmp(tag, "Float64") == 0) {
            fprintf(out, "        case %d: c->data.%.*s = values[i]; break;\n",
                    v, m->as.attribute.name.length, m->as.attribute.name.start);
        }
        v++;
    }
    fputs(
        "        default: return fmi3Error;\n"
        "        }\n"
        "    }\n"
        "    return fmi3OK;\n"
        "}\n"
        "\n", out);

    /* Repeat for Int64. */
    fputs(
        "/* ---- Int64 getter / setter ---------------------------------- */\n"
        "fmi3Status fmi3GetInt64(fmi3Instance instance,\n"
        "                        const fmi3ValueReference vr[], size_t nvr,\n"
        "                        fmi3Int64 values[], size_t nValues) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    Component* c = (Component*)instance;\n"
        "    if (nValues < nvr) return fmi3Error;\n"
        "    for (size_t i = 0; i < nvr; i++) {\n"
        "        switch (vr[i]) {\n", out);
    v = 1;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_ATTRIBUTE || !attrEmittable(m)) continue;
        const Node* t = attrResolvedType(m);
        if (strcmp(fmiTypeTag(t), "Int64") == 0) {
            fprintf(out, "        case %d: values[i] = c->data.%.*s; break;\n",
                    v, m->as.attribute.name.length, m->as.attribute.name.start);
        }
        v++;
    }
    fputs(
        "        default: return fmi3Error;\n"
        "        }\n"
        "    }\n"
        "    return fmi3OK;\n"
        "}\n"
        "\n"
        "fmi3Status fmi3SetInt64(fmi3Instance instance,\n"
        "                        const fmi3ValueReference vr[], size_t nvr,\n"
        "                        const fmi3Int64 values[], size_t nValues) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    Component* c = (Component*)instance;\n"
        "    if (nValues < nvr) return fmi3Error;\n"
        "    for (size_t i = 0; i < nvr; i++) {\n"
        "        switch (vr[i]) {\n", out);
    v = 1;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_ATTRIBUTE || !attrEmittable(m)) continue;
        const Node* t = attrResolvedType(m);
        if (strcmp(fmiTypeTag(t), "Int64") == 0) {
            fprintf(out, "        case %d: c->data.%.*s = values[i]; break;\n",
                    v, m->as.attribute.name.length, m->as.attribute.name.start);
        }
        v++;
    }
    fputs(
        "        default: return fmi3Error;\n"
        "        }\n"
        "    }\n"
        "    return fmi3OK;\n"
        "}\n"
        "\n", out);

    /* Boolean and String — same pattern. */
    fputs(
        "/* ---- Boolean getter / setter -------------------------------- */\n"
        "fmi3Status fmi3GetBoolean(fmi3Instance instance,\n"
        "                          const fmi3ValueReference vr[], size_t nvr,\n"
        "                          fmi3Boolean values[], size_t nValues) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    Component* c = (Component*)instance;\n"
        "    if (nValues < nvr) return fmi3Error;\n"
        "    for (size_t i = 0; i < nvr; i++) {\n"
        "        switch (vr[i]) {\n", out);
    v = 1;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_ATTRIBUTE || !attrEmittable(m)) continue;
        const Node* t = attrResolvedType(m);
        if (strcmp(fmiTypeTag(t), "Boolean") == 0) {
            fprintf(out, "        case %d: values[i] = c->data.%.*s; break;\n",
                    v, m->as.attribute.name.length, m->as.attribute.name.start);
        }
        v++;
    }
    fputs(
        "        default: return fmi3Error;\n"
        "        }\n"
        "    }\n"
        "    return fmi3OK;\n"
        "}\n"
        "\n"
        "fmi3Status fmi3SetBoolean(fmi3Instance instance,\n"
        "                          const fmi3ValueReference vr[], size_t nvr,\n"
        "                          const fmi3Boolean values[], size_t nValues) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    Component* c = (Component*)instance;\n"
        "    if (nValues < nvr) return fmi3Error;\n"
        "    for (size_t i = 0; i < nvr; i++) {\n"
        "        switch (vr[i]) {\n", out);
    v = 1;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_ATTRIBUTE || !attrEmittable(m)) continue;
        const Node* t = attrResolvedType(m);
        if (strcmp(fmiTypeTag(t), "Boolean") == 0) {
            fprintf(out, "        case %d: c->data.%.*s = values[i]; break;\n",
                    v, m->as.attribute.name.length, m->as.attribute.name.start);
        }
        v++;
    }
    fputs(
        "        default: return fmi3Error;\n"
        "        }\n"
        "    }\n"
        "    return fmi3OK;\n"
        "}\n"
        "\n", out);

    /* String — get-only routing in v0.25 (no Set support; FMI's\n"
     * fmi3SetString needs a copy/free strategy we'll do later). */
    fputs(
        "/* ---- String getter / setter --------------------------------- */\n"
        "fmi3Status fmi3GetString(fmi3Instance instance,\n"
        "                         const fmi3ValueReference vr[], size_t nvr,\n"
        "                         fmi3String values[], size_t nValues) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    Component* c = (Component*)instance;\n"
        "    if (nValues < nvr) return fmi3Error;\n"
        "    for (size_t i = 0; i < nvr; i++) {\n"
        "        switch (vr[i]) {\n", out);
    v = 1;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_ATTRIBUTE || !attrEmittable(m)) continue;
        const Node* t = attrResolvedType(m);
        if (strcmp(fmiTypeTag(t), "String") == 0) {
            fprintf(out, "        case %d: values[i] = c->data.%.*s; break;\n",
                    v, m->as.attribute.name.length, m->as.attribute.name.start);
        }
        v++;
    }
    fputs(
        "        default: return fmi3Error;\n"
        "        }\n"
        "    }\n"
        "    return fmi3OK;\n"
        "}\n"
        "\n"
        "fmi3Status fmi3SetString(fmi3Instance instance,\n"
        "                         const fmi3ValueReference vr[], size_t nvr,\n"
        "                         const fmi3String values[], size_t nValues) {\n"
        "    /* v0.25: SetString is a no-op error; string ownership/copying\n"
        "     * needs a proper allocator, deferred to a later version. */\n"
        "    (void)instance; (void)vr; (void)nvr; (void)values; (void)nValues;\n"
        "    return fmi3Error;\n"
        "}\n"
        "\n", out);
}

/* The rest of fmu.c is fully boilerplate — it doesn't depend on the
 * model's variables.  Emit it from a static template.              */
static void emitFmuC_Boilerplate_Top(FILE* out) {
    fputs(
        "/* fmu.c — generated by sml2c.  Do not edit by hand.\n"
        " *\n"
        " * Implements the FMI 3.0 Co-Simulation entry-point surface.\n"
        " * Float64/Int64/Boolean/String are routed through ModelData;\n"
        " * Float32, Int8/16/32, UInt*, Binary, Clock, Discrete-state,\n"
        " * Event, FMU-state, and Configuration entry points are stubs\n"
        " * that return fmi3Error or fmi3OK as appropriate. */\n"
        "\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "#include \"fmi3Functions.h\"\n"
        "#include \"model.h\"\n"
        "\n"
        "typedef struct {\n"
        "    fmi3String              instanceName;\n"
        "    fmi3InstanceEnvironment instanceEnvironment;\n"
        "    fmi3LogMessageCallback  logMessage;\n"
        "    ModelData               data;\n"
        "    fmi3Boolean             initialized;\n"
        "} Component;\n"
        "\n"
        "#define EXPECT_INSTANCE(instance) \\\n"
        "    do { if (!(instance)) return fmi3Error; } while (0)\n"
        "\n"
        "/* ---- Stub generators for unsupported feature classes ------- */\n"
        "#define UNSUPPORTED_GET(NAME, T)                                         \\\n"
        "    fmi3Status NAME(fmi3Instance instance,                               \\\n"
        "                    const fmi3ValueReference vr[], size_t nvr,           \\\n"
        "                    T values[], size_t nValues) {                        \\\n"
        "        (void)instance; (void)vr; (void)nvr; (void)values; (void)nValues;\\\n"
        "        return fmi3Error;                                                \\\n"
        "    }\n"
        "#define UNSUPPORTED_SET(NAME, T)                                         \\\n"
        "    fmi3Status NAME(fmi3Instance instance,                               \\\n"
        "                    const fmi3ValueReference vr[], size_t nvr,           \\\n"
        "                    const T values[], size_t nValues) {                  \\\n"
        "        (void)instance; (void)vr; (void)nvr; (void)values; (void)nValues;\\\n"
        "        return fmi3Error;                                                \\\n"
        "    }\n"
        "#define UNSUPPORTED_CLOCK_GET(NAME, T)                                   \\\n"
        "    fmi3Status NAME(fmi3Instance instance,                               \\\n"
        "                    const fmi3ValueReference vr[], size_t nvr,           \\\n"
        "                    T values[]) {                                        \\\n"
        "        (void)instance; (void)vr; (void)nvr; (void)values;               \\\n"
        "        return fmi3Error;                                                \\\n"
        "    }\n"
        "#define UNSUPPORTED_CLOCK_SET(NAME, T)                                   \\\n"
        "    fmi3Status NAME(fmi3Instance instance,                               \\\n"
        "                    const fmi3ValueReference vr[], size_t nvr,           \\\n"
        "                    const T values[]) {                                  \\\n"
        "        (void)instance; (void)vr; (void)nvr; (void)values;               \\\n"
        "        return fmi3Error;                                                \\\n"
        "    }\n"
        "\n"
        "/* ---- Version + logging ------------------------------------- */\n"
        "const char* fmi3GetVersion(void) {\n"
        "    return fmi3Version;\n"
        "}\n"
        "\n"
        "fmi3Status fmi3SetDebugLogging(fmi3Instance instance,\n"
        "                               fmi3Boolean loggingOn,\n"
        "                               size_t nCategories,\n"
        "                               const fmi3String categories[]) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    (void)loggingOn; (void)nCategories; (void)categories;\n"
        "    return fmi3OK;\n"
        "}\n"
        "\n"
        "/* ---- Instantiation / lifecycle ----------------------------- */\n"
        "fmi3Instance fmi3InstantiateModelExchange(\n"
        "    fmi3String instanceName, fmi3String token, fmi3String resourcePath,\n"
        "    fmi3Boolean visible, fmi3Boolean loggingOn,\n"
        "    fmi3InstanceEnvironment env, fmi3LogMessageCallback logMessage) {\n"
        "    /* v0.25: Co-Simulation only. */\n"
        "    (void)instanceName; (void)token; (void)resourcePath; (void)visible;\n"
        "    (void)loggingOn; (void)env; (void)logMessage;\n"
        "    return NULL;\n"
        "}\n"
        "\n"
        "fmi3Instance fmi3InstantiateCoSimulation(\n"
        "    fmi3String instanceName, fmi3String token, fmi3String resourcePath,\n"
        "    fmi3Boolean visible, fmi3Boolean loggingOn,\n"
        "    fmi3Boolean eventModeUsed, fmi3Boolean earlyReturnAllowed,\n"
        "    const fmi3ValueReference required[], size_t nRequired,\n"
        "    fmi3InstanceEnvironment env,\n"
        "    fmi3LogMessageCallback logMessage,\n"
        "    fmi3IntermediateUpdateCallback intermediateUpdate) {\n"
        "    (void)token; (void)resourcePath; (void)visible; (void)loggingOn;\n"
        "    (void)eventModeUsed; (void)earlyReturnAllowed;\n"
        "    (void)required; (void)nRequired;\n"
        "    (void)intermediateUpdate;\n"
        "    Component* c = (Component*)calloc(1, sizeof(Component));\n"
        "    if (!c) return NULL;\n"
        "    c->instanceName = instanceName;\n"
        "    c->instanceEnvironment = env;\n"
        "    c->logMessage = logMessage;\n"
        "    c->initialized = fmi3False;\n"
        "    model_setDefaults(&c->data);\n"
        "    return (fmi3Instance)c;\n"
        "}\n"
        "\n"
        "fmi3Instance fmi3InstantiateScheduledExecution(\n"
        "    fmi3String instanceName, fmi3String token, fmi3String resourcePath,\n"
        "    fmi3Boolean visible, fmi3Boolean loggingOn,\n"
        "    fmi3InstanceEnvironment env,\n"
        "    fmi3LogMessageCallback logMessage,\n"
        "    fmi3ClockUpdateCallback clockUpdate,\n"
        "    fmi3LockPreemptionCallback lockPreemption,\n"
        "    fmi3UnlockPreemptionCallback unlockPreemption) {\n"
        "    (void)instanceName; (void)token; (void)resourcePath; (void)visible;\n"
        "    (void)loggingOn; (void)env; (void)logMessage;\n"
        "    (void)clockUpdate; (void)lockPreemption; (void)unlockPreemption;\n"
        "    return NULL;\n"
        "}\n"
        "\n"
        "void fmi3FreeInstance(fmi3Instance instance) {\n"
        "    if (instance) free(instance);\n"
        "}\n"
        "\n"
        "fmi3Status fmi3EnterInitializationMode(\n"
        "    fmi3Instance instance, fmi3Boolean toleranceDefined, fmi3Float64 tolerance,\n"
        "    fmi3Float64 startTime, fmi3Boolean stopTimeDefined, fmi3Float64 stopTime) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    (void)toleranceDefined; (void)tolerance;\n"
        "    (void)startTime; (void)stopTimeDefined; (void)stopTime;\n"
        "    return fmi3OK;\n"
        "}\n"
        "\n"
        "fmi3Status fmi3ExitInitializationMode(fmi3Instance instance) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    Component* c = (Component*)instance;\n"
        "    c->initialized = fmi3True;\n"
        "    return fmi3OK;\n"
        "}\n"
        "\n"
        "fmi3Status fmi3EnterEventMode(fmi3Instance instance) {\n"
        "    (void)instance; return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3Terminate(fmi3Instance instance) {\n"
        "    EXPECT_INSTANCE(instance); return fmi3OK;\n"
        "}\n"
        "fmi3Status fmi3Reset(fmi3Instance instance) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    Component* c = (Component*)instance;\n"
        "    model_setDefaults(&c->data);\n"
        "    c->initialized = fmi3False;\n"
        "    return fmi3OK;\n"
        "}\n"
        "\n"
        "/* ---- Co-Simulation step ------------------------------------ */\n"
        "fmi3Status fmi3DoStep(fmi3Instance instance,\n"
        "                      fmi3Float64 currentCommunicationPoint,\n"
        "                      fmi3Float64 communicationStepSize,\n"
        "                      fmi3Boolean noSetFMUStatePriorToCurrentPoint,\n"
        "                      fmi3Boolean *eventHandlingNeeded,\n"
        "                      fmi3Boolean *terminateSimulation,\n"
        "                      fmi3Boolean *earlyReturn,\n"
        "                      fmi3Float64 *lastSuccessfulTime) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    (void)currentCommunicationPoint;\n"
        "    (void)noSetFMUStatePriorToCurrentPoint;\n"
        "    if (eventHandlingNeeded)  *eventHandlingNeeded  = fmi3False;\n"
        "    if (terminateSimulation)  *terminateSimulation  = fmi3False;\n"
        "    if (earlyReturn)          *earlyReturn          = fmi3False;\n"
        "    if (lastSuccessfulTime)   *lastSuccessfulTime   =\n"
        "        currentCommunicationPoint + communicationStepSize;\n"
        "    return fmi3OK;\n"
        "}\n"
        "\n"
        "fmi3Status fmi3EnterStepMode(fmi3Instance instance) {\n"
        "    EXPECT_INSTANCE(instance); return fmi3OK;\n"
        "}\n"
        "fmi3Status fmi3EnterConfigurationMode(fmi3Instance instance) {\n"
        "    EXPECT_INSTANCE(instance); return fmi3OK;\n"
        "}\n"
        "fmi3Status fmi3ExitConfigurationMode(fmi3Instance instance) {\n"
        "    EXPECT_INSTANCE(instance); return fmi3OK;\n"
        "}\n"
        "\n", out);
}

static void emitFmuC_Boilerplate_Bottom(FILE* out) {
    /* All the Get/Set entry points we don't support: stub them out. */
    fputs(
        "/* ---- Unsupported scalar getters / setters ------------------ */\n"
        "UNSUPPORTED_GET(fmi3GetFloat32, fmi3Float32)\n"
        "UNSUPPORTED_SET(fmi3SetFloat32, fmi3Float32)\n"
        "UNSUPPORTED_GET(fmi3GetInt8,    fmi3Int8)\n"
        "UNSUPPORTED_SET(fmi3SetInt8,    fmi3Int8)\n"
        "UNSUPPORTED_GET(fmi3GetUInt8,   fmi3UInt8)\n"
        "UNSUPPORTED_SET(fmi3SetUInt8,   fmi3UInt8)\n"
        "UNSUPPORTED_GET(fmi3GetInt16,   fmi3Int16)\n"
        "UNSUPPORTED_SET(fmi3SetInt16,   fmi3Int16)\n"
        "UNSUPPORTED_GET(fmi3GetUInt16,  fmi3UInt16)\n"
        "UNSUPPORTED_SET(fmi3SetUInt16,  fmi3UInt16)\n"
        "UNSUPPORTED_GET(fmi3GetInt32,   fmi3Int32)\n"
        "UNSUPPORTED_SET(fmi3SetInt32,   fmi3Int32)\n"
        "UNSUPPORTED_GET(fmi3GetUInt32,  fmi3UInt32)\n"
        "UNSUPPORTED_SET(fmi3SetUInt32,  fmi3UInt32)\n"
        "UNSUPPORTED_GET(fmi3GetUInt64,  fmi3UInt64)\n"
        "UNSUPPORTED_SET(fmi3SetUInt64,  fmi3UInt64)\n"
        "\n"
        "/* Binary has the size-array signature variant. */\n"
        "fmi3Status fmi3GetBinary(fmi3Instance instance,\n"
        "                         const fmi3ValueReference vr[], size_t nvr,\n"
        "                         size_t valueSizes[],\n"
        "                         fmi3Binary values[], size_t nValues) {\n"
        "    (void)instance; (void)vr; (void)nvr; (void)valueSizes;\n"
        "    (void)values; (void)nValues;\n"
        "    return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3SetBinary(fmi3Instance instance,\n"
        "                         const fmi3ValueReference vr[], size_t nvr,\n"
        "                         const size_t valueSizes[],\n"
        "                         const fmi3Binary values[], size_t nValues) {\n"
        "    (void)instance; (void)vr; (void)nvr; (void)valueSizes;\n"
        "    (void)values; (void)nValues;\n"
        "    return fmi3Error;\n"
        "}\n"
        "UNSUPPORTED_CLOCK_GET(fmi3GetClock, fmi3Clock)\n"
        "UNSUPPORTED_CLOCK_SET(fmi3SetClock, fmi3Clock)\n"
        "\n"
        "/* ---- Variable dependency information ----------------------- */\n"
        "fmi3Status fmi3GetNumberOfVariableDependencies(fmi3Instance instance,\n"
        "                                               fmi3ValueReference valueReference,\n"
        "                                               size_t *nDependencies) {\n"
        "    (void)instance; (void)valueReference; (void)nDependencies;\n"
        "    return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3GetVariableDependencies(fmi3Instance instance,\n"
        "    fmi3ValueReference dependent, size_t elementIndicesOfDependent[],\n"
        "    fmi3ValueReference independents[], size_t elementIndicesOfIndependents[],\n"
        "    fmi3DependencyKind dependencyKinds[], size_t nDependencies) {\n"
        "    (void)instance; (void)dependent; (void)elementIndicesOfDependent;\n"
        "    (void)independents; (void)elementIndicesOfIndependents;\n"
        "    (void)dependencyKinds; (void)nDependencies;\n"
        "    return fmi3Error;\n"
        "}\n"
        "\n"
        "/* ---- FMU state save/restore -------------------------------- */\n"
        "fmi3Status fmi3GetFMUState(fmi3Instance instance, fmi3FMUState *state) {\n"
        "    (void)instance; (void)state; return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3SetFMUState(fmi3Instance instance, fmi3FMUState state) {\n"
        "    (void)instance; (void)state; return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3FreeFMUState(fmi3Instance instance, fmi3FMUState *state) {\n"
        "    (void)instance; (void)state; return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3SerializedFMUStateSize(fmi3Instance instance, fmi3FMUState state, size_t *size) {\n"
        "    (void)instance; (void)state; (void)size; return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3SerializeFMUState(fmi3Instance instance, fmi3FMUState state,\n"
        "                                 fmi3Byte serialized[], size_t size) {\n"
        "    (void)instance; (void)state; (void)serialized; (void)size; return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3DeserializeFMUState(fmi3Instance instance,\n"
        "                                   const fmi3Byte serialized[], size_t size,\n"
        "                                   fmi3FMUState *state) {\n"
        "    (void)instance; (void)serialized; (void)size; (void)state; return fmi3Error;\n"
        "}\n"
        "\n"
        "/* ---- Continuous-time state (Model Exchange only) ----------- */\n"
        "fmi3Status fmi3GetDirectionalDerivative(fmi3Instance instance,\n"
        "    const fmi3ValueReference unknowns[], size_t nUnknowns,\n"
        "    const fmi3ValueReference knowns[], size_t nKnowns,\n"
        "    const fmi3Float64 seed[], size_t nSeed,\n"
        "    fmi3Float64 sensitivity[], size_t nSensitivity) {\n"
        "    (void)instance; (void)unknowns; (void)nUnknowns;\n"
        "    (void)knowns; (void)nKnowns; (void)seed; (void)nSeed;\n"
        "    (void)sensitivity; (void)nSensitivity;\n"
        "    return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3GetAdjointDerivative(fmi3Instance instance,\n"
        "    const fmi3ValueReference unknowns[], size_t nUnknowns,\n"
        "    const fmi3ValueReference knowns[], size_t nKnowns,\n"
        "    const fmi3Float64 seed[], size_t nSeed,\n"
        "    fmi3Float64 sensitivity[], size_t nSensitivity) {\n"
        "    (void)instance; (void)unknowns; (void)nUnknowns;\n"
        "    (void)knowns; (void)nKnowns; (void)seed; (void)nSeed;\n"
        "    (void)sensitivity; (void)nSensitivity;\n"
        "    return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3SetTime(fmi3Instance instance, fmi3Float64 time) {\n"
        "    (void)instance; (void)time; return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3SetContinuousStates(fmi3Instance instance,\n"
        "                                   const fmi3Float64 cs[], size_t n) {\n"
        "    (void)instance; (void)cs; (void)n; return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3GetContinuousStateDerivatives(fmi3Instance instance,\n"
        "                                             fmi3Float64 derivatives[], size_t n) {\n"
        "    (void)instance; (void)derivatives; (void)n; return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3GetEventIndicators(fmi3Instance instance,\n"
        "                                  fmi3Float64 indicators[], size_t n) {\n"
        "    (void)instance; (void)indicators; (void)n; return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3GetContinuousStates(fmi3Instance instance,\n"
        "                                   fmi3Float64 cs[], size_t n) {\n"
        "    (void)instance; (void)cs; (void)n; return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3GetNominalsOfContinuousStates(fmi3Instance instance,\n"
        "                                             fmi3Float64 nominals[], size_t n) {\n"
        "    (void)instance; (void)nominals; (void)n; return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3GetNumberOfEventIndicators(fmi3Instance instance, size_t *n) {\n"
        "    (void)instance; if (n) *n = 0; return fmi3OK;\n"
        "}\n"
        "fmi3Status fmi3GetNumberOfContinuousStates(fmi3Instance instance, size_t *n) {\n"
        "    (void)instance; if (n) *n = 0; return fmi3OK;\n"
        "}\n"
        "fmi3Status fmi3CompletedIntegratorStep(fmi3Instance instance,\n"
        "    fmi3Boolean noSetFMUStatePriorToCurrentPoint,\n"
        "    fmi3Boolean *enterEventMode, fmi3Boolean *terminateSimulation) {\n"
        "    (void)instance; (void)noSetFMUStatePriorToCurrentPoint;\n"
        "    if (enterEventMode)     *enterEventMode = fmi3False;\n"
        "    if (terminateSimulation) *terminateSimulation = fmi3False;\n"
        "    return fmi3Error;\n"
        "}\n"
        "\n"
        "/* ---- Discrete states (Event Mode) -------------------------- */\n"
        "fmi3Status fmi3UpdateDiscreteStates(fmi3Instance instance,\n"
        "    fmi3Boolean *discreteStatesNeedUpdate,\n"
        "    fmi3Boolean *terminateSimulation,\n"
        "    fmi3Boolean *nominalsOfContinuousStatesChanged,\n"
        "    fmi3Boolean *valuesOfContinuousStatesChanged,\n"
        "    fmi3Boolean *nextEventTimeDefined,\n"
        "    fmi3Float64 *nextEventTime) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    if (discreteStatesNeedUpdate)         *discreteStatesNeedUpdate = fmi3False;\n"
        "    if (terminateSimulation)              *terminateSimulation = fmi3False;\n"
        "    if (nominalsOfContinuousStatesChanged)*nominalsOfContinuousStatesChanged = fmi3False;\n"
        "    if (valuesOfContinuousStatesChanged)  *valuesOfContinuousStatesChanged = fmi3False;\n"
        "    if (nextEventTimeDefined)             *nextEventTimeDefined = fmi3False;\n"
        "    if (nextEventTime)                    *nextEventTime = 0.0;\n"
        "    return fmi3OK;\n"
        "}\n"
        "fmi3Status fmi3EvaluateDiscreteStates(fmi3Instance instance) {\n"
        "    (void)instance; return fmi3OK;\n"
        "}\n"
        "\n"
        "/* ---- Clocks: interval / shift ------------------------------ */\n"
        "fmi3Status fmi3GetIntervalDecimal(fmi3Instance instance,\n"
        "    const fmi3ValueReference vr[], size_t nvr,\n"
        "    fmi3Float64 intervals[], fmi3IntervalQualifier qualifiers[]) {\n"
        "    (void)instance; (void)vr; (void)nvr; (void)intervals; (void)qualifiers;\n"
        "    return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3GetIntervalFraction(fmi3Instance instance,\n"
        "    const fmi3ValueReference vr[], size_t nvr,\n"
        "    fmi3UInt64 counters[], fmi3UInt64 resolutions[],\n"
        "    fmi3IntervalQualifier qualifiers[]) {\n"
        "    (void)instance; (void)vr; (void)nvr;\n"
        "    (void)counters; (void)resolutions; (void)qualifiers;\n"
        "    return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3SetIntervalDecimal(fmi3Instance instance,\n"
        "    const fmi3ValueReference vr[], size_t nvr,\n"
        "    const fmi3Float64 intervals[]) {\n"
        "    (void)instance; (void)vr; (void)nvr; (void)intervals;\n"
        "    return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3SetIntervalFraction(fmi3Instance instance,\n"
        "    const fmi3ValueReference vr[], size_t nvr,\n"
        "    const fmi3UInt64 counters[], const fmi3UInt64 resolutions[]) {\n"
        "    (void)instance; (void)vr; (void)nvr; (void)counters; (void)resolutions;\n"
        "    return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3GetShiftDecimal(fmi3Instance instance,\n"
        "    const fmi3ValueReference vr[], size_t nvr, fmi3Float64 shifts[]) {\n"
        "    (void)instance; (void)vr; (void)nvr; (void)shifts;\n"
        "    return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3GetShiftFraction(fmi3Instance instance,\n"
        "    const fmi3ValueReference vr[], size_t nvr,\n"
        "    fmi3UInt64 counters[], fmi3UInt64 resolutions[]) {\n"
        "    (void)instance; (void)vr; (void)nvr; (void)counters; (void)resolutions;\n"
        "    return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3SetShiftDecimal(fmi3Instance instance,\n"
        "    const fmi3ValueReference vr[], size_t nvr,\n"
        "    const fmi3Float64 shifts[]) {\n"
        "    (void)instance; (void)vr; (void)nvr; (void)shifts;\n"
        "    return fmi3Error;\n"
        "}\n"
        "fmi3Status fmi3SetShiftFraction(fmi3Instance instance,\n"
        "    const fmi3ValueReference vr[], size_t nvr,\n"
        "    const fmi3UInt64 counters[], const fmi3UInt64 resolutions[]) {\n"
        "    (void)instance; (void)vr; (void)nvr; (void)counters; (void)resolutions;\n"
        "    return fmi3Error;\n"
        "}\n", out);
}

static void emitFmuC(FILE* out, const char* modelName, const Node* def) {
    (void)modelName;
    emitFmuC_Boilerplate_Top(out);
    emitFmuC_Stubs(out, def);
    emitFmuC_Boilerplate_Bottom(out);
}

/* --- test/CMakeLists.txt --- */

static void emitTestCMakeLists(FILE* out, const char* modelName) {
    fprintf(out,
        "# Generated by sml2c.  Do not edit.\n"
        "add_executable(test_fmu test_fmu.c)\n"
        "target_include_directories(test_fmu PRIVATE ${CMAKE_SOURCE_DIR}/include)\n"
        "target_link_libraries(test_fmu PRIVATE %s)\n"
        "if (CMAKE_C_COMPILER_ID MATCHES \"GNU|Clang\")\n"
        "    target_compile_options(test_fmu PRIVATE -Wall -Wextra)\n"
        "endif()\n"
        "add_test(NAME smoke COMMAND test_fmu)\n",
        modelName);
}

/* --- test/test_fmu.c --- */

static void emitTestFmuC(FILE* out, const char* modelName, const Node* def) {
    (void)modelName;
    fprintf(out,
        "/* test_fmu.c — generated by sml2c.\n"
        " *\n"
        " * Smoke test: instantiate the FMU, transition through Init,\n"
        " * read back every parameter through fmi3Get*, free.\n"
        " * Exit code 0 = pass, non-zero = fail.\n"
        " *\n"
        " * Linked directly against the FMU library (no dlopen) to keep\n"
        " * the test plumbing small.  fmi3 functions are name-prefixed\n"
        " * with %s_ via FMI3_FUNCTION_PREFIX, so we set it before\n"
        " * including fmi3Functions.h.                                */\n"
        "#define FMI3_FUNCTION_PREFIX %s_\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "#include \"fmi3Functions.h\"\n"
        "#include \"model.h\"\n"
        "\n"
        "static void onLog(fmi3InstanceEnvironment env, fmi3Status status,\n"
        "                  fmi3String category, fmi3String message) {\n"
        "    (void)env;\n"
        "    fprintf(stderr, \"[%%d %%s] %%s\\n\", (int)status,\n"
        "            category ? category : \"\", message);\n"
        "}\n"
        "\n"
        "int main(void) {\n"
        "    fmi3Instance c = fmi3InstantiateCoSimulation(\n"
        "        \"test\", \"\", NULL,\n"
        "        fmi3False, fmi3False, fmi3False, fmi3False,\n"
        "        NULL, 0, NULL, onLog, NULL);\n"
        "    if (!c) { fprintf(stderr, \"instantiate failed\\n\"); return 1; }\n"
        "\n"
        "    if (fmi3EnterInitializationMode(c, fmi3False, 0.0, 0.0,\n"
        "                                   fmi3False, 0.0) != fmi3OK) {\n"
        "        fprintf(stderr, \"enter init failed\\n\"); return 2;\n"
        "    }\n"
        "    if (fmi3ExitInitializationMode(c) != fmi3OK) {\n"
        "        fprintf(stderr, \"exit init failed\\n\"); return 3;\n"
        "    }\n"
        "\n"
        "    int errors = 0;\n",
        modelName, modelName);

    /* Generate per-attribute read-back assertions. */
    int v = 1;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_ATTRIBUTE || !attrEmittable(m)) continue;
        const Node* t = attrResolvedType(m);
        const char* tag = fmiTypeTag(t);
        const char* name_start = m->as.attribute.name.start;
        int name_len = m->as.attribute.name.length;

        fprintf(out, "    {\n");
        fprintf(out, "        fmi3ValueReference vr = %d;\n", v);

        if (strcmp(tag, "Float64") == 0 || strcmp(tag, "Int64") == 0
                                        || strcmp(tag, "Boolean") == 0) {
            const char* ctype = (strcmp(tag, "Float64") == 0) ? "fmi3Float64"
                              : (strcmp(tag, "Int64")   == 0) ? "fmi3Int64"
                                                              : "fmi3Boolean";
            fprintf(out, "        %s val = 0;\n", ctype);
            fprintf(out, "        if (fmi3Get%s(c, &vr, 1, &val, 1) != fmi3OK) {\n", tag);
            fprintf(out, "            fprintf(stderr, \"get %.*s failed\\n\"); errors++;\n",
                    name_len, name_start);
            fprintf(out, "        } else {\n");
            fprintf(out, "            printf(\"%.*s = ", name_len, name_start);
            fprintf(out, (strcmp(tag, "Float64") == 0) ? "%%g\\n\", (double)val);\n"
                       : (strcmp(tag, "Int64")   == 0) ? "%%lld\\n\", (long long)val);\n"
                                                       : "%%d\\n\", (int)val);\n");
            fprintf(out, "        }\n");
        } else if (strcmp(tag, "String") == 0) {
            fprintf(out, "        fmi3String val = NULL;\n");
            fprintf(out, "        if (fmi3GetString(c, &vr, 1, &val, 1) != fmi3OK) {\n");
            fprintf(out, "            fprintf(stderr, \"get %.*s failed\\n\"); errors++;\n",
                    name_len, name_start);
            fprintf(out, "        } else {\n");
            fprintf(out, "            printf(\"%.*s = %%s\\n\", val ? val : \"(null)\");\n",
                    name_len, name_start);
            fprintf(out, "        }\n");
        }
        fprintf(out, "    }\n");
        v++;
    }

    fputs(
        "\n"
        "    fmi3Boolean evNeed = fmi3False, term = fmi3False, early = fmi3False;\n"
        "    fmi3Float64 lastT = 0.0;\n"
        "    if (fmi3DoStep(c, 0.0, 0.01, fmi3False,\n"
        "                   &evNeed, &term, &early, &lastT) != fmi3OK) {\n"
        "        fprintf(stderr, \"DoStep failed\\n\"); errors++;\n"
        "    }\n"
        "\n"
        "    if (fmi3Terminate(c) != fmi3OK) errors++;\n"
        "    fmi3FreeInstance(c);\n"
        "    return errors == 0 ? 0 : 4;\n"
        "}\n", out);
}

/* ===================================================================
 * Top-level orchestration
 * =================================================================== */

int emitFmuProject(const char* outputDir, Node* program,
                   const char* rootName, const char* vendoredDir) {
    if (!outputDir || !program || !vendoredDir) {
        fprintf(stderr, "fmu: emitFmuProject: NULL argument\n");
        return 1;
    }
    const Node* def = findOuterPartDef(program, rootName);
    if (!def) return 1;

    /* Stash the model name (null-terminated) for use in templates. */
    char modelName[128];
    if (def->as.scope.name.length >= (int)sizeof(modelName)) {
        fprintf(stderr, "fmu: model name too long\n");
        return 1;
    }
    memcpy(modelName, def->as.scope.name.start,
           (size_t)def->as.scope.name.length);
    modelName[def->as.scope.name.length] = '\0';

    /* Create the directory tree. */
    if (mkdirs(outputDir) != 0) return 1;

    /* Copy vendored FMI 3.0 headers. */
    if (copyVendoredHeaders(outputDir, vendoredDir) != 0) return 1;

    /* Write each generated file. */
    struct { const char* rel; void (*emit)(FILE*, const char*, const Node*); } files[] = {
        { "include/model.h", emitModelH },
        { "src/model.c",     emitModelC },
        { "src/fmu.c",       emitFmuC },
        { "test/test_fmu.c", emitTestFmuC },
    };
    for (size_t i = 0; i < sizeof(files)/sizeof(files[0]); i++) {
        FILE* f = openOutput(outputDir, files[i].rel);
        if (!f) return 1;
        files[i].emit(f, modelName, def);
        fclose(f);
    }

    /* Files that don't need the part def AST. */
    FILE* f;
    if (!(f = openOutput(outputDir, "CMakeLists.txt"))) return 1;
    emitCMakeLists(f, modelName); fclose(f);
    if (!(f = openOutput(outputDir, "test/CMakeLists.txt"))) return 1;
    emitTestCMakeLists(f, modelName); fclose(f);
    if (!(f = openOutput(outputDir, "src/resources/modelDescription.xml"))) return 1;
    emitModelDescription(f, modelName, def); fclose(f);
    /* TerminalsAndIcons.xml is *optional* in FMI 3.0 (omit when there
     * are no Terminals — the schema requires at least one Terminal
     * child of the Terminals element).  v0.25 has nothing to emit
     * here yet; v0.27 starts populating it from port usages.       */

    fprintf(stderr, "fmu: wrote %s for part def %s\n", outputDir, modelName);
    return 0;
}
