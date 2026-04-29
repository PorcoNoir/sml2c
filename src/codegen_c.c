/* sysmlc — codegen_c.c
 *
 * First C-emission pass.  Uses the v0.13 visitor framework for tree
 * descent and v0.14's cached `inferredType` for expression-type
 * lookup.  See codegen_c.h for the supported subset.
 */

#include "codegen_c.h"
#include "ast_walk.h"
#include "builtin.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ---- per-walk state ------------------------------------------- */

/* Registry of definitions that we've decided to emit as C structs.
 * Built in a pre-pass (collectEmittable) before any emission so that
 * a struct's field-type lookup can see definitions that appear later
 * in source order.  The list is small (one entry per emittable def)
 * and lookups are linear by pointer identity.                      */
typedef struct {
    const Node** items;
    int          count;
    int          capacity;
} EmittableDefs;

typedef struct {
    FILE* out;
    /* True while the AST-walker is descending into a definition
     * (emittable or not).  Used to suppress duplicate field emission:
     * emittable defs emit their members during the registry-driven
     * pass, so when the visitor walk reaches the same members during
     * descent we skip them.  Non-emittable defs are already a comment
     * line, so their members shouldn't bleed out as file-scope decls.
     * The flag is set in onDefinitionEnter and cleared in
     * onDefinitionLeave regardless of emittability.                */
    bool insideAnyDef;
    /* Pre-computed set of all definitions we've decided to emit.
     * Read by both the field-type mapper (to allow user-typed fields)
     * and the definition-emit hook (to skip cleanly).              */
    EmittableDefs emittable;
} CCtx;

static bool isEmittable(const CCtx* s, const Node* def) {
    for (int i = 0; i < s->emittable.count; i++) {
        if (s->emittable.items[i] == def) return true;
    }
    return false;
}

static void registerEmittable(CCtx* s, const Node* def) {
    if (s->emittable.count == s->emittable.capacity) {
        int cap = s->emittable.capacity ? s->emittable.capacity * 2 : 8;
        s->emittable.items = realloc(s->emittable.items,
                                     (size_t)cap * sizeof(const Node*));
        s->emittable.capacity = cap;
    }
    s->emittable.items[s->emittable.count++] = def;
}

/* ---- type mapping --------------------------------------------- */

/* Map a resolved SysML type to its C equivalent.  Two flavors:
 *   - "field"  : used in struct-member declarations.  String becomes
 *                `const char*` (the field stores a pointer to a
 *                string literal).
 *   - "static" : used in `static const ...` declarations at file
 *                scope.  String becomes `char*` so that the leading
 *                `static const` from the caller naturally reads as
 *                `static const char* greeting = "hi";` instead of
 *                `static const const char*`.
 * NULL return means "no clean mapping" — caller skips with a
 * comment.                                                         */
static const char* cFieldTypeFor(const Node* type) {
    if (!type) return NULL;
    if (type == builtinReal())    return "double";
    if (type == builtinInteger()) return "long long";
    if (type == builtinBoolean()) return "bool";
    if (type == builtinString())  return "const char*";
    if (type == builtinNumber())  return "double";
    return NULL;
}

static const char* cStaticTypeFor(const Node* type) {
    if (type == builtinString()) return "char*";
    return cFieldTypeFor(type);
}

static const char* cTypeOfAttributeField(const Node* attr) {
    if (!attr || attr->kind != NODE_ATTRIBUTE) return NULL;
    const NodeList* types = &attr->as.attribute.types;
    if (types->count == 0) return NULL;
    const Node* tref = types->items[0];
    if (!tref || tref->kind != NODE_QUALIFIED_NAME) return NULL;
    return cFieldTypeFor(tref->as.qualifiedName.resolved);
}

static const char* cTypeOfAttributeStatic(const Node* attr) {
    if (!attr || attr->kind != NODE_ATTRIBUTE) return NULL;
    const NodeList* types = &attr->as.attribute.types;
    if (types->count == 0) return NULL;
    const Node* tref = types->items[0];
    if (!tref || tref->kind != NODE_QUALIFIED_NAME) return NULL;
    return cStaticTypeFor(tref->as.qualifiedName.resolved);
}

/* Multiplicity → fixed-size C array suffix.  Three states:
 *
 *   MULT_NONE       — no `[...]` on the source; emit a scalar field
 *   MULT_FIXED      — `[N]` with N >= 1; *outN is set; emit `field[N]`
 *   MULT_UNSUPPORTED — ranges, wildcards, or zero-sized; the enclosing
 *                      definition is rejected as not C-emittable so we
 *                      don't silently drop dimensionality
 *
 * The AST stores integer literal bounds as `long`; we don't yet support
 * expression bounds.                                                */
typedef enum { MULT_NONE, MULT_FIXED, MULT_UNSUPPORTED } MultKind;

static MultKind classifyMultiplicity(const Node* mult, long* outN) {
    if (!mult) return MULT_NONE;
    if (mult->kind != NODE_MULTIPLICITY) return MULT_UNSUPPORTED;
    if (mult->as.multiplicity.lowerWildcard
     || mult->as.multiplicity.upperWildcard
     || mult->as.multiplicity.isRange) return MULT_UNSUPPORTED;
    long n = mult->as.multiplicity.lower;
    if (n < 1) return MULT_UNSUPPORTED;
    if (outN) *outN = n;
    return MULT_FIXED;
}

static MultKind attributeMultiplicity(const Node* attr, long* outN) {
    if (!attr || attr->kind != NODE_ATTRIBUTE) return MULT_NONE;
    return classifyMultiplicity(attr->as.attribute.multiplicity, outN);
}

static MultKind usageMultiplicity(const Node* usage, long* outN) {
    if (!usage || usage->kind != NODE_USAGE) return MULT_NONE;
    return classifyMultiplicity(usage->as.usage.multiplicity, outN);
}

/* For a part/item usage that names another user definition as its
 * type, return the resolved NODE_DEFINITION pointer.  Used both for
 * field-type lookup and for emittability testing.  Caller doesn't own
 * the result.                                                       */
static const Node* resolvedTypeDef(const Node* usage) {
    if (!usage || usage->kind != NODE_USAGE) return NULL;
    const NodeList* types = &usage->as.usage.types;
    if (types->count == 0) return NULL;
    const Node* tref = types->items[0];
    if (!tref || tref->kind != NODE_QUALIFIED_NAME) return NULL;
    return tref->as.qualifiedName.resolved;
}

static bool usageHasEmittableType(const CCtx* s, const Node* usage) {
    const Node* td = resolvedTypeDef(usage);
    return td && isEmittable(s, td);
}

/* ---- expression printer --------------------------------------- */

static void emitExpr(CCtx* s, const Node* e);

static void emitLiteralC(CCtx* s, const Node* lit) {
    switch (lit->as.literal.litKind) {
    case LIT_INT:
        fprintf(s->out, "%lldLL", lit->as.literal.value.iv);
        break;
    case LIT_REAL: {
        /* Always print with a decimal point so the C compiler types
         * the literal as `double` rather than `int`.  `%g` may emit
         * `5` for 5.0, which would silently change inferred type
         * on the C side.                                            */
        double v = lit->as.literal.value.rv;
        char buf[64];
        snprintf(buf, sizeof buf, "%g", v);
        fputs(buf, s->out);
        if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
            fputs(".0", s->out);
        }
        break;
    }
    case LIT_BOOL:
        fputs(lit->as.literal.value.bv ? "true" : "false", s->out);
        break;
    case LIT_STRING:
        /* Token includes surrounding quotes — emit as-is; SysML and
         * C share the same string-literal syntax for the cases the
         * scanner accepts (no Unicode escapes yet).                 */
        fwrite(lit->as.literal.token.start, 1,
               (size_t)lit->as.literal.token.length, s->out);
        break;
    }
}

static void emitQNameC(CCtx* s, const Node* q) {
    /* Flat lowering: emit the LAST part of a dotted/scoped reference.
     * For first-slice expressions this matches the C scope where the
     * referenced name was emitted as a static const at file scope.
     * Cross-package references and member accesses through structs
     * are left for a later iteration.                                */
    int n = q->as.qualifiedName.partCount;
    if (n == 0) { fputs("/*?*/", s->out); return; }
    Token last = q->as.qualifiedName.parts[n - 1];
    fwrite(last.start, 1, (size_t)last.length, s->out);
}

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
    default:                   return NULL;     /* caller skips */
    }
}

static const char* unOpC(TokenType t) {
    switch (t) {
    case TOKEN_MINUS: return "-";
    case TOKEN_BANG:  return "!";
    default:          return NULL;
    }
}

static void emitExpr(CCtx* s, const Node* e) {
    if (!e) { fputs("/*?*/", s->out); return; }
    switch (e->kind) {
    case NODE_LITERAL:
        emitLiteralC(s, e);
        break;
    case NODE_QUALIFIED_NAME:
        emitQNameC(s, e);
        break;
    case NODE_BINARY: {
        const char* op = binOpC(e->as.binary.op.type);
        if (!op) { fputs("/* unsupported op */ 0", s->out); return; }
        fputc('(', s->out);
        emitExpr(s, e->as.binary.left);
        fprintf(s->out, " %s ", op);
        emitExpr(s, e->as.binary.right);
        fputc(')', s->out);
        break;
    }
    case NODE_UNARY: {
        const char* op = unOpC(e->as.unary.op.type);
        if (!op) { fputs("/* unsupported unop */ 0", s->out); return; }
        fputc('(', s->out);
        fputs(op, s->out);
        emitExpr(s, e->as.unary.operand);
        fputc(')', s->out);
        break;
    }
    default:
        /* CALL and MEMBER_ACCESS could be lowered later; for now flag
         * them so users see what's missing rather than getting silent
         * `0` placeholders.                                          */
        fputs("/* unsupported expr */ 0", s->out);
        break;
    }
}

/* ---- definitions: pre-flight check --------------------------- */

/* A definition is C-emittable iff every direct member is either an
 * attribute we can lower to a primitive C field, or a part/item usage
 * whose type is itself an already-registered emittable definition.
 * Anything else (behavioral members, calc bodies, ports, connections)
 * means the definition would be partially modeled, which is worse
 * than skipping cleanly.                                            */
static bool definitionEmittable(const CCtx* s, const Node* def) {
    if (def->kind != NODE_DEFINITION) return false;
    DefKind dk = def->as.scope.defKind;
    if (dk != DEF_PART && dk != DEF_ITEM && dk != DEF_ATTRIBUTE_DEF) return false;
    if (def->as.scope.name.length == 0) return false;
    if (def->as.scope.memberCount == 0) return false;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m) continue;
        if (m->kind == NODE_ATTRIBUTE) {
            if (m->as.attribute.name.length == 0) return false;
            if (!cTypeOfAttributeField(m)) return false;
            if (attributeMultiplicity(m, NULL) == MULT_UNSUPPORTED) return false;
        } else if (m->kind == NODE_USAGE) {
            DefKind mdk = m->as.usage.defKind;
            if (mdk != DEF_PART && mdk != DEF_ITEM) return false;
            if (m->as.usage.name.length == 0) return false;
            if (!usageHasEmittableType(s, m))   return false;
            if (usageMultiplicity(m, NULL) == MULT_UNSUPPORTED) return false;
        } else {
            return false;
        }
    }
    return true;
}

/* ---- pre-pass: collect emittable definitions ----------------- */

static void emitDefinitionFromRegistry(CCtx* s, const Node* def) {
    fputs("\ntypedef struct {\n", s->out);
    /* Emit fields by walking the def's members directly — we know
     * every member is emittable because the registry guarantees it. */
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m) continue;
        if (m->kind == NODE_ATTRIBUTE) {
            const char* cty = cTypeOfAttributeField(m);
            long n = 0;
            MultKind mk = attributeMultiplicity(m, &n);
            if (mk == MULT_FIXED) {
                fprintf(s->out, "    %s %.*s[%ld];\n", cty,
                        m->as.attribute.name.length,
                        m->as.attribute.name.start, n);
            } else {
                fprintf(s->out, "    %s %.*s;\n", cty,
                        m->as.attribute.name.length,
                        m->as.attribute.name.start);
            }
        } else if (m->kind == NODE_USAGE) {
            const Node* td = resolvedTypeDef(m);
            long n = 0;
            MultKind mk = usageMultiplicity(m, &n);
            if (mk == MULT_FIXED) {
                fprintf(s->out, "    %.*s %.*s[%ld];\n",
                        td->as.scope.name.length, td->as.scope.name.start,
                        m->as.usage.name.length,  m->as.usage.name.start, n);
            } else {
                fprintf(s->out, "    %.*s %.*s;\n",
                        td->as.scope.name.length, td->as.scope.name.start,
                        m->as.usage.name.length,  m->as.usage.name.start);
            }
        }
    }
    fprintf(s->out, "} %.*s;\n",
            def->as.scope.name.length, def->as.scope.name.start);
}

/* Registration visitor.  Walks the program once; iterated to fixed
 * point by the caller.  Registers any definition that satisfies
 * `definitionEmittable` against the current registry contents.    */
static void onCollectDefinition(AstWalkCtx* ctx, Node* n) {
    CCtx* s = ctx->userData;
    if (isEmittable(s, n)) return;          /* already registered */
    if (definitionEmittable(s, n)) {
        registerEmittable(s, n);
    }
}

/* Iterate registration to fixed point.  In practice the dependency
 * depth is shallow (Car uses Engine, Engine has primitives only) so
 * one or two passes suffice; the loop guards against degenerate
 * inputs without inspecting actual dependency edges.              */
static void collectEmittable(CCtx* s, Node* program) {
    static const AstVisitor v = { .onDefinitionEnter = onCollectDefinition };
    int prev;
    do {
        prev = s->emittable.count;
        astWalkProgram(program, &v, s);
    } while (s->emittable.count > prev);
}

/* ---- visitor hooks ------------------------------------------- */

static void onPackageBanner(AstWalkCtx* ctx, Node* n) {
    CCtx* s = ctx->userData;
    if (n->as.scope.name.length > 0) {
        fprintf(s->out, "/* package %.*s */\n",
                n->as.scope.name.length, n->as.scope.name.start);
    }
    (void)ctx;
}

static void onPackageEnter(AstWalkCtx* ctx, Node* n) {
    /* No-op in the main pass.  Package comments are emitted in the
     * one-shot banner pre-walk so they appear before the struct
     * section rather than after it.                              */
    (void)ctx;
    (void)n;
}

static void onDefinitionEnter(AstWalkCtx* ctx, Node* n) {
    CCtx* s = ctx->userData;
    s->insideAnyDef = true;
    if (!isEmittable(s, n)) {
        fprintf(s->out, "/* skipped: %.*s (not C-emittable) */\n",
                n->as.scope.name.length, n->as.scope.name.start);
    }
    /* Emittable defs were already emitted by the registry-driven
     * pass; do not re-emit here.                                  */
}

static void onDefinitionLeave(AstWalkCtx* ctx, Node* n) {
    (void)n;
    CCtx* s = ctx->userData;
    s->insideAnyDef = false;
}

static void onUsageEnter(AstWalkCtx* ctx, Node* n) {
    /* All struct-field emission happens during the registry-driven
     * pass.  The visitor walk only sees usages while descending into
     * a definition (which we want to suppress) or at the top level
     * (where bare part/item usages don't have a clean C lowering yet
     * — they're skipped silently for now).                          */
    (void)ctx;
    (void)n;
}

static void onAttribute(AstWalkCtx* ctx, Node* n) {
    CCtx* s = ctx->userData;
    /* Inside any definition — emittable or not — the registry pass
     * already handled members for emittable defs, and skip comments
     * stand in for non-emittable ones.  Either way, do nothing
     * during the visitor walk.                                      */
    if (s->insideAnyDef) return;
    if (n->as.attribute.name.length == 0) return;        /* anonymous     */
    const char* cty = cTypeOfAttributeStatic(n);
    if (!cty) {
        if (!ctx->enclosing) {
            fprintf(s->out, "/* skipped: %.*s (unsupported type) */\n",
                    n->as.attribute.name.length, n->as.attribute.name.start);
        }
        return;
    }
    /* Top-level — only emit when truly at file scope.  An attribute
     * inside a usage body would shadow / collide depending on the
     * enclosing kind, so skip silently in those cases.              */
    if (ctx->enclosing) return;
    /* Multiplicity at file scope would need a tuple-literal lowering
     * for the default value, which we don't have yet.  Skip with a
     * note so the gap is visible.                                    */
    if (attributeMultiplicity(n, NULL) != MULT_NONE) {
        fprintf(s->out, "/* skipped: %.*s (multiplicity at file scope) */\n",
                n->as.attribute.name.length, n->as.attribute.name.start);
        return;
    }

    fprintf(s->out, "static const %s %.*s",
            cty, n->as.attribute.name.length, n->as.attribute.name.start);
    if (n->as.attribute.defaultValue) {
        fputs(" = ", s->out);
        emitExpr(s, n->as.attribute.defaultValue);
    }
    fputs(";\n", s->out);
}

/* ---- entry --------------------------------------------------- */

void emitC(FILE* out, const Node* program) {
    CCtx s = {
        .out          = out,
        .insideAnyDef = false,
        .emittable    = { NULL, 0, 0 },
    };
    /* `astWalkProgram` is non-const on its first arg, but we never
     * mutate during emission.  Cast away const for both passes.    */
    Node* prog = (Node*)program;

    /* Pass 1 — register every emittable definition.  Iterated to
     * fixed point inside collectEmittable so a forward-referenced
     * type (Car declared before Engine) gets accepted on a later
     * iteration once Engine has been registered.  The registry list
     * is therefore in dependency order: every entry's field types
     * are already in the list before it.                            */
    collectEmittable(&s, prog);

    /* Header preamble — emitted directly so the registry pass can
     * follow it without going through onProgramEnter twice.        */
    fputs("/* Generated by sml2c — do not edit by hand. */\n", out);
    fputs("#include <stdbool.h>\n", out);
    fputs("#include <stdint.h>\n\n", out);

    /* Package banner — one-shot walk so the comment appears before
     * the struct section rather than after it.                     */
    static const AstVisitor banner = { .onPackageEnter = onPackageBanner };
    astWalkProgram(prog, &banner, &s);

    /* Pass 2 — emit each emittable definition in registry order
     * (which is dependency order).  Each emission is self-contained
     * because all referenced types have already been emitted.      */
    for (int i = 0; i < s.emittable.count; i++) {
        emitDefinitionFromRegistry(&s, s.emittable.items[i]);
    }

    /* Pass 3 — visitor walk for the bits the registry doesn't
     * handle: package comments, top-level static const attributes,
     * and skip comments for non-emittable definitions.             */
    static const AstVisitor v = {
        .onPackageEnter    = onPackageEnter,
        .onDefinitionEnter = onDefinitionEnter,
        .onDefinitionLeave = onDefinitionLeave,
        .onUsageEnter      = onUsageEnter,
        .onAttribute       = onAttribute,
    };
    astWalkProgram(prog, &v, &s);

    free(s.emittable.items);
}
