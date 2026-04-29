/* codegen_fmu.c — FMU project-tree emitter (`--emit-fmu-c`).
 *
 * Walks a typed SysML AST and writes an FMI 3.0-compliant FMU project
 * tree to disk.  Output is buildable with cmake against the vendored
 * FMI 3.0.2 headers in runtime/fmi3/.  See design/fmu-c-codegen.md
 * for the design rationale and roadmap.
 *
 * Currently shipped (per CHANGELOG.md):
 *
 *   v0.25 — Project-tree foundation: scalar attributes become FMI
 *           parameter variables; vendored headers; lifecycle entry
 *           points; cmake driver.
 *   v0.26 — Calc defs lower to free C functions in src/model.c;
 *           inline assert constraints lower to a model_check
 *           function called from fmi3ExitInitializationMode whose
 *           failures route through the FMI 3.0 logger callback.
 *   v0.27 — Port usages on the outer part def lower to FMI 3.0
 *           Terminals with structured-named member variables.
 *           Conjugate ports (`~PortDef`) flip every member's
 *           causality.  Items with no attributes lower as Binary.
 *
 * Per-FMU output layout (every file written by emitFmuProject):
 *
 *   <output-dir>/CMakeLists.txt
 *   <output-dir>/include/model.h
 *   <output-dir>/include/fmi3{PlatformTypes,FunctionTypes,Functions}.h  (vendored)
 *   <output-dir>/src/fmu.c
 *   <output-dir>/src/model.c
 *   <output-dir>/src/resources/modelDescription.xml
 *   <output-dir>/src/resources/terminalsAndIcons/TerminalsAndIcons.xml  (only if ports)
 *   <output-dir>/test/CMakeLists.txt
 *   <output-dir>/test/test_fmu.c
 *
 * Internal layout (top-to-bottom, marked by `=== Section ===` banners):
 *
 *   1. Type mapping            — SysML-type → FMI-type tables, expression
 *                                emitter (emitFmuExpr / emitFmuExprAsCString),
 *                                calc-def emitters.  This section also has
 *                                a few small helpers that probably belong
 *                                in their own sub-section but haven't been
 *                                broken out yet.
 *   2. Port usage helpers      — Port/item navigation; the FmuVarVisitor
 *                                struct + walkFmuVars.  Every emitter that
 *                                cares about VR ordering goes through this.
 *   3. Filesystem helpers      — mkdirs, openOutput, copyVendoredHeaders.
 *   4. Root part-def selection — findOuterPartDef, --root resolution.
 *   5. Default-value rendering — emitDefaultXml (only thing left here
 *                                after v0.26 retired emitDefaultC).
 *   6. Per-file emitters       — One function (or small cluster) per
 *                                output file.  Some are split further:
 *                                emitFmuC = Top + Stubs + Bottom, and
 *                                Bottom is itself split into seven
 *                                feature-specific helpers.
 *   7. Top-level orchestration — emitFmuProject: the public entry point.
 *
 * Convention: any walk over the outer part def's variables (struct
 * fields, VR macros, modelDescription entries, fmu.c switch cases,
 * test_fmu.c readbacks) MUST go through walkFmuVars so VRs stay in
 * lockstep across emitters.  Open-coded `for (int i = 0; i < def->
 * as.scope.memberCount; i++)` walks are fine in helpers that aren't
 * allocating VRs (e.g., defNeedsCheck) but are a smell anywhere
 * that emits a VR.
 *
 * Test coverage: the four fixtures named test/<X>.fmu.sysml exercise
 * the shipped lowering paths end-to-end (parse → emit → cmake build →
 * ctest).  See `make test-fmu-c`.                                  */

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

/* Resolved type of a usage's `: T` annotation (used for calc-def
 * `in` parameters), or NULL.                                       */
static const Node* usageResolvedType(const Node* usage) {
    if (!usage || usage->kind != NODE_USAGE) return NULL;
    const NodeList* types = &usage->as.usage.types;
    if (types->count == 0) return NULL;
    const Node* tref = types->items[0];
    if (!tref || tref->kind != NODE_QUALIFIED_NAME) return NULL;
    return tref->as.qualifiedName.resolved;
}

/* For intermediate attributes inside calc-def bodies, the type may be
 * inferred (no `: T` written) — the typechecker fills inferredType.
 * It lives on Node directly (used by every expression-kind node).  */
static const Node* attrTypeOrInferred(const Node* attr) {
    const Node* t = attrResolvedType(attr);
    if (t) return t;
    if (attr && attr->inferredType) return attr->inferredType;
    return NULL;
}

/* Operator → C surface form (used inside expressions in model.c). */
static const char* binOpC(TokenType t) {
    switch (t) {
    case TOKEN_PLUS:           return "+";
    case TOKEN_MINUS:          return "-";
    case TOKEN_STAR:           return "*";
    case TOKEN_SLASH:          return "/";
    case TOKEN_LESS:           return "<";
    case TOKEN_LESS_EQUAL:     return "<=";
    case TOKEN_GREATER:        return ">";
    case TOKEN_GREATER_EQUAL:  return ">=";
    case TOKEN_EQUAL_EQUAL:    return "==";
    case TOKEN_BANG_EQUAL:     return "!=";
    case TOKEN_AND:            return "&&";
    case TOKEN_OR:             return "||";
    default:                   return NULL;
    }
}
static const char* unOpC(TokenType t) {
    switch (t) {
    case TOKEN_MINUS: return "-";
    case TOKEN_BANG:  return "!";
    default:          return NULL;
    }
}

/* Operator → SysML surface form (used in constraint failure messages
 * so the reported text matches the source).                         */
static const char* binOpSysml(TokenType t) {
    switch (t) {
    case TOKEN_PLUS:           return "+";
    case TOKEN_MINUS:          return "-";
    case TOKEN_STAR:           return "*";
    case TOKEN_SLASH:          return "/";
    case TOKEN_LESS:           return "<";
    case TOKEN_LESS_EQUAL:     return "<=";
    case TOKEN_GREATER:        return ">";
    case TOKEN_GREATER_EQUAL:  return ">=";
    case TOKEN_EQUAL_EQUAL:    return "==";
    case TOKEN_BANG_EQUAL:     return "!=";
    case TOKEN_AND:            return "and";
    case TOKEN_OR:             return "or";
    default:                   return "?";
    }
}
static const char* unOpSysml(TokenType t) {
    switch (t) {
    case TOKEN_MINUS: return "-";
    case TOKEN_BANG:  return "not ";
    default:          return "";
    }
}

/* Is `resolved` a member attribute of part def `def`?  Used by the
 * expression emitter to decide whether to prefix a bare reference
 * with `m->` (the FMU's equivalent of `--emit-c`'s `self->`).      */
static bool isMemberOfPartDef(const Node* def, const Node* resolved) {
    if (!def || !resolved) return false;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (m == resolved) return true;
    }
    return false;
}

/* Recursive expression emitter for model.c contexts.
 *
 *   memberOf — when non-NULL, bare references resolving to one of
 *              this part def's attributes get prefixed with `m->`.
 *              Inside calc-def bodies (where bare names refer to
 *              function parameters or local intermediates) it's NULL.  */
static void emitFmuExpr(FILE* out, const Node* e, const Node* memberOf) {
    if (!e) return;
    switch (e->kind) {
    case NODE_LITERAL: {
        Token tk = e->as.literal.token;
        if (e->as.literal.litKind == LIT_BOOL) {
            if (tk.length == 4 && memcmp(tk.start, "true", 4) == 0) {
                fputs("fmi3True", out);
            } else {
                fputs("fmi3False", out);
            }
            return;
        }
        fwrite(tk.start, 1, (size_t)tk.length, out);
        break;
    }
    case NODE_QUALIFIED_NAME: {
        if (e->as.qualifiedName.partCount == 1
                && memberOf
                && isMemberOfPartDef(memberOf, e->as.qualifiedName.resolved)) {
            Token tk = e->as.qualifiedName.parts[0];
            fprintf(out, "m->%.*s", tk.length, tk.start);
            return;
        }
        for (int i = 0; i < e->as.qualifiedName.partCount; i++) {
            if (i > 0) fputs("_", out);
            Token tk = e->as.qualifiedName.parts[i];
            fwrite(tk.start, 1, (size_t)tk.length, out);
        }
        break;
    }
    case NODE_BINARY: {
        const char* op = binOpC(e->as.binary.op.type);
        if (!op) { fputs("/* unsupported op */", out); return; }
        fputc('(', out);
        emitFmuExpr(out, e->as.binary.left,  memberOf);
        fprintf(out, " %s ", op);
        emitFmuExpr(out, e->as.binary.right, memberOf);
        fputc(')', out);
        break;
    }
    case NODE_UNARY: {
        const char* op = unOpC(e->as.unary.op.type);
        if (!op) { fputs("/* unsupported op */", out); return; }
        fputs(op, out);
        emitFmuExpr(out, e->as.unary.operand, memberOf);
        break;
    }
    case NODE_CALL: {
        if (e->as.call.callee) emitFmuExpr(out, e->as.call.callee, memberOf);
        fputc('(', out);
        for (int i = 0; i < e->as.call.args.count; i++) {
            if (i > 0) fputs(", ", out);
            emitFmuExpr(out, e->as.call.args.items[i], memberOf);
        }
        fputc(')', out);
        break;
    }
    default:
        fputs("/* unsupported expression */", out);
        break;
    }
}

/* Source-text recovery for embedding inside a `"..."` literal in a
 * constraint failure message.  Backslashes and quotes escape.       */
static void emitFmuExprAsCString(FILE* out, const Node* e) {
    if (!e) { fputs("?", out); return; }
    switch (e->kind) {
    case NODE_LITERAL: {
        Token tk = e->as.literal.token;
        for (int i = 0; i < tk.length; i++) {
            char c = tk.start[i];
            if (c == '\\' || c == '"') fputc('\\', out);
            fputc(c, out);
        }
        break;
    }
    case NODE_QUALIFIED_NAME:
        for (int i = 0; i < e->as.qualifiedName.partCount; i++) {
            if (i > 0) fputs("::", out);
            Token tk = e->as.qualifiedName.parts[i];
            fwrite(tk.start, 1, (size_t)tk.length, out);
        }
        break;
    case NODE_BINARY:
        emitFmuExprAsCString(out, e->as.binary.left);
        fprintf(out, " %s ", binOpSysml(e->as.binary.op.type));
        emitFmuExprAsCString(out, e->as.binary.right);
        break;
    case NODE_UNARY:
        fputs(unOpSysml(e->as.unary.op.type), out);
        emitFmuExprAsCString(out, e->as.unary.operand);
        break;
    case NODE_CALL:
        if (e->as.call.callee) emitFmuExprAsCString(out, e->as.call.callee);
        fputc('(', out);
        for (int i = 0; i < e->as.call.args.count; i++) {
            if (i > 0) fputs(", ", out);
            emitFmuExprAsCString(out, e->as.call.args.items[i]);
        }
        fputc(')', out);
        break;
    default:
        fputs("?", out);
        break;
    }
}

/* Calc-def emittability — same shape as codegen_c.c.               */
static bool calcDefEmittable(const Node* def) {
    if (!def || def->kind != NODE_DEFINITION) return false;
    if (def->as.scope.defKind != DEF_CALC) return false;
    if (def->as.scope.name.length == 0)    return false;
    bool hasReturn = false;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m) continue;
        if (m->kind == NODE_USAGE) {
            if (m->as.usage.direction != DIR_IN) return false;
            if (m->as.usage.name.length == 0)    return false;
            if (!cFmiTypeName(usageResolvedType(m))) return false;
        } else if (m->kind == NODE_ATTRIBUTE) {
            if (m->as.attribute.name.length == 0) return false;
            if (!cFmiTypeName(attrTypeOrInferred(m))) return false;
            if (!m->as.attribute.defaultValue)    return false;
        } else if (m->kind == NODE_RETURN) {
            if (hasReturn) return false;
            hasReturn = true;
            if (m->as.ret.types.count != 1) return false;
            const Node* tref = m->as.ret.types.items[0];
            if (!tref || tref->kind != NODE_QUALIFIED_NAME) return false;
            if (!cFmiTypeName(tref->as.qualifiedName.resolved)) return false;
            if (!m->as.ret.defaultValue) return false;
        } else {
            return false;
        }
    }
    return hasReturn;
}

static const char* calcReturnFmiType(const Node* def) {
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_RETURN) continue;
        const Node* tref = m->as.ret.types.items[0];
        return cFmiTypeName(tref->as.qualifiedName.resolved);
    }
    return NULL;
}

static void emitCalcSignature(FILE* out, const Node* def) {
    fprintf(out, "%s %.*s(", calcReturnFmiType(def),
            def->as.scope.name.length, def->as.scope.name.start);
    bool first = true;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_USAGE) continue;
        if (m->as.usage.direction != DIR_IN) continue;
        if (!first) fputs(", ", out);
        first = false;
        const char* pty = cFmiTypeName(usageResolvedType(m));
        fprintf(out, "%s %.*s", pty,
                m->as.usage.name.length, m->as.usage.name.start);
    }
    if (first) fputs("void", out);
    fputc(')', out);
}

static void emitCalcDefPrototype(FILE* out, const Node* def) {
    emitCalcSignature(out, def);
    fputs(";\n", out);
}

static void emitCalcDefBody(FILE* out, const Node* def) {
    fputc('\n', out);
    emitCalcSignature(out, def);
    fputs(" {\n", out);
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_ATTRIBUTE) continue;
        const char* lty = cFmiTypeName(attrTypeOrInferred(m));
        fprintf(out, "    %s %.*s = ", lty,
                m->as.attribute.name.length, m->as.attribute.name.start);
        emitFmuExpr(out, m->as.attribute.defaultValue, NULL);
        fputs(";\n", out);
    }
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_RETURN) continue;
        fputs("    return ", out);
        emitFmuExpr(out, m->as.ret.defaultValue, NULL);
        fputs(";\n", out);
        break;
    }
    fputs("}\n", out);
}

/* Walk program for emittable calc defs.  Source order preserved.   */
static void collectCalcDefs(const Node* program,
                            const Node** out, int* count, int max) {
    if (!program) return;
    for (int i = 0; i < program->as.scope.memberCount; i++) {
        const Node* m = program->as.scope.members[i];
        if (!m) continue;
        if (m->kind == NODE_DEFINITION
                && m->as.scope.defKind == DEF_CALC
                && calcDefEmittable(m)) {
            if (*count < max) out[(*count)++] = m;
        } else if (m->kind == NODE_PACKAGE) {
            for (int j = 0; j < m->as.scope.memberCount; j++) {
                const Node* mm = m->as.scope.members[j];
                if (mm && mm->kind == NODE_DEFINITION
                       && mm->as.scope.defKind == DEF_CALC
                       && calcDefEmittable(mm)) {
                    if (*count < max) out[(*count)++] = mm;
                }
            }
        }
    }
}

/* Does `def` contain at least one inline `assert constraint`?       */
static bool defNeedsCheck(const Node* def) {
    if (!def || def->kind != NODE_DEFINITION) return false;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_USAGE) continue;
        if (m->as.usage.defKind != DEF_CONSTRAINT) continue;
        if (m->as.usage.assertKind != ASSERT_ASSERT) continue;
        if (m->as.usage.body) return true;
    }
    return false;
}

/* ===================================================================
 * Port usage helpers (v0.27)
 *
 * A port usage on the outer part def lowers to one or more FMI
 * variables (one per item member of the port def), grouped under a
 * single <Terminal> in TerminalsAndIcons.xml.  Conjugate ports
 * (`port p : ~PortDef`) flip the effective direction of every item.
 *
 * v0.27 scope: items with no attributes lower as fmi3Binary (one
 * variable per item).  Items with scalar attributes are deferred —
 * they'd flatten to one variable per attribute under the same
 * Terminal, but the running Sleigh-Reindeer fixture doesn't need
 * that yet.
 * =================================================================== */

static bool isPortUsageOnPartDef(const Node* m) {
    return m && m->kind == NODE_USAGE && m->as.usage.defKind == DEF_PORT;
}

static const Node* portUsagePortDef(const Node* portUsage) {
    if (!isPortUsageOnPartDef(portUsage)) return NULL;
    const NodeList* types = &portUsage->as.usage.types;
    if (types->count == 0) return NULL;
    const Node* tref = types->items[0];
    if (!tref || tref->kind != NODE_QUALIFIED_NAME) return NULL;
    return tref->as.qualifiedName.resolved;
}

static bool portUsageIsConjugated(const Node* portUsage) {
    if (!isPortUsageOnPartDef(portUsage)) return false;
    const NodeList* types = &portUsage->as.usage.types;
    if (types->count == 0) return false;
    const Node* tref = types->items[0];
    if (!tref || tref->kind != NODE_QUALIFIED_NAME) return false;
    return tref->as.qualifiedName.isConjugated;
}

/* Item def is "empty" iff zero attribute members.  An empty item is
 * a presence-only signal — it carries no scalar payload — and lowers
 * to fmi3Binary in v0.27.                                           */
static bool itemDefIsEmpty(const Node* itemDef) {
    if (!itemDef) return true;
    if (itemDef->kind != NODE_DEFINITION) return true;
    if (itemDef->as.scope.defKind != DEF_ITEM) return true;
    for (int i = 0; i < itemDef->as.scope.memberCount; i++) {
        const Node* m = itemDef->as.scope.members[i];
        if (m && m->kind == NODE_ATTRIBUTE) return false;
    }
    return true;
}

/* Resolved item def of a port-def-member item usage. */
static const Node* itemUsageItemDef(const Node* itemUsage) {
    if (!itemUsage || itemUsage->kind != NODE_USAGE) return NULL;
    if (itemUsage->as.usage.defKind != DEF_ITEM) return NULL;
    const NodeList* types = &itemUsage->as.usage.types;
    if (types->count == 0) return NULL;
    const Node* tref = types->items[0];
    if (!tref || tref->kind != NODE_QUALIFIED_NAME) return NULL;
    return tref->as.qualifiedName.resolved;
}

/* Direction → FMI causality string.                                */
static const char* directionToCausality(Direction d) {
    switch (d) {
    case DIR_IN:    return "input";
    case DIR_OUT:   return "output";
    case DIR_INOUT: return "input";   /* arbitrary but pick one for v0.27 */
    default:        return "local";
    }
}

/* Walker: visit every FMI variable contributed by `def`, in source
 * order, with a stable VR allocation starting at 1.  Each variable
 * goes through one of three callbacks; any may be NULL.
 *
 *   onParamAttr      — top-level part def attribute
 *   onPortBinary     — port-member item with no attributes (empty)
 *   onPortItemAttr   — leaf scalar attribute reachable from a port
 *                      member through 1+ levels of item-typed
 *                      attribute chaining.  attrPath is the chain
 *                      of NODE_ATTRIBUTE pointers from the port-def
 *                      member's first attribute (depth 1) down to
 *                      the leaf scalar (depth pathLen).
 *
 * Direction inheritance for onPortItemAttr: every leaf attribute
 * inherits the parent item-usage's direction with the conjugate-port
 * flip applied uniformly — nested item-typed attributes don't carry
 * a direction of their own.
 *
 * Recursion bound: FMU_MAX_NEST guards against pathologically deep
 * (or cyclic) item-def graphs.  In practice 8 is several levels of
 * headroom over realistic SysML usage.                            */
#define FMU_MAX_NEST 8

/* Helpers used by onPortItemAttr callbacks to render the chain of
 * attribute names that lead from a port-def's item member down to
 * the leaf scalar.  Two flavors: dot-joined for FMI structured
 * names (modelDescription, TerminalMemberVariable, test-driver
 * printf), underscore-joined for C identifiers (model.h field
 * names, fmu.c switch-case targets).                                */
static void emitItemPathDots(FILE* out, const Node* itemUsage,
                             const Node* const* path, int pathLen) {
    fprintf(out, "%.*s",
            itemUsage->as.usage.name.length, itemUsage->as.usage.name.start);
    for (int i = 0; i < pathLen; i++) {
        fprintf(out, ".%.*s",
                path[i]->as.attribute.name.length,
                path[i]->as.attribute.name.start);
    }
}
static void emitItemPathUnders(FILE* out, const Node* itemUsage,
                               const Node* const* path, int pathLen) {
    fprintf(out, "%.*s",
            itemUsage->as.usage.name.length, itemUsage->as.usage.name.start);
    for (int i = 0; i < pathLen; i++) {
        fprintf(out, "_%.*s",
                path[i]->as.attribute.name.length,
                path[i]->as.attribute.name.start);
    }
}

typedef struct FmuVarVisitor {
    void (*onParamAttr)(const Node* attr, int vr, void* ctx);
    void (*onPortBinary)(const Node* portUsage, const Node* itemUsage,
                         Direction effDir, int vr, void* ctx);
    void (*onPortItemAttr)(const Node* portUsage, const Node* itemUsage,
                           const Node* const* attrPath, int pathLen,
                           Direction effDir, int vr, void* ctx);
    void* ctx;
} FmuVarVisitor;

/* Forward decl for the recursion. */
static void walkItemAttrs(const Node* portUsage, const Node* itemUsage,
                          const Node* itemDef, const Node** path, int pathLen,
                          Direction d, int* vr, FmuVarVisitor* v,
                          const Node** seen, int* seenLen);

static void walkFmuVars(const Node* def, FmuVarVisitor* v) {
    if (!def || def->kind != NODE_DEFINITION || !v) return;
    int vr = 1;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m) continue;
        if (m->kind == NODE_ATTRIBUTE && attrEmittable(m)) {
            if (v->onParamAttr) v->onParamAttr(m, vr, v->ctx);
            vr++;
            continue;
        }
        if (isPortUsageOnPartDef(m)) {
            const Node* pd = portUsagePortDef(m);
            bool conj = portUsageIsConjugated(m);
            if (!pd || pd->kind != NODE_DEFINITION) continue;
            for (int j = 0; j < pd->as.scope.memberCount; j++) {
                const Node* iu = pd->as.scope.members[j];
                if (!iu || iu->kind != NODE_USAGE) continue;
                if (iu->as.usage.defKind != DEF_ITEM) continue;
                if (iu->as.usage.name.length == 0) continue;
                Direction d = iu->as.usage.direction;
                if (conj) {
                    if (d == DIR_IN)       d = DIR_OUT;
                    else if (d == DIR_OUT) d = DIR_IN;
                }
                const Node* itemDef = itemUsageItemDef(iu);
                if (itemDefIsEmpty(itemDef)) {
                    /* v0.27 path — single Binary variable. */
                    if (v->onPortBinary) v->onPortBinary(m, iu, d, vr, v->ctx);
                    vr++;
                } else {
                    /* v0.28+ path — flatten each scalar attribute under
                     * the Terminal, recursing through item-typed
                     * intermediate attributes.                          */
                    const Node* path[FMU_MAX_NEST];
                    const Node* seen[FMU_MAX_NEST];
                    int seenLen = 1;
                    seen[0] = itemDef;
                    walkItemAttrs(m, iu, itemDef, path, 0, d, &vr, v,
                                  seen, &seenLen);
                }
            }
        }
    }
}

/* Recursive walker: iterate this item def's attributes, emit leaf
 * scalars via the callback, recurse on item-typed attributes.        */
static void walkItemAttrs(const Node* portUsage, const Node* itemUsage,
                          const Node* itemDef, const Node** path, int pathLen,
                          Direction d, int* vr, FmuVarVisitor* v,
                          const Node** seen, int* seenLen) {
    if (pathLen >= FMU_MAX_NEST) return;
    for (int k = 0; k < itemDef->as.scope.memberCount; k++) {
        const Node* ia = itemDef->as.scope.members[k];
        if (!ia || ia->kind != NODE_ATTRIBUTE) continue;
        if (ia->as.attribute.name.length == 0) continue;
        const Node* t = attrResolvedType(ia);
        /* Item-typed intermediate? Recurse if it's a non-empty item
         * we haven't already entered (cycle break).                 */
        if (t && t->kind == NODE_DEFINITION
              && t->as.scope.defKind == DEF_ITEM
              && !itemDefIsEmpty(t)) {
            bool cycle = false;
            for (int s = 0; s < *seenLen; s++) {
                if (seen[s] == t) { cycle = true; break; }
            }
            if (cycle) continue;
            if (*seenLen >= FMU_MAX_NEST) continue;
            path[pathLen] = ia;
            seen[(*seenLen)++] = t;
            walkItemAttrs(portUsage, itemUsage, t, path, pathLen + 1,
                          d, vr, v, seen, seenLen);
            (*seenLen)--;
            continue;
        }
        /* Leaf scalar. */
        if (!attrEmittable(ia)) continue;
        path[pathLen] = ia;
        if (v->onPortItemAttr) {
            v->onPortItemAttr(portUsage, itemUsage, path, pathLen + 1,
                              d, *vr, v->ctx);
        }
        (*vr)++;
    }
}

/* True iff this item def has at least one emittable leaf scalar
 * reachable through any depth of item-typed-attribute chaining.
 * Cycle-safe via the same seen-set as walkItemAttrs.                */
static bool itemHasReachableScalar(const Node* itemDef,
                                   const Node** seen, int* seenLen) {
    if (!itemDef || itemDef->kind != NODE_DEFINITION) return false;
    if (itemDef->as.scope.defKind != DEF_ITEM)        return false;
    for (int k = 0; k < itemDef->as.scope.memberCount; k++) {
        const Node* ia = itemDef->as.scope.members[k];
        if (!ia || ia->kind != NODE_ATTRIBUTE) continue;
        const Node* t = attrResolvedType(ia);
        if (t && t->kind == NODE_DEFINITION && t->as.scope.defKind == DEF_ITEM) {
            bool cycle = false;
            for (int s = 0; s < *seenLen; s++) {
                if (seen[s] == t) { cycle = true; break; }
            }
            if (cycle) continue;
            if (*seenLen >= FMU_MAX_NEST) continue;
            seen[(*seenLen)++] = t;
            bool found = itemHasReachableScalar(t, seen, seenLen);
            (*seenLen)--;
            if (found) return true;
            continue;
        }
        if (attrEmittable(ia)) return true;
    }
    return false;
}

/* True iff the def has at least one port usage that contributes
 * any FMI variable — either an empty item (Binary) or a non-empty
 * item with at least one reachable scalar attribute (flattened).
 * Used by the orchestrator to decide whether to emit a non-empty
 * TerminalsAndIcons.xml.                                            */
static bool defHasPortVars(const Node* def) {
    if (!def || def->kind != NODE_DEFINITION) return false;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!isPortUsageOnPartDef(m)) continue;
        const Node* pd = portUsagePortDef(m);
        if (!pd || pd->kind != NODE_DEFINITION) continue;
        for (int j = 0; j < pd->as.scope.memberCount; j++) {
            const Node* iu = pd->as.scope.members[j];
            if (!iu || iu->kind != NODE_USAGE) continue;
            if (iu->as.usage.defKind != DEF_ITEM) continue;
            if (iu->as.usage.name.length == 0)   continue;
            const Node* it = itemUsageItemDef(iu);
            if (itemDefIsEmpty(it)) return true;
            const Node* seen[FMU_MAX_NEST];
            int seenLen = 1;
            seen[0] = it;
            if (itemHasReachableScalar(it, seen, &seenLen)) return true;
        }
    }
    return false;
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

/* Default-value rendering for the modelDescription.xml `start=`
 * attribute.  XML differs from C in two ways: booleans are "true"/
 * "false" (not fmi3True), and strings have no surrounding quotes
 * (the XML attribute provides them).
 *
 * Default-value rendering for model.c lives in emitFmuExpr above —
 * it handles the full expression grammar (literals, arithmetic,
 * calc-def calls), not just bare literals.                       */
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

/* model.h emission helpers — visitor callbacks that write VR macros
 * and struct fields for each FMI variable in source order.          */

typedef struct { FILE* out; } MhCtx;

static void mhVrAttr(const Node* attr, int vr, void* c) {
    MhCtx* x = (MhCtx*)c;
    fprintf(x->out, "#define VR_%.*s %d\n",
            attr->as.attribute.name.length,
            attr->as.attribute.name.start, vr);
}
static void mhVrPortBin(const Node* p, const Node* iu, Direction d, int vr, void* c) {
    (void)d;
    MhCtx* x = (MhCtx*)c;
    fprintf(x->out, "#define VR_%.*s_%.*s %d\n",
            p->as.usage.name.length,  p->as.usage.name.start,
            iu->as.usage.name.length, iu->as.usage.name.start, vr);
}
static void mhFieldAttr(const Node* attr, int vr, void* c) {
    (void)vr;
    MhCtx* x = (MhCtx*)c;
    const char* ct = cFmiTypeName(attrResolvedType(attr));
    fprintf(x->out, "    %-12s %.*s;\n",
            ct, attr->as.attribute.name.length, attr->as.attribute.name.start);
}
static void mhFieldPortBin(const Node* p, const Node* iu, Direction d, int vr, void* c) {
    (void)d; (void)vr;
    MhCtx* x = (MhCtx*)c;
    /* Bounded carrier: 256 bytes + size_t for the live length.     */
    fprintf(x->out, "    fmi3Byte    %.*s_%.*s_data[256];\n",
            p->as.usage.name.length,  p->as.usage.name.start,
            iu->as.usage.name.length, iu->as.usage.name.start);
    fprintf(x->out, "    size_t      %.*s_%.*s_size;\n",
            p->as.usage.name.length,  p->as.usage.name.start,
            iu->as.usage.name.length, iu->as.usage.name.start);
}
static void mhVrPortItemAttr(const Node* p, const Node* iu,
                             const Node* const* path, int pathLen,
                             Direction d, int vr, void* c) {
    (void)d;
    MhCtx* x = (MhCtx*)c;
    fprintf(x->out, "#define VR_%.*s_",
            p->as.usage.name.length, p->as.usage.name.start);
    emitItemPathUnders(x->out, iu, path, pathLen);
    fprintf(x->out, " %d\n", vr);
}
static void mhFieldPortItemAttr(const Node* p, const Node* iu,
                                const Node* const* path, int pathLen,
                                Direction d, int vr, void* c) {
    (void)d; (void)vr;
    MhCtx* x = (MhCtx*)c;
    /* Type from the leaf scalar (path[pathLen-1]). */
    const char* ct = cFmiTypeName(attrResolvedType(path[pathLen - 1]));
    fprintf(x->out, "    %-12s %.*s_", ct,
            p->as.usage.name.length, p->as.usage.name.start);
    emitItemPathUnders(x->out, iu, path, pathLen);
    fputs(";\n", x->out);
}

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
        "#include <stddef.h>\n"
        "#include \"fmi3PlatformTypes.h\"\n"
        "#include \"fmi3FunctionTypes.h\"\n"
        "\n",
        modelName);

    MhCtx ctx = { out };

    /* Value-reference macros in source order. */
    fputs("/* Value references (1-indexed, source-declaration order). */\n", out);
    {
        FmuVarVisitor v = { mhVrAttr, mhVrPortBin, mhVrPortItemAttr, &ctx };
        walkFmuVars(def, &v);
    }
    fputs("\n", out);

    /* ModelData struct. */
    fprintf(out, "typedef struct {\n");
    {
        FmuVarVisitor v = { mhFieldAttr, mhFieldPortBin, mhFieldPortItemAttr, &ctx };
        walkFmuVars(def, &v);
    }
    fputs("} ModelData;\n\n", out);

    fputs(
        "/* Set every field to its modelDescription start value.\n"
        " * Called from fmi3InstantiateCoSimulation. */\n"
        "void model_setDefaults(ModelData* m);\n"
        "\n"
        "/* Validate constraints.  Called from fmi3ExitInitializationMode.\n"
        " * Returns fmi3OK if every assert constraint holds, otherwise\n"
        " * calls the logger callback with a per-constraint failure\n"
        " * message and returns fmi3Error. */\n"
        "fmi3Status model_check(const ModelData* m,\n"
        "                       fmi3InstanceEnvironment env,\n"
        "                       fmi3LogMessageCallback   logger);\n"
        "\n"
        "#endif /* MODEL_H */\n", out);
}

/* --- src/model.c --- */

/* model_setDefaults port-init callbacks.  ctx is the FILE*; we don't
 * need a struct because we never read state back.                  */
static void scInitPortBin(const Node* p, const Node* iu, Direction d,
                          int vr, void* ctx) {
    (void)d; (void)vr;
    FILE* out = (FILE*)ctx;
    fprintf(out, "    m->%.*s_%.*s_size = 0;\n",
            p->as.usage.name.length,  p->as.usage.name.start,
            iu->as.usage.name.length, iu->as.usage.name.start);
}
static void scInitPortItemAttr(const Node* p, const Node* iu,
                               const Node* const* path, int pathLen,
                               Direction d, int vr, void* ctx) {
    (void)d; (void)vr;
    FILE* out = (FILE*)ctx;
    const Node* leaf = path[pathLen - 1];
    const Node* t = attrResolvedType(leaf);
    const char* zero =
          (t == builtinString())  ? "\"\""
        : (t == builtinBoolean()) ? "fmi3False"
        : (t == builtinInteger()) ? "0"
        :                           "0.0";
    fprintf(out, "    m->%.*s_",
            p->as.usage.name.length, p->as.usage.name.start);
    emitItemPathUnders(out, iu, path, pathLen);
    fprintf(out, " = %s;\n", zero);
}

static void emitModelC(FILE* out, const char* modelName,
                       const Node* def, const Node* program) {
    fprintf(out,
        "/* model.c — generated by sml2c for FMU '%s'.\n"
        " * Do not edit by hand. */\n"
        "#include \"model.h\"\n",
        modelName);

    /* Calc def prototypes + bodies (v0.26).  Same lowering as
     * --emit-c, but the function lives in this FMU's model.c.   */
    const Node* calcs[64];
    int calcCount = 0;
    collectCalcDefs(program, calcs, &calcCount, 64);

    if (calcCount > 0) {
        fputc('\n', out);
        for (int i = 0; i < calcCount; i++) {
            emitCalcDefPrototype(out, calcs[i]);
        }
        for (int i = 0; i < calcCount; i++) {
            emitCalcDefBody(out, calcs[i]);
        }
    }

    /* model_setDefaults — uses the full expression emitter so calc-def
     * calls and arithmetic expressions in attribute defaults lower
     * correctly.  Bare member references rewrite to `m->name`.    */
    fprintf(out, "\nvoid model_setDefaults(ModelData* m) {\n");
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* attr = def->as.scope.members[i];
        if (!attr || attr->kind != NODE_ATTRIBUTE) continue;
        if (!attrEmittable(attr)) continue;
        fprintf(out, "    m->%.*s = ",
                attr->as.attribute.name.length,
                attr->as.attribute.name.start);
        const Node* dv = attr->as.attribute.defaultValue;
        if (!dv) {
            const Node* t = attrResolvedType(attr);
            if (t == builtinString())       fputs("\"\"",      out);
            else if (t == builtinBoolean()) fputs("fmi3False", out);
            else if (t == builtinInteger()) fputs("0",         out);
            else                            fputs("0.0",       out);
        } else {
            emitFmuExpr(out, dv, def);
        }
        fputs(";\n", out);
    }
    /* Port-member init: binary carriers start with size 0 (no payload),
     * port-item attributes start at type-appropriate zero.  Routed via
     * the visitor so VR ordering stays implicit and we never reach
     * past attrEmittable here.  We don't emit anything for top-level
     * attributes in this pass — they were already done above.       */
    {
        FmuVarVisitor v = { NULL, scInitPortBin, scInitPortItemAttr, out };
        walkFmuVars(def, &v);
    }
    fputs("}\n", out);

    /* model_check — emits one early-return branch per inline assert
     * constraint.  Failures route through the FMI 3.0 logger
     * callback (not stderr) and return fmi3Error.                  */
    fprintf(out,
        "\nfmi3Status model_check(const ModelData* m,\n"
        "                       fmi3InstanceEnvironment env,\n"
        "                       fmi3LogMessageCallback   logger) {\n");
    if (!defNeedsCheck(def)) {
        fputs("    (void)m; (void)env; (void)logger;\n", out);
    } else {
        fputs("    (void)env; (void)logger;\n", out);
        for (int i = 0; i < def->as.scope.memberCount; i++) {
            const Node* mu = def->as.scope.members[i];
            if (!mu || mu->kind != NODE_USAGE) continue;
            if (mu->as.usage.defKind != DEF_CONSTRAINT) continue;
            if (mu->as.usage.assertKind != ASSERT_ASSERT) continue;
            if (!mu->as.usage.body) continue;

            fputs("    if (!(", out);
            emitFmuExpr(out, mu->as.usage.body, def);
            fputs(")) {\n", out);

            /* Logger message: `<TypeName>.<name> failed: <source>` for
             * named constraints, `<TypeName> constraint failed: ...`
             * for anonymous.  */
            fputs("        if (logger) logger(env, fmi3Error, \"logStatusError\",\n", out);
            fputs("            \"", out);
            if (mu->as.usage.name.length > 0) {
                fprintf(out, "%.*s.%.*s failed: ",
                        def->as.scope.name.length, def->as.scope.name.start,
                        mu->as.usage.name.length,    mu->as.usage.name.start);
            } else {
                fprintf(out, "%.*s constraint failed: ",
                        def->as.scope.name.length, def->as.scope.name.start);
            }
            emitFmuExprAsCString(out, mu->as.usage.body);
            fputs("\");\n", out);
            fputs("        return fmi3Error;\n", out);
            fputs("    }\n", out);
        }
    }
    fputs("    return fmi3OK;\n", out);
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

/* modelDescription.xml emission helpers.                            */
typedef struct { FILE* out; } MdCtx;

static void mdAttr(const Node* m, int vr, void* c) {
    MdCtx* x = (MdCtx*)c;
    const char* tag = fmiTypeTag(attrResolvedType(m));
    if (strcmp(tag, "String") == 0) {
        /* Strings have a child <Start value="..."/> per fmi3Variable.xsd —
         * not a `start=` attribute like the numeric types.          */
        fprintf(x->out,
            "    <String name=\"%.*s\" valueReference=\"%d\" "
            "causality=\"parameter\" variability=\"tunable\">\n"
            "      <Start value=\"",
            m->as.attribute.name.length, m->as.attribute.name.start, vr);
        emitDefaultXml(x->out, m);
        fputs("\"/>\n    </String>\n", x->out);
    } else {
        fprintf(x->out,
            "    <%s name=\"%.*s\" valueReference=\"%d\" "
            "causality=\"parameter\" variability=\"tunable\" start=\"",
            tag,
            m->as.attribute.name.length, m->as.attribute.name.start, vr);
        emitDefaultXml(x->out, m);
        fputs("\"/>\n", x->out);
    }
}
static void mdPortBin(const Node* p, const Node* iu, Direction d, int vr, void* c) {
    MdCtx* x = (MdCtx*)c;
    /* Structured naming: <port>.<member> per FMI 3.0 §2.4.3.        */
    fprintf(x->out,
        "    <Binary name=\"%.*s.%.*s\" valueReference=\"%d\" causality=\"%s\"/>\n",
        p->as.usage.name.length,  p->as.usage.name.start,
        iu->as.usage.name.length, iu->as.usage.name.start,
        vr, directionToCausality(d));
}
static void mdPortItemAttr(const Node* p, const Node* iu,
                           const Node* const* path, int pathLen,
                           Direction d, int vr, void* c) {
    MdCtx* x = (MdCtx*)c;
    const Node* leaf = path[pathLen - 1];
    const char* tag = fmiTypeTag(attrResolvedType(leaf));
    /* Structured name: <port>.<item>[.<attr>]+  Variability is
     * discrete (item attrs change at simulation events; v0.27/v0.28
     * cf. design/fmu-c-codegen.md).                                 */
    if (strcmp(tag, "String") == 0) {
        fprintf(x->out, "    <String name=\"%.*s.",
                p->as.usage.name.length, p->as.usage.name.start);
        emitItemPathDots(x->out, iu, path, pathLen);
        fprintf(x->out,
            "\" valueReference=\"%d\" causality=\"%s\" "
            "variability=\"discrete\">\n"
            "      <Start value=\"\"/>\n"
            "    </String>\n",
            vr, directionToCausality(d));
    } else {
        const char* zero =
              (strcmp(tag, "Boolean") == 0) ? "false"
            : (strcmp(tag, "Int64")   == 0) ? "0"
            :                                 "0.0";
        fprintf(x->out, "    <%s name=\"%.*s.", tag,
                p->as.usage.name.length, p->as.usage.name.start);
        emitItemPathDots(x->out, iu, path, pathLen);
        fprintf(x->out,
            "\" valueReference=\"%d\" causality=\"%s\" "
            "variability=\"discrete\" start=\"%s\"/>\n",
            vr, directionToCausality(d), zero);
    }
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

    /* Skipped-member comments are nice for traceability, so emit them
     * before the visitor does its real walk.                        */
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m) continue;
        if (m->kind == NODE_ATTRIBUTE) {
            if (!attrEmittable(m)) {
                fprintf(out, "    <!-- skipped: attribute %.*s (no kernel type / multiplicity) -->\n",
                        m->as.attribute.name.length, m->as.attribute.name.start);
            }
        } else if (m->kind == NODE_USAGE && m->as.usage.defKind == DEF_PORT) {
            /* Port — handled by visitor below; nothing to skip here. */
        } else {
            const char* what = "member";
            if (m->kind == NODE_USAGE)      what = "usage";
            if (m->kind == NODE_DEFINITION) what = "nested def";
            fprintf(out, "    <!-- skipped: %s (deferred to v0.28+) -->\n", what);
        }
    }

    MdCtx ctx = { out };
    FmuVarVisitor v = { mdAttr, mdPortBin, mdPortItemAttr, &ctx };
    walkFmuVars(def, &v);

    fputs(
        "  </ModelVariables>\n"
        "\n"
        "  <ModelStructure/>\n"
        "</fmiModelDescription>\n", out);
}

/* --- src/resources/terminalsAndIcons/TerminalsAndIcons.xml ---
 * v0.27: one <Terminal> per port usage on the outer part def; each
 * member is a flow-direction-tagged <TerminalMemberVariable>.
 *
 *   terminalKind  = "dev.sml2c.port"
 *   matchingRule  = "dev.sml2c.port.conjugate"
 *   variableKind  = "dev.sml2c.port.flow"
 *
 * Custom kinds under our own reverse-domain namespace (per FMI 3.0
 * §2.4.5: importer-recognized strings allowed as long as namespaced).
 * Importers that recognize the matching rule can auto-wire conjugate
 * pairs across two FMUs; importers that don't see plain Terminals.  */
/* ===================================================================
 * F13: connection/interface metadata
 *
 * `interface def` and `connection def` declarations in the SysML
 * source declare which port-def types are intended to wire together.
 * For each Terminal we emit, we add an <Annotations> block listing
 * every interface/connection def whose ends reference the Terminal's
 * port-def type.  Importers that recognize our annotation type can
 * use this to pair Terminals across two FMUs that share a declared
 * interface even when the matching rule is the generic conjugate.
 *
 * matchingRule itself stays as "dev.sml2c.port.conjugate" — making
 * it interface-specific would *restrict* importer matching too far
 * (Terminals from different SysML sources couldn't pair).  The
 * annotations are advisory metadata.
 * =================================================================== */

#define FMU_MAX_IFACE_REFS 32

typedef struct {
    const Node* def;        /* the interface def or connection def */
    const Node* end;        /* the NODE_USAGE inside it (DEF_END) */
    bool        isConn;     /* true for connection def, false for interface def */
} IfaceEndRef;

typedef struct {
    IfaceEndRef refs[FMU_MAX_IFACE_REFS];
    int         count;
} IfaceEndIndex;

/* Walk one container (program or package) and append matching ends. */
static void collectEndsIn(const Node* container, IfaceEndIndex* idx) {
    if (!container) return;
    for (int i = 0; i < container->as.scope.memberCount; i++) {
        const Node* m = container->as.scope.members[i];
        if (!m) continue;
        if (m->kind == NODE_PACKAGE) {
            collectEndsIn(m, idx);
            continue;
        }
        if (m->kind != NODE_DEFINITION) continue;
        DefKind dk = m->as.scope.defKind;
        if (dk != DEF_INTERFACE && dk != DEF_CONNECTION) continue;
        for (int j = 0; j < m->as.scope.memberCount; j++) {
            const Node* e = m->as.scope.members[j];
            if (!e || e->kind != NODE_USAGE) continue;
            if (e->as.usage.defKind != DEF_END) continue;
            if (e->as.usage.types.count == 0) continue;
            if (idx->count >= FMU_MAX_IFACE_REFS) return;
            idx->refs[idx->count++] = (IfaceEndRef){
                .def    = m,
                .end    = e,
                .isConn = (dk == DEF_CONNECTION),
            };
        }
    }
}

static void buildIfaceEndIndex(const Node* program, IfaceEndIndex* idx) {
    idx->count = 0;
    collectEndsIn(program, idx);
}

/* Emit one <Annotation> per interface/connection end that references
 * portDef.  Returns true if at least one annotation was emitted —
 * caller uses that to decide whether to wrap with <Annotations>...
 * </Annotations>.                                                    */
static bool emitTerminalAnnotations(FILE* out, const Node* portDef,
                                    const IfaceEndIndex* idx) {
    bool any = false;
    for (int i = 0; i < idx->count; i++) {
        const IfaceEndRef* r = &idx->refs[i];
        const Node* tref = r->end->as.usage.types.items[0];
        if (!tref || tref->kind != NODE_QUALIFIED_NAME) continue;
        if (tref->as.qualifiedName.resolved != portDef) continue;
        if (!any) {
            fputs("      <Annotations>\n", out);
            any = true;
        }
        const char* type = r->isConn ? "dev.sml2c.connection"
                                     : "dev.sml2c.interface";
        const char* tag  = r->isConn ? "ConnectionRef"
                                     : "InterfaceRef";
        fprintf(out,
            "        <Annotation type=\"%s\">\n"
            "          <%s defName=\"%.*s\" endName=\"%.*s\" conjugated=\"%s\"/>\n"
            "        </Annotation>\n",
            type, tag,
            r->def->as.scope.name.length, r->def->as.scope.name.start,
            r->end->as.usage.name.length, r->end->as.usage.name.start,
            tref->as.qualifiedName.isConjugated ? "true" : "false");
    }
    if (any) fputs("      </Annotations>\n", out);
    return any;
}

/* Recursive helper for emitTerminalsAndIcons: emit one
 * <TerminalMemberVariable> per leaf scalar reachable from `it`.
 * `path` accumulates the chain of attribute nodes from the item-def
 * member's first level down to the leaf.                            */
static void emitTerminalItemMembers(FILE* out, const Node* p, const Node* iu,
                                    const Node* it, const Node** path,
                                    int pathLen, const Node** seen, int* seenLen) {
    if (pathLen >= FMU_MAX_NEST) return;
    for (int k = 0; k < it->as.scope.memberCount; k++) {
        const Node* ia = it->as.scope.members[k];
        if (!ia || ia->kind != NODE_ATTRIBUTE) continue;
        if (ia->as.attribute.name.length == 0) continue;
        const Node* t = attrResolvedType(ia);
        if (t && t->kind == NODE_DEFINITION
              && t->as.scope.defKind == DEF_ITEM
              && !itemDefIsEmpty(t)) {
            bool cycle = false;
            for (int s = 0; s < *seenLen; s++) {
                if (seen[s] == t) { cycle = true; break; }
            }
            if (cycle) continue;
            if (*seenLen >= FMU_MAX_NEST) continue;
            path[pathLen] = ia;
            seen[(*seenLen)++] = t;
            emitTerminalItemMembers(out, p, iu, t, path, pathLen + 1,
                                    seen, seenLen);
            (*seenLen)--;
            continue;
        }
        if (!attrEmittable(ia)) continue;
        path[pathLen] = ia;
        /* variableName is `<port>.<dotted item path>`.
         * memberName is `<dotted item path>` (relative to port).    */
        fprintf(out,
            "      <TerminalMemberVariable variableName=\"%.*s.",
            p->as.usage.name.length, p->as.usage.name.start);
        emitItemPathDots(out, iu, path, pathLen + 1);
        fputs("\"\n                              memberName=\"", out);
        emitItemPathDots(out, iu, path, pathLen + 1);
        fputs("\"\n                              variableKind=\"dev.sml2c.port.flow\"/>\n", out);
    }
}

static void emitTerminalsAndIcons(FILE* out, const Node* def,
                                  const Node* program) {
    /* Build the interface/connection end index once per FMU. */
    IfaceEndIndex idx;
    buildIfaceEndIndex(program, &idx);

    fputs(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<fmiTerminalsAndIcons fmiVersion=\"3.0\">\n"
        "  <Terminals>\n", out);

    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* p = def->as.scope.members[i];
        if (!isPortUsageOnPartDef(p)) continue;
        const Node* pd = portUsagePortDef(p);
        if (!pd || pd->kind != NODE_DEFINITION) continue;
        bool conj = portUsageIsConjugated(p);

        /* Port-def name for the description string.                 */
        const char* pdName = pd->as.scope.name.start;
        int pdNameLen = pd->as.scope.name.length;

        fprintf(out,
            "    <Terminal name=\"%.*s\"\n"
            "              terminalKind=\"dev.sml2c.port\"\n"
            "              matchingRule=\"dev.sml2c.port.conjugate\"\n"
            "              description=\"Port of type %s%.*s\">\n",
            p->as.usage.name.length, p->as.usage.name.start,
            conj ? "~" : "", pdNameLen, pdName);

        for (int j = 0; j < pd->as.scope.memberCount; j++) {
            const Node* iu = pd->as.scope.members[j];
            if (!iu || iu->kind != NODE_USAGE) continue;
            if (iu->as.usage.defKind != DEF_ITEM) continue;
            if (iu->as.usage.name.length == 0)   continue;
            const Node* it = itemUsageItemDef(iu);
            if (itemDefIsEmpty(it)) {
                /* Empty item -> one Binary, one TerminalMemberVariable. */
                fprintf(out,
                    "      <TerminalMemberVariable variableName=\"%.*s.%.*s\"\n"
                    "                              memberName=\"%.*s\"\n"
                    "                              variableKind=\"dev.sml2c.port.flow\"/>\n",
                    p->as.usage.name.length,  p->as.usage.name.start,
                    iu->as.usage.name.length, iu->as.usage.name.start,
                    iu->as.usage.name.length, iu->as.usage.name.start);
            } else {
                /* Non-empty item -> one TerminalMemberVariable per
                 * emittable leaf scalar (recursively flattened
                 * through any depth of item-typed attribute chain). */
                const Node* path[FMU_MAX_NEST];
                const Node* seen[FMU_MAX_NEST];
                int seenLen = 1;
                seen[0] = it;
                emitTerminalItemMembers(out, p, iu, it, path, 0,
                                        seen, &seenLen);
            }
        }
        /* F13: emit <Annotations> listing every interface/connection
         * def whose ends reference this port-def.  Per the FMI 3.0
         * Terminal schema, Annotations come after TerminalMemberVariable. */
        emitTerminalAnnotations(out, pd, &idx);

        fputs("    </Terminal>\n", out);
    }

    fputs(
        "  </Terminals>\n"
        "</fmiTerminalsAndIcons>\n", out);
}

/* --- src/fmu.c --- */
/* The FMI 3.0 entry-point surface.  Most are stubs returning fmi3Error
 * for unsupported feature classes; the lifecycle and Float64/Int64/
 * Boolean/String getters/setters are wired through ModelData.        */

/* ---- emitFmuC_Stubs: scalar getter/setter VR routing ----------- *
 *
 * For each of Float64 / Int64 / Boolean / String, walk every FMI
 * variable in source order and emit a switch case for those whose
 * type matches.  Port-binary VRs occupy slots in the numbering but
 * don't contribute scalar cases; they're handled by the Binary
 * getter/setter in emitFmuC_Boilerplate_Bottom.                    */

typedef struct {
    FILE*       out;
    const char* matchTag;   /* "Float64" / "Int64" / "Boolean" / "String" */
    bool        isSet;      /* true for setter; affects case body */
} FmuStubCtx;

static void stubAttrCase(const Node* attr, int vr, void* c) {
    FmuStubCtx* x = (FmuStubCtx*)c;
    const char* tag = fmiTypeTag(attrResolvedType(attr));
    if (strcmp(tag, x->matchTag) != 0) return;
    if (x->isSet) {
        fprintf(x->out, "        case %d: c->data.%.*s = values[i]; break;\n",
                vr, attr->as.attribute.name.length, attr->as.attribute.name.start);
    } else {
        fprintf(x->out, "        case %d: values[i] = c->data.%.*s; break;\n",
                vr, attr->as.attribute.name.length, attr->as.attribute.name.start);
    }
}
/* Port-binary slots aren't scalar cases — visitor still increments
 * the VR counter for us by virtue of being called.  No-op here.    */
static void stubPortBinNoop(const Node* p, const Node* iu, Direction d,
                            int vr, void* c) {
    (void)p; (void)iu; (void)d; (void)vr; (void)c;
}
/* Port-item attribute -> scalar case targeting structured field name. */
static void stubPortItemAttrCase(const Node* p, const Node* iu,
                                 const Node* const* path, int pathLen,
                                 Direction d, int vr, void* c) {
    (void)d;
    FmuStubCtx* x = (FmuStubCtx*)c;
    const Node* leaf = path[pathLen - 1];
    const char* tag = fmiTypeTag(attrResolvedType(leaf));
    if (strcmp(tag, x->matchTag) != 0) return;
    fprintf(x->out, "        case %d: ", vr);
    if (x->isSet) {
        fprintf(x->out, "c->data.%.*s_",
                p->as.usage.name.length, p->as.usage.name.start);
        emitItemPathUnders(x->out, iu, path, pathLen);
        fputs(" = values[i]; break;\n", x->out);
    } else {
        fprintf(x->out, "values[i] = c->data.%.*s_",
                p->as.usage.name.length, p->as.usage.name.start);
        emitItemPathUnders(x->out, iu, path, pathLen);
        fputs("; break;\n", x->out);
    }
}

static void emitScalarGetSet(FILE* out, const Node* def, const char* tag) {
    /* Getter */
    fprintf(out,
        "fmi3Status fmi3Get%s(fmi3Instance instance,\n"
        "                          const fmi3ValueReference vr[], size_t nvr,\n"
        "                          fmi3%s values[], size_t nValues) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    Component* c = (Component*)instance;\n"
        "    if (nValues < nvr) return fmi3Error;\n"
        "    for (size_t i = 0; i < nvr; i++) {\n"
        "        switch (vr[i]) {\n", tag, tag);
    {
        FmuStubCtx ctx = { out, tag, false };
        FmuVarVisitor v = { stubAttrCase, stubPortBinNoop, stubPortItemAttrCase, &ctx };
        walkFmuVars(def, &v);
    }
    fputs(
        "        default: return fmi3Error;\n"
        "        }\n"
        "    }\n"
        "    return fmi3OK;\n"
        "}\n"
        "\n", out);

    /* Setter — String setter is a no-op error in v0.25 (no allocator); same here. */
    if (strcmp(tag, "String") == 0) {
        fputs(
            "fmi3Status fmi3SetString(fmi3Instance instance,\n"
            "                         const fmi3ValueReference vr[], size_t nvr,\n"
            "                         const fmi3String values[], size_t nValues) {\n"
            "    /* SetString is a no-op error: string ownership/copying needs\n"
            "     * a proper allocator, deferred to a later version. */\n"
            "    (void)instance; (void)vr; (void)nvr; (void)values; (void)nValues;\n"
            "    return fmi3Error;\n"
            "}\n"
            "\n", out);
        return;
    }

    fprintf(out,
        "fmi3Status fmi3Set%s(fmi3Instance instance,\n"
        "                          const fmi3ValueReference vr[], size_t nvr,\n"
        "                          const fmi3%s values[], size_t nValues) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    Component* c = (Component*)instance;\n"
        "    if (nValues < nvr) return fmi3Error;\n"
        "    for (size_t i = 0; i < nvr; i++) {\n"
        "        switch (vr[i]) {\n", tag, tag);
    {
        FmuStubCtx ctx = { out, tag, true };
        FmuVarVisitor v = { stubAttrCase, stubPortBinNoop, stubPortItemAttrCase, &ctx };
        walkFmuVars(def, &v);
    }
    fputs(
        "        default: return fmi3Error;\n"
        "        }\n"
        "    }\n"
        "    return fmi3OK;\n"
        "}\n"
        "\n", out);
}

static void emitFmuC_Stubs(FILE* out, const Node* def) {
    fputs("/* ---- Float64 getter / setter -------------------------------- */\n", out);
    emitScalarGetSet(out, def, "Float64");
    fputs("/* ---- Int64 getter / setter ---------------------------------- */\n", out);
    emitScalarGetSet(out, def, "Int64");
    fputs("/* ---- Boolean getter / setter -------------------------------- */\n", out);
    emitScalarGetSet(out, def, "Boolean");
    fputs("/* ---- String getter / setter --------------------------------- */\n", out);
    emitScalarGetSet(out, def, "String");
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
        "    /* Validate every assert constraint.  On failure, model_check\n"
        "     * has already routed a per-constraint message through the\n"
        "     * logger callback we recorded at Instantiate. */\n"
        "    fmi3Status rc = model_check(&c->data, c->instanceEnvironment,\n"
        "                                c->logMessage);\n"
        "    if (rc != fmi3OK) return rc;\n"
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

/* Binary getter/setter case-emitter callbacks for emitFmuC_Boilerplate_Bottom.
 * The visitor advances VR through both attribute slots (which we skip with
 * onParamAttr=NULL) and port-binary slots (where we emit the switch case). */
typedef struct { FILE* out; } BinCaseCtx;

static void binGetCase(const Node* p, const Node* iu, Direction d,
                       int vr, void* c) {
    (void)d;
    BinCaseCtx* x = (BinCaseCtx*)c;
    fprintf(x->out,
        "        case %d:\n"
        "            valueSizes[i] = c->data.%.*s_%.*s_size;\n"
        "            values[i]     = c->data.%.*s_%.*s_data;\n"
        "            break;\n",
        vr,
        p->as.usage.name.length,  p->as.usage.name.start,
        iu->as.usage.name.length, iu->as.usage.name.start,
        p->as.usage.name.length,  p->as.usage.name.start,
        iu->as.usage.name.length, iu->as.usage.name.start);
}

static void binSetCase(const Node* p, const Node* iu, Direction d,
                       int vr, void* c) {
    (void)d;
    BinCaseCtx* x = (BinCaseCtx*)c;
    fprintf(x->out,
        "        case %d:\n"
        "            if (sz > sizeof(c->data.%.*s_%.*s_data)) return fmi3Error;\n"
        "            if (sz > 0 && src) memcpy(c->data.%.*s_%.*s_data, src, sz);\n"
        "            c->data.%.*s_%.*s_size = sz;\n"
        "            break;\n",
        vr,
        p->as.usage.name.length,  p->as.usage.name.start,
        iu->as.usage.name.length, iu->as.usage.name.start,
        p->as.usage.name.length,  p->as.usage.name.start,
        iu->as.usage.name.length, iu->as.usage.name.start,
        p->as.usage.name.length,  p->as.usage.name.start,
        iu->as.usage.name.length, iu->as.usage.name.start);
}

/* emitFmuC_Boilerplate_Bottom is split into one helper per FMI 3.0
 * concern.  Each helper writes a self-contained block of fmu.c —
 * keeping the bottom orchestrator readable and making future edits
 * (adding a new stub, changing one signature) localizable.        */

static void emitUnsupportedScalarStubs(FILE* out) {
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
        "\n", out);
}

static void emitBinaryGetSet(FILE* out, const Node* def) {
    fputs(
        "/* Binary getter/setter — routed through ModelData for any\n"
        " * port-member binary VRs (one per empty `item def` reachable\n"
        " * from a port usage).  Unknown VRs return fmi3Error. */\n"
        "fmi3Status fmi3GetBinary(fmi3Instance instance,\n"
        "                         const fmi3ValueReference vr[], size_t nvr,\n"
        "                         size_t valueSizes[],\n"
        "                         fmi3Binary values[], size_t nValues) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    Component* c = (Component*)instance;\n"
        "    if (nValues < nvr) return fmi3Error;\n"
        "    for (size_t i = 0; i < nvr; i++) {\n"
        "        switch (vr[i]) {\n", out);
    {
        FmuVarVisitor vv = { NULL, binGetCase, NULL, &(BinCaseCtx){ out } };
        walkFmuVars(def, &vv);
    }
    fputs(
        "        default: return fmi3Error;\n"
        "        }\n"
        "    }\n"
        "    return fmi3OK;\n"
        "}\n"
        "\n"
        "fmi3Status fmi3SetBinary(fmi3Instance instance,\n"
        "                         const fmi3ValueReference vr[], size_t nvr,\n"
        "                         const size_t valueSizes[],\n"
        "                         const fmi3Binary values[], size_t nValues) {\n"
        "    EXPECT_INSTANCE(instance);\n"
        "    Component* c = (Component*)instance;\n"
        "    if (nValues < nvr) return fmi3Error;\n"
        "    for (size_t i = 0; i < nvr; i++) {\n"
        "        size_t sz = valueSizes[i];\n"
        "        const fmi3Byte* src = values[i];\n"
        "        switch (vr[i]) {\n", out);
    {
        FmuVarVisitor vv = { NULL, binSetCase, NULL, &(BinCaseCtx){ out } };
        walkFmuVars(def, &vv);
    }
    fputs(
        "        default: return fmi3Error;\n"
        "        }\n"
        "    }\n"
        "    return fmi3OK;\n"
        "}\n", out);
}

static void emitClockStubsTop(FILE* out) {
    /* Clock get/set entry points — the per-VR Clock variant.  No FMU
     * we generate has Clock VRs yet, so both stub out cleanly.      */
    fputs(
        "UNSUPPORTED_CLOCK_GET(fmi3GetClock, fmi3Clock)\n"
        "UNSUPPORTED_CLOCK_SET(fmi3SetClock, fmi3Clock)\n"
        "\n", out);
}

static void emitVariableDependencyStubs(FILE* out) {
    fputs(
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
        "\n", out);
}

static void emitFmuStateStubs(FILE* out) {
    fputs(
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
        "\n", out);
}

static void emitContinuousTimeStubs(FILE* out) {
    fputs(
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
        "\n", out);
}

static void emitDiscreteStateStubs(FILE* out) {
    fputs(
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
        "\n", out);
}

static void emitClockIntervalStubs(FILE* out) {
    fputs(
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

static void emitFmuC_Boilerplate_Bottom(FILE* out, const Node* def) {
    emitUnsupportedScalarStubs(out);
    emitBinaryGetSet(out, def);
    emitClockStubsTop(out);
    emitVariableDependencyStubs(out);
    emitFmuStateStubs(out);
    emitContinuousTimeStubs(out);
    emitDiscreteStateStubs(out);
    emitClockIntervalStubs(out);
}

static void emitFmuC(FILE* out, const char* modelName, const Node* def) {
    (void)modelName;
    emitFmuC_Boilerplate_Top(out);
    emitFmuC_Stubs(out, def);
    emitFmuC_Boilerplate_Bottom(out, def);
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

/* Test-driver visitor callbacks emit per-variable readbacks into the
 * generated test_fmu.c.  Using file-scope statics so the visitor's
 * void* ctx is just a pointer to a small TdCtx struct (no nested-fn
 * closures, which are a GCC extension we can avoid).               */
typedef struct { FILE* out; int firstFloat64Vr; } TdCtx;

static void tdAttrReadback(const Node* m, int vr, void* c0) {
    TdCtx* x = (TdCtx*)c0;
    FILE* f = x->out;
    const Node* t = attrResolvedType(m);
    const char* tag = fmiTypeTag(t);
    const char* name_start = m->as.attribute.name.start;
    int name_len = m->as.attribute.name.length;
    fprintf(f, "    {\n");
    fprintf(f, "        fmi3ValueReference vr = %d;\n", vr);
    if (strcmp(tag, "Float64") == 0 || strcmp(tag, "Int64") == 0
                                    || strcmp(tag, "Boolean") == 0) {
        const char* ctype = (strcmp(tag, "Float64") == 0) ? "fmi3Float64"
                          : (strcmp(tag, "Int64")   == 0) ? "fmi3Int64"
                                                          : "fmi3Boolean";
        fprintf(f, "        %s val = 0;\n", ctype);
        fprintf(f, "        if (fmi3Get%s(c, &vr, 1, &val, 1) != fmi3OK) {\n", tag);
        fprintf(f, "            fprintf(stderr, \"get %.*s failed\\n\"); errors++;\n",
                name_len, name_start);
        fprintf(f, "        } else {\n");
        fprintf(f, "            printf(\"%.*s = ", name_len, name_start);
        fprintf(f, (strcmp(tag, "Float64") == 0) ? "%%g\\n\", (double)val);\n"
                 : (strcmp(tag, "Int64")   == 0) ? "%%lld\\n\", (long long)val);\n"
                                                 : "%%d\\n\", (int)val);\n");
        fprintf(f, "        }\n");
        if (x->firstFloat64Vr == 0 && strcmp(tag, "Float64") == 0) {
            x->firstFloat64Vr = vr;
        }
    } else if (strcmp(tag, "String") == 0) {
        fprintf(f, "        fmi3String val = NULL;\n");
        fprintf(f, "        if (fmi3GetString(c, &vr, 1, &val, 1) != fmi3OK) {\n");
        fprintf(f, "            fprintf(stderr, \"get %.*s failed\\n\"); errors++;\n",
                name_len, name_start);
        fprintf(f, "        } else {\n");
        fprintf(f, "            printf(\"%.*s = %%s\\n\", val ? val : \"(null)\");\n",
                name_len, name_start);
        fprintf(f, "        }\n");
    }
    fprintf(f, "    }\n");
}

static void tdPortBinReadback(const Node* p, const Node* iu, Direction d,
                              int vr, void* c0) {
    (void)d;
    TdCtx* x = (TdCtx*)c0;
    FILE* f = x->out;
    fprintf(f, "    {\n");
    fprintf(f, "        fmi3ValueReference vr = %d;\n", vr);
    fprintf(f, "        size_t sz = 0;\n");
    fprintf(f, "        fmi3Binary val = NULL;\n");
    fprintf(f, "        if (fmi3GetBinary(c, &vr, 1, &sz, &val, 1) != fmi3OK) {\n");
    fprintf(f, "            fprintf(stderr, \"get %.*s.%.*s failed\\n\"); errors++;\n",
            p->as.usage.name.length,  p->as.usage.name.start,
            iu->as.usage.name.length, iu->as.usage.name.start);
    fprintf(f, "        } else {\n");
    fprintf(f, "            printf(\"%.*s.%.*s size=%%zu\\n\", sz);\n",
            p->as.usage.name.length,  p->as.usage.name.start,
            iu->as.usage.name.length, iu->as.usage.name.start);
    fprintf(f, "        }\n");
    fprintf(f, "    }\n");
}

static void tdPortItemAttrReadback(const Node* p, const Node* iu,
                                   const Node* const* path, int pathLen,
                                   Direction d, int vr, void* c0) {
    (void)d;
    TdCtx* x = (TdCtx*)c0;
    FILE* f = x->out;
    const Node* leaf = path[pathLen - 1];
    const Node* t = attrResolvedType(leaf);
    const char* tag = fmiTypeTag(t);
    /* Helper-emitted name "<port>.<item>[.<attr>]+" appears in both the
     * error path's fprintf and the success path's printf, so we'll
     * inline it both places.                                        */
    fprintf(f, "    {\n");
    fprintf(f, "        fmi3ValueReference vr = %d;\n", vr);
    if (strcmp(tag, "Float64") == 0 || strcmp(tag, "Int64") == 0
                                    || strcmp(tag, "Boolean") == 0) {
        const char* ctype = (strcmp(tag, "Float64") == 0) ? "fmi3Float64"
                          : (strcmp(tag, "Int64")   == 0) ? "fmi3Int64"
                                                          : "fmi3Boolean";
        fprintf(f, "        %s val = 0;\n", ctype);
        fprintf(f, "        if (fmi3Get%s(c, &vr, 1, &val, 1) != fmi3OK) {\n", tag);
        fprintf(f, "            fprintf(stderr, \"get %.*s.",
                p->as.usage.name.length, p->as.usage.name.start);
        emitItemPathDots(f, iu, path, pathLen);
        fprintf(f, " failed\\n\"); errors++;\n");
        fprintf(f, "        } else {\n");
        fprintf(f, "            printf(\"%.*s.",
                p->as.usage.name.length, p->as.usage.name.start);
        emitItemPathDots(f, iu, path, pathLen);
        fputs(" = ", f);
        fprintf(f, (strcmp(tag, "Float64") == 0) ? "%%g\\n\", (double)val);\n"
                 : (strcmp(tag, "Int64")   == 0) ? "%%lld\\n\", (long long)val);\n"
                                                 : "%%d\\n\", (int)val);\n");
        fprintf(f, "        }\n");
    } else if (strcmp(tag, "String") == 0) {
        fprintf(f, "        fmi3String val = NULL;\n");
        fprintf(f, "        if (fmi3GetString(c, &vr, 1, &val, 1) != fmi3OK) {\n");
        fprintf(f, "            fprintf(stderr, \"get %.*s.",
                p->as.usage.name.length, p->as.usage.name.start);
        emitItemPathDots(f, iu, path, pathLen);
        fprintf(f, " failed\\n\"); errors++;\n");
        fprintf(f, "        } else {\n");
        fprintf(f, "            printf(\"%.*s.",
                p->as.usage.name.length, p->as.usage.name.start);
        emitItemPathDots(f, iu, path, pathLen);
        fputs(" = %s\\n\", val ? val : \"(null)\");\n", f);
        fprintf(f, "        }\n");
    }
    fprintf(f, "    }\n");
}

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
        "/* Captures the most recent logger message so the violation\n"
        " * test can assert the constraint name appeared in the log. */\n"
        "static char logBuf[1024];\n"
        "static fmi3Status logStatus = fmi3OK;\n"
        "\n"
        "static void onLog(fmi3InstanceEnvironment env, fmi3Status status,\n"
        "                  fmi3String category, fmi3String message) {\n"
        "    (void)env;\n"
        "    logStatus = status;\n"
        "    if (message) snprintf(logBuf, sizeof(logBuf), \"%%s\", message);\n"
        "    fprintf(stderr, \"[%%d %%s] %%s\\n\", (int)status,\n"
        "            category ? category : \"\", message ? message : \"\");\n"
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

    /* Per-variable readbacks via the visitor — keeps VR allocation
     * lockstep with model.h and the modelDescription.xml.          */
    TdCtx td = { out, 0 };
    {
        FmuVarVisitor vv = { tdAttrReadback, tdPortBinReadback,
                             tdPortItemAttrReadback, &td };
        walkFmuVars(def, &vv);
    }
    int firstFloat64Vr = td.firstFloat64Vr;

    fputs(
        "\n"
        "    fmi3Boolean evNeed = fmi3False, term = fmi3False, early = fmi3False;\n"
        "    fmi3Float64 lastT = 0.0;\n"
        "    if (fmi3DoStep(c, 0.0, 0.01, fmi3False,\n"
        "                   &evNeed, &term, &early, &lastT) != fmi3OK) {\n"
        "        fprintf(stderr, \"DoStep failed\\n\"); errors++;\n"
        "    }\n", out);

    /* v0.26 — if the def has constraints, exercise the failure path:
     * Reset → EnterInit → set first Float64 to a hugely negative value
     * → ExitInit must return fmi3Error and the logger must have been
     * called with a non-empty message. */
    if (defNeedsCheck(def) && firstFloat64Vr > 0) {
        fprintf(out,
            "\n"
            "    /* v0.26 constraint-violation re-init.  We expect ExitInit\n"
            "     * to return fmi3Error and the logger to have received a\n"
            "     * per-constraint failure message.  We use a hugely\n"
            "     * negative value as the violator — for the FmuConstraints\n"
            "     * fixture this fails `force > 0.0`. */\n"
            "    if (fmi3Reset(c) != fmi3OK) { errors++; }\n"
            "    if (fmi3EnterInitializationMode(c, fmi3False, 0.0, 0.0,\n"
            "                                   fmi3False, 0.0) != fmi3OK) {\n"
            "        fprintf(stderr, \"re-enter init failed\\n\"); errors++;\n"
            "    }\n"
            "    {\n"
            "        fmi3ValueReference vr = %d;\n"
            "        fmi3Float64 bad = -999999.0;\n"
            "        if (fmi3SetFloat64(c, &vr, 1, &bad, 1) != fmi3OK) {\n"
            "            fprintf(stderr, \"set violator failed\\n\"); errors++;\n"
            "        }\n"
            "    }\n"
            "    logBuf[0] = '\\0';\n"
            "    fmi3Status rc = fmi3ExitInitializationMode(c);\n"
            "    if (rc == fmi3OK) {\n"
            "        fprintf(stderr,\n"
            "            \"violation NOT detected: ExitInit returned fmi3OK\\n\");\n"
            "        errors++;\n"
            "    } else {\n"
            "        if (logBuf[0] == '\\0') {\n"
            "            fprintf(stderr,\n"
            "                \"violation logger NOT called\\n\");\n"
            "            errors++;\n"
            "        } else {\n"
            "            printf(\"violation captured: %%s\\n\", logBuf);\n"
            "        }\n"
            "    }\n",
            firstFloat64Vr);
    }

    fputs(
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
    FILE* f;
    if (!(f = openOutput(outputDir, "include/model.h"))) return 1;
    emitModelH(f, modelName, def); fclose(f);
    if (!(f = openOutput(outputDir, "src/model.c"))) return 1;
    emitModelC(f, modelName, def, program); fclose(f);
    if (!(f = openOutput(outputDir, "src/fmu.c"))) return 1;
    emitFmuC(f, modelName, def); fclose(f);
    if (!(f = openOutput(outputDir, "test/test_fmu.c"))) return 1;
    emitTestFmuC(f, modelName, def); fclose(f);

    /* Files that don't need the part def AST. */
    if (!(f = openOutput(outputDir, "CMakeLists.txt"))) return 1;
    emitCMakeLists(f, modelName); fclose(f);
    if (!(f = openOutput(outputDir, "test/CMakeLists.txt"))) return 1;
    emitTestCMakeLists(f, modelName); fclose(f);
    if (!(f = openOutput(outputDir, "src/resources/modelDescription.xml"))) return 1;
    emitModelDescription(f, modelName, def); fclose(f);
    /* TerminalsAndIcons.xml is *optional* in FMI 3.0 (the XSD requires
     * at least one <Terminal> child of <Terminals>), so we only emit
     * the file when the part def actually has port-driven Terminal
     * content.  v0.25 fixtures (no ports) skip it entirely; v0.27+
     * fixtures with ports get a fully-populated file.              */
    if (defHasPortVars(def)) {
        if (!(f = openOutput(outputDir,
                "src/resources/terminalsAndIcons/TerminalsAndIcons.xml"))) return 1;
        emitTerminalsAndIcons(f, def, program); fclose(f);
    }

    fprintf(stderr, "fmu: wrote %s for part def %s\n", outputDir, modelName);
    return 0;
}
