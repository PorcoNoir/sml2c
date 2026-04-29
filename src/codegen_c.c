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
    /* Pre-computed set of every emittable calc def — populated by
     * collectCalcDefs in a single walk after collectEmittable.  Used
     * by the prototype + body emission passes; also lets the
     * visitor's per-definition hook know which calcs were already
     * emitted by registry passes (so the descent doesn't try to
     * print them again as skipped).                                  */
    EmittableDefs calcs;
    /* Top-level attributes whose default value is a non-constant
     * expression (contains a function call).  Hoisted to non-const
     * globals + assignment in the generated `__sml2c_init` function.
     * Populated during the visitor walk in `onAttribute`; consumed
     * after the walk to emit the init function body.                */
    EmittableDefs topLevelL2;
    /* Set during T_init body emission so emitQNameC knows to prepend
     * `self->` for bare-name references that resolve to attributes of
     * `currentInitDef`.  NULL outside an init body (top-level or
     * calc-def context — in those, bare names lower to file-scope or
     * local C identifiers, not struct-field accesses).               */
    const Node* currentInitDef;
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
 *   - "field"  : used in struct-member declarations.
 *   - "static" : used in `static const ...` declarations at file
 *                scope.  Same as field for everything except String,
 *                where the leading `const` from the caller would
 *                otherwise read as `static const const char*`.
 *
 * Returns the typedef name from runtime/sml2c-runtime.h, not the raw
 * C primitive — the generated source preserves SysML's type names
 * (`Real mass`, not `double mass`) and the runtime header maps them
 * onto C primitives.  See runtime/sml2c-runtime.h for the full list.
 *
 * NULL return means "no clean mapping" — caller skips with a
 * comment.                                                         */
static const char* cFieldTypeFor(const Node* type) {
    if (!type) return NULL;
    if (type == builtinReal())    return "Real";
    if (type == builtinInteger()) return "Integer";
    if (type == builtinBoolean()) return "Boolean";
    if (type == builtinString())  return "String";
    if (type == builtinNumber())  return "Number";
    return NULL;
}

static const char* cStaticTypeFor(const Node* type) {
    /* Same mapping for both contexts now that Strings are typedef'd
     * to const char* in the runtime header — `static const String x`
     * is well-formed (parsed as `static const (const char*) x`).   */
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

/* True iff `candidate` is one of `def`'s direct members.  Used by
 * emitQNameC to detect bare-name references inside a T_init body
 * that should be rewritten with a `self->` prefix.                 */
static bool isMemberOf(const Node* def, const Node* candidate) {
    if (!def || !candidate) return false;
    if (def->kind != NODE_DEFINITION) return false;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        if (def->as.scope.members[i] == candidate) return true;
    }
    return false;
}

static void emitQNameC(CCtx* s, const Node* q) {
    /* Flat lowering: emit the LAST part of a dotted/scoped reference.
     * For first-slice expressions this matches the C scope where the
     * referenced name was emitted as a static const at file scope.
     * Cross-package references and member accesses through structs
     * are left for a later iteration.
     *
     * Special case: inside a T_init body (currentInitDef set), a
     * single-segment reference whose `resolved` decl is a member of
     * the enclosing T rewrites to `self-><name>`.  This makes
     * defaults like `area = pi*r*r` lower correctly to
     * `self->pi * self->r * self->r`. */
    int n = q->as.qualifiedName.partCount;
    if (n == 0) { fputs("/*?*/", s->out); return; }
    if (n == 1 && s->currentInitDef
              && isMemberOf(s->currentInitDef, q->as.qualifiedName.resolved)) {
        fputs("self->", s->out);
    }
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
    case NODE_CALL: {
        /* Lower function-call expressions to bare-name C calls.  Per
         * Q4 of the codegen design doc, calc-def names are emitted
         * verbatim with no package mangling — collisions are caught
         * by the C compiler as duplicate-symbol errors.  Only
         * single/multi-segment qname callees are supported; anything
         * else (computed callee, member-access callee) falls through
         * to the unsupported-expr placeholder.                       */
        const Node* callee = e->as.call.callee;
        if (!callee || callee->kind != NODE_QUALIFIED_NAME
                    || callee->as.qualifiedName.partCount == 0) {
            fputs("/* unsupported call */ 0", s->out);
            return;
        }
        emitQNameC(s, callee);
        fputc('(', s->out);
        for (int i = 0; i < e->as.call.args.count; i++) {
            if (i > 0) fputs(", ", s->out);
            emitExpr(s, e->as.call.args.items[i]);
        }
        fputc(')', s->out);
        break;
    }
    default:
        /* MEMBER_ACCESS could be lowered later (struct field reads);
         * for now flag unsupported expression forms so the user sees
         * what's missing instead of a silent `0` placeholder.         */
        fputs("/* unsupported expr */ 0", s->out);
        break;
    }
}

/* ---- calc defs --------------------------------------------------- *
 *
 * Per design/c-codegen.md §5 (v0.22): every emittable `calc def F`
 * lowers to a free C function `R F(T1 a, T2 b, …)`.  Parameters come
 * from `in`-direction usage members; the return type comes from the
 * `NODE_RETURN` member's type; the body is the return expression
 * (plus any intermediate `attribute` members declared as locals).
 *
 * Emission order: prototypes first (so mutual recursion works), then
 * bodies.  Both passes iterate the registry built by
 * `collectCalcDefs`.                                                  */

/* Registry shape mirrors EmittableDefs above. */
static bool isCalcRegistered(const CCtx* s, const Node* def) {
    for (int i = 0; i < s->calcs.count; i++) {
        if (s->calcs.items[i] == def) return true;
    }
    return false;
}

static void registerCalc(CCtx* s, const Node* def) {
    if (s->calcs.count == s->calcs.capacity) {
        int cap = s->calcs.capacity ? s->calcs.capacity * 2 : 8;
        s->calcs.items = realloc(s->calcs.items,
                                 (size_t)cap * sizeof(const Node*));
        s->calcs.capacity = cap;
    }
    s->calcs.items[s->calcs.count++] = def;
}

/* Resolve a usage's first declared type to its target node.  Returns
 * NULL if the usage has no type, the type ref isn't a qname, or
 * resolution didn't fill in `resolved`.  Used by the calc-def
 * predicate and emitter for parameter type lookup.                  */
static const Node* usageResolvedTypeNode(const Node* usage) {
    if (!usage || usage->kind != NODE_USAGE) return NULL;
    const NodeList* types = &usage->as.usage.types;
    if (types->count == 0) return NULL;
    const Node* tref = types->items[0];
    if (!tref || tref->kind != NODE_QUALIFIED_NAME) return NULL;
    return tref->as.qualifiedName.resolved;
}

/* Effective C field type for an attribute, falling back to the type
 * inferred by the typechecker if no `: T` annotation was given.
 * This makes intermediate `attribute scaled = mass * factor;` inside
 * a calc-def body work without a redundant `: Real` annotation —
 * the typechecker has already determined the type from the RHS.    */
static const char* cTypeOfAttributeFieldOrInferred(const Node* attr) {
    const char* declared = cTypeOfAttributeField(attr);
    if (declared) return declared;
    if (!attr || attr->kind != NODE_ATTRIBUTE) return NULL;
    const Node* dv = attr->as.attribute.defaultValue;
    if (!dv || !dv->inferredType) return NULL;
    return cFieldTypeFor(dv->inferredType);
}

/* True iff the calc def can be lowered cleanly.  v0.22 accepts:
 *   - DEF_CALC kind, named
 *   - exactly one NODE_RETURN with a primitive-typed return + a body
 *   - zero or more `in`-direction USAGE parameters, primitive-typed
 *   - zero or more intermediate ATTRIBUTE locals, primitive-typed,
 *     with default-value initializers
 *   - no other member kinds                                           */
static bool calcDefEmittable(const Node* def) {
    if (!def || def->kind != NODE_DEFINITION) return false;
    if (def->as.scope.defKind != DEF_CALC) return false;
    if (def->as.scope.name.length == 0)    return false;

    bool hasReturn = false;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m) continue;
        if (m->kind == NODE_USAGE) {
            if (m->as.usage.direction != DIR_IN)  return false;
            if (m->as.usage.name.length == 0)     return false;
            if (!cFieldTypeFor(usageResolvedTypeNode(m))) return false;
        } else if (m->kind == NODE_ATTRIBUTE) {
            if (m->as.attribute.name.length == 0) return false;
            if (!cTypeOfAttributeFieldOrInferred(m)) return false;
            if (!m->as.attribute.defaultValue)    return false;
        } else if (m->kind == NODE_RETURN) {
            if (hasReturn) return false;          /* multiple returns */
            hasReturn = true;
            if (m->as.ret.types.count != 1)       return false;
            const Node* tref = m->as.ret.types.items[0];
            if (!tref || tref->kind != NODE_QUALIFIED_NAME) return false;
            if (!cFieldTypeFor(tref->as.qualifiedName.resolved)) return false;
            if (!m->as.ret.defaultValue)          return false;
        } else {
            return false;
        }
    }
    return hasReturn;
}

/* Find the calc's return type's C name.  Caller has already
 * established calcDefEmittable, so this never returns NULL.         */
static const char* calcReturnCType(const Node* def) {
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_RETURN) continue;
        const Node* tref = m->as.ret.types.items[0];
        return cFieldTypeFor(tref->as.qualifiedName.resolved);
    }
    return NULL;
}

/* Emit `R F(T1 a, T2 b, …)` — the signature, no trailing punctuation.
 * Used by both the prototype and body emitters.                     */
static void emitCalcSignature(CCtx* s, const Node* def) {
    fprintf(s->out, "%s %.*s(", calcReturnCType(def),
            def->as.scope.name.length, def->as.scope.name.start);
    bool first = true;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_USAGE)  continue;
        if (m->as.usage.direction != DIR_IN) continue;
        if (!first) fputs(", ", s->out);
        first = false;
        const char* pty = cFieldTypeFor(usageResolvedTypeNode(m));
        fprintf(s->out, "%s %.*s", pty,
                m->as.usage.name.length, m->as.usage.name.start);
    }
    if (first) fputs("void", s->out);  /* zero-param: explicit (void) */
    fputc(')', s->out);
}

static void emitCalcDefPrototype(CCtx* s, const Node* def) {
    emitCalcSignature(s, def);
    fputs(";\n", s->out);
}

static void emitCalcDefBody(CCtx* s, const Node* def) {
    fputc('\n', s->out);
    emitCalcSignature(s, def);
    fputs(" {\n", s->out);
    /* Intermediate attributes become local C variables.  Source order
     * matters — a later attribute may reference an earlier one. */
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_ATTRIBUTE) continue;
        const char* lty = cTypeOfAttributeFieldOrInferred(m);
        fprintf(s->out, "    %s %.*s = ", lty,
                m->as.attribute.name.length, m->as.attribute.name.start);
        emitExpr(s, m->as.attribute.defaultValue);
        fputs(";\n", s->out);
    }
    /* Single return statement. */
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_RETURN) continue;
        fputs("    return ", s->out);
        emitExpr(s, m->as.ret.defaultValue);
        fputs(";\n", s->out);
        break;
    }
    fputs("}\n", s->out);
}

/* Walk the program collecting every emittable calc def into the
 * registry.  No fixed-point needed — calcs depend only on primitive
 * types and other calcs (which are forward-declared together).      */
static void onCollectCalc(AstWalkCtx* ctx, Node* n) {
    CCtx* s = ctx->userData;
    if (isCalcRegistered(s, n)) return;
    if (calcDefEmittable(n)) registerCalc(s, n);
}

static void collectCalcDefs(CCtx* s, Node* program) {
    static const AstVisitor v = { .onDefinitionEnter = onCollectCalc };
    astWalkProgram(program, &v, s);
}

/* True iff an expression contains any function-call subnode.  Used to
 * gate top-level `static const` emission: C doesn't allow function
 * calls in const initializers, so we hoist these to non-const
 * globals + assignment in `__sml2c_init`.                           */
static bool exprContainsCall(const Node* e) {
    if (!e) return false;
    switch (e->kind) {
    case NODE_CALL:    return true;
    case NODE_BINARY:  return exprContainsCall(e->as.binary.left)
                           || exprContainsCall(e->as.binary.right);
    case NODE_UNARY:   return exprContainsCall(e->as.unary.operand);
    default:           return false;
    }
}

/* ---- topological sort for init ordering -------------------------- *
 *
 * Both T_init (per part def) and __sml2c_init (top-level non-const
 * attributes) need to assign attributes in dependency order so an
 * initializer doesn't read an uninitialized sibling.  Build a directed
 * graph (X depends on Y iff X's default expression references Y),
 * DFS-with-three-colors, emit assignments in topo order.  Cycles
 * produce a codegen comment listing the cycle and skip the affected
 * init function.                                                    */

typedef enum { COLOR_WHITE = 0, COLOR_GRAY, COLOR_BLACK } TopoColor;

typedef struct {
    const Node** attrs;     /* attribute set we're sorting */
    int          count;
    TopoColor*   colors;
    const Node** sorted;    /* output buffer (capacity == count)   */
    int          sortedCount;
    bool         cycleFound;
} TopoState;

static int topoIndexOf(const TopoState* t, const Node* attr) {
    for (int i = 0; i < t->count; i++) {
        if (t->attrs[i] == attr) return i;
    }
    return -1;
}

static void topoVisitNode(TopoState* t, const Node* attr);

static void topoVisitDeps(TopoState* t, const Node* expr) {
    if (!expr) return;
    switch (expr->kind) {
    case NODE_QUALIFIED_NAME: {
        /* Single-segment reference to a sibling attribute creates a
         * dependency edge.  Multi-segment qnames (cross-type or
         * package-qualified) don't enter the local context's graph;
         * they reference values that are already defined elsewhere. */
        if (expr->as.qualifiedName.partCount == 1) {
            const Node* resolved = expr->as.qualifiedName.resolved;
            if (resolved && topoIndexOf(t, resolved) >= 0) {
                topoVisitNode(t, resolved);
            }
        }
        break;
    }
    case NODE_BINARY:
        topoVisitDeps(t, expr->as.binary.left);
        topoVisitDeps(t, expr->as.binary.right);
        break;
    case NODE_UNARY:
        topoVisitDeps(t, expr->as.unary.operand);
        break;
    case NODE_CALL:
        /* Calls don't depend on the callee — calc defs are forward-
         * declared at file scope before any init function runs.
         * Arguments do create dependencies on whatever they read.    */
        for (int i = 0; i < expr->as.call.args.count; i++) {
            topoVisitDeps(t, expr->as.call.args.items[i]);
        }
        break;
    default:
        break;
    }
}

static void topoVisitNode(TopoState* t, const Node* attr) {
    int idx = topoIndexOf(t, attr);
    if (idx < 0) return;
    if (t->colors[idx] == COLOR_BLACK) return;
    if (t->colors[idx] == COLOR_GRAY) {
        t->cycleFound = true;
        return;
    }
    t->colors[idx] = COLOR_GRAY;
    if (attr && attr->kind == NODE_ATTRIBUTE && attr->as.attribute.defaultValue) {
        topoVisitDeps(t, attr->as.attribute.defaultValue);
    }
    t->colors[idx] = COLOR_BLACK;
    t->sorted[t->sortedCount++] = attr;
}

/* Topo-sort `attrs[0..count-1]` in dependency order (Y before X if X
 * depends on Y).  Returns true on success and fills `*outSorted`
 * with `count` entries the caller must `free`.  Returns false on
 * cycle, with a codegen-error comment written to `s->out` for the
 * user to see in the generated source. */
static bool topoSort(CCtx* s, const Node** attrs, int count,
                     const char* contextName,
                     const Node*** outSorted) {
    if (count == 0) {
        *outSorted = NULL;
        return true;
    }
    TopoState t = {
        .attrs = attrs,
        .count = count,
        .colors = (TopoColor*)calloc((size_t)count, sizeof(TopoColor)),
        .sorted = (const Node**)malloc((size_t)count * sizeof(const Node*)),
        .sortedCount = 0,
        .cycleFound = false,
    };
    for (int i = 0; i < count && !t.cycleFound; i++) {
        if (t.colors[i] == COLOR_WHITE) {
            topoVisitNode(&t, attrs[i]);
        }
    }
    free(t.colors);
    if (t.cycleFound) {
        fprintf(s->out,
                "/* codegen error: cyclic init dependency in %s — skipped */\n",
                contextName);
        free(t.sorted);
        *outSorted = NULL;
        return false;
    }
    *outSorted = t.sorted;
    return true;
}

/* ---- T_init emission -------------------------------------------- */

/* Does this part def need a T_init function?  Yes if any of its
 * attribute members has a defaultValue, or if any of its nested
 * struct usage members has its own T_init (chained init).          */
static bool defNeedsInit(const CCtx* s, const Node* def) {
    if (!def || def->kind != NODE_DEFINITION) return false;
    if (!isEmittable(s, def)) return false;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m) continue;
        if (m->kind == NODE_ATTRIBUTE && m->as.attribute.defaultValue) {
            return true;
        }
        if (m->kind == NODE_USAGE) {
            const Node* td = resolvedTypeDef(m);
            if (defNeedsInit(s, td)) return true;
        }
    }
    return false;
}

/* Emit the prototype `void T_init(T* self);` */
static void emitInitPrototype(CCtx* s, const Node* def) {
    fprintf(s->out, "void %.*s_init(%.*s* self);\n",
            def->as.scope.name.length, def->as.scope.name.start,
            def->as.scope.name.length, def->as.scope.name.start);
}

/* Emit the body for `T_init`.  Order:
 *   1. Chain into nested struct fields' inits (so their defaults
 *      land before any sibling reads them).
 *   2. Topo-sorted scalar attribute assignments.                    */
static void emitInitBody(CCtx* s, const Node* def) {
    fputc('\n', s->out);
    fprintf(s->out, "void %.*s_init(%.*s* self) {\n",
            def->as.scope.name.length, def->as.scope.name.start,
            def->as.scope.name.length, def->as.scope.name.start);

    /* Step 1: chain into nested struct fields' inits. */
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (!m || m->kind != NODE_USAGE) continue;
        const Node* td = resolvedTypeDef(m);
        if (!defNeedsInit(s, td)) continue;
        long n = 0;
        MultKind mk = usageMultiplicity(m, &n);
        if (mk == MULT_FIXED) {
            fprintf(s->out, "    for (long _i = 0; _i < %ld; _i++) {\n", n);
            fprintf(s->out, "        %.*s_init(&self->%.*s[_i]);\n",
                    td->as.scope.name.length, td->as.scope.name.start,
                    m->as.usage.name.length,  m->as.usage.name.start);
            fprintf(s->out, "    }\n");
        } else {
            fprintf(s->out, "    %.*s_init(&self->%.*s);\n",
                    td->as.scope.name.length, td->as.scope.name.start,
                    m->as.usage.name.length,  m->as.usage.name.start);
        }
    }

    /* Step 2: topo-sort scalar attribute assignments. */
    int attrCount = 0;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (m && m->kind == NODE_ATTRIBUTE && m->as.attribute.defaultValue) {
            attrCount++;
        }
    }
    if (attrCount > 0) {
        const Node** attrs = (const Node**)malloc((size_t)attrCount * sizeof(const Node*));
        int j = 0;
        for (int i = 0; i < def->as.scope.memberCount; i++) {
            const Node* m = def->as.scope.members[i];
            if (m && m->kind == NODE_ATTRIBUTE && m->as.attribute.defaultValue) {
                attrs[j++] = m;
            }
        }
        const Node** sorted = NULL;
        char ctx[128];
        snprintf(ctx, sizeof ctx, "%.*s_init",
                 def->as.scope.name.length, def->as.scope.name.start);
        if (topoSort(s, attrs, attrCount, ctx, &sorted)) {
            s->currentInitDef = def;
            for (int i = 0; i < attrCount; i++) {
                const Node* m = sorted[i];
                fprintf(s->out, "    self->%.*s = ",
                        m->as.attribute.name.length, m->as.attribute.name.start);
                emitExpr(s, m->as.attribute.defaultValue);
                fputs(";\n", s->out);
            }
            s->currentInitDef = NULL;
            free(sorted);
        }
        free(attrs);
    }

    fputs("}\n", s->out);
}

/* ---- __sml2c_init emission --------------------------------------- *
 *
 * Top-level non-const attributes (those whose default contains a
 * call, can't lower to `static const`) are stored in a registry
 * during the visitor walk.  After the walk, this function emits a
 * `void __sml2c_init(void)` whose body assigns each one in
 * topo-sorted order.                                                */
static void emitProgramInit(CCtx* s) {
    if (s->topLevelL2.count == 0) return;

    fputs("\nvoid __sml2c_init(void) {\n", s->out);

    const Node** attrs = (const Node**)s->topLevelL2.items;
    const Node** sorted = NULL;
    if (topoSort(s, attrs, s->topLevelL2.count, "__sml2c_init", &sorted)) {
        for (int i = 0; i < s->topLevelL2.count; i++) {
            const Node* m = sorted[i];
            fprintf(s->out, "    %.*s = ",
                    m->as.attribute.name.length, m->as.attribute.name.start);
            emitExpr(s, m->as.attribute.defaultValue);
            fputs(";\n", s->out);
        }
        free(sorted);
    }

    fputs("}\n", s->out);
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
    if (isEmittable(s, n) || isCalcRegistered(s, n)) {
        /* Already emitted in an earlier pass — struct registry or
         * calc-def registry.  Don't re-emit and don't print a skip
         * comment. */
        return;
    }
    fprintf(s->out, "/* skipped: %.*s (not C-emittable) */\n",
            n->as.scope.name.length, n->as.scope.name.start);
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
    /* L2: top-level attribute whose default value contains a function
     * call.  C doesn't allow function calls in `static const`
     * initializers, so we hoist these to non-const globals and queue
     * them for assignment in the generated `__sml2c_init`.  The
     * declaration is emitted here in source order; the assignment
     * order is decided by topoSort after the visitor walk so that an
     * L2 attribute that depends on another L2 sibling reads it after
     * it's been initialized.                                          */
    if (n->as.attribute.defaultValue
     && exprContainsCall(n->as.attribute.defaultValue)) {
        fprintf(s->out, "%s %.*s;\n", cty,
                n->as.attribute.name.length, n->as.attribute.name.start);
        if (s->topLevelL2.count == s->topLevelL2.capacity) {
            int cap = s->topLevelL2.capacity ? s->topLevelL2.capacity * 2 : 8;
            s->topLevelL2.items = realloc(s->topLevelL2.items,
                                          (size_t)cap * sizeof(const Node*));
            s->topLevelL2.capacity = cap;
        }
        s->topLevelL2.items[s->topLevelL2.count++] = n;
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
        .out             = out,
        .insideAnyDef    = false,
        .emittable       = { NULL, 0, 0 },
        .calcs           = { NULL, 0, 0 },
        .topLevelL2      = { NULL, 0, 0 },
        .currentInitDef  = NULL,
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

    /* Pass 1.5 — register every emittable calc def. */
    collectCalcDefs(&s, prog);

    /* Header preamble.  The runtime header brings in the kernel + ISQ
     * typedefs (Real, Integer, Boolean, String, MassValue, …) plus
     * stdbool/stdint transitively.                                  */
    fputs("/* Generated by sml2c — do not edit by hand. */\n", out);
    fputs("#include \"sml2c-runtime.h\"\n\n", out);

    /* Package banner — one-shot walk so the comment appears before
     * the struct section rather than after it.                     */
    static const AstVisitor banner = { .onPackageEnter = onPackageBanner };
    astWalkProgram(prog, &banner, &s);

    /* Pass 2a — calc def prototypes (forward declarations). */
    if (s.calcs.count > 0) fputc('\n', out);
    for (int i = 0; i < s.calcs.count; i++) {
        emitCalcDefPrototype(&s, s.calcs.items[i]);
    }

    /* Pass 2b — emit each emittable struct definition in registry
     * order (dependency order).                                    */
    for (int i = 0; i < s.emittable.count; i++) {
        emitDefinitionFromRegistry(&s, s.emittable.items[i]);
    }

    /* Pass 2c — T_init prototypes for every struct that needs one.
     * Forward-declared together so a T_init that calls a sibling
     * T_init (e.g. nested struct with its own defaults) compiles. */
    bool anyInit = false;
    for (int i = 0; i < s.emittable.count; i++) {
        if (defNeedsInit(&s, s.emittable.items[i])) anyInit = true;
    }
    if (anyInit) fputc('\n', out);
    for (int i = 0; i < s.emittable.count; i++) {
        if (defNeedsInit(&s, s.emittable.items[i])) {
            emitInitPrototype(&s, s.emittable.items[i]);
        }
    }
    if (s.topLevelL2.count > 0 || anyInit) {
        /* Forward-declare __sml2c_init so user code can call it
         * before its definition appears (which is at end of file). */
    }

    /* Pass 2d — calc def bodies. */
    for (int i = 0; i < s.calcs.count; i++) {
        emitCalcDefBody(&s, s.calcs.items[i]);
    }

    /* Pass 2e — T_init bodies. */
    for (int i = 0; i < s.emittable.count; i++) {
        if (defNeedsInit(&s, s.emittable.items[i])) {
            emitInitBody(&s, s.emittable.items[i]);
        }
    }

    /* Pass 3 — visitor walk for the bits the registry doesn't
     * handle: package comments, top-level static const attributes
     * (L1) and non-const global declarations (L2), and skip comments
     * for non-emittable definitions.                                */
    static const AstVisitor v = {
        .onPackageEnter    = onPackageEnter,
        .onDefinitionEnter = onDefinitionEnter,
        .onDefinitionLeave = onDefinitionLeave,
        .onUsageEnter      = onUsageEnter,
        .onAttribute       = onAttribute,
    };
    astWalkProgram(prog, &v, &s);

    /* Pass 4 — `__sml2c_init` body, if any L2 attributes were
     * collected during the visitor walk. */
    emitProgramInit(&s);

    free(s.emittable.items);
    free(s.calcs.items);
    free(s.topLevelL2.items);
}
