/* sysmlc — typechecker.c
 *
 * Bottom-up expression type inference + checking.  See
 * typechecker.h for purpose.
 *
 *   Type           ::= const Node*  (a NODE_DEFINITION; NULL = unknown)
 *   typeOf(e)      ::= the type computed for expression e
 *   specializes(a,b) ::= true iff a == b or a transitively :> b
 *   join(a,b)      ::= the more general of two numeric types
 *
 * The walker visits every NODE_ATTRIBUTE in the AST and, when the
 * attribute carries a defaultValue, checks that the expression's
 * inferred type fits the attribute's declared type.
 *
 * Side effects: errors are written to stderr; the AST is not
 * mutated.  We considered storing inferredType on each expression
 * node but the recompute cost is trivial for the sizes we handle,
 * and not mutating keeps the AST as pure parser output for now.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "typechecker.h"
#include "builtin.h"

/* ---- module state ---------------------------------------------- */

static int errorCount = 0;

/* Cached well-known datatypes — populated lazily on first call. */
static const Node* tNumber  = NULL;
static const Node* tReal    = NULL;
static const Node* tInteger = NULL;
static const Node* tBoolean = NULL;
static const Node* tString  = NULL;

static void initStdlibCache(void) {
    if (tNumber) return;
    tNumber  = builtinNumber();
    tReal    = builtinReal();
    tInteger = builtinInteger();
    tBoolean = builtinBoolean();
    tString  = builtinString();
}

/* ---- error reporting ------------------------------------------- */

/* We use a small ring of name buffers so a single fprintf can call
 * typeName() multiple times without later calls clobbering earlier
 * results.  4 slots is enough for current error messages. */
static char nameBufs[4][128];
static int  nameBufIdx = 0;

static const char* typeName(const Node* t) {
    if (!t) return "<unknown>";
    if (t->kind != NODE_DEFINITION) return "<not-a-type>";
    char* buf = nameBufs[nameBufIdx];
    nameBufIdx = (nameBufIdx + 1) % 4;
    Token n = t->as.scope.name;
    int len = (n.length < 127) ? n.length : 127;
    memcpy(buf, n.start, (size_t)len);
    buf[len] = '\0';
    return buf;
}

static void typeError(int line, const char* fmt, ...) {
    fprintf(stderr, "[line %d] Type error: ", line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    errorCount++;
}

/* ---- type relation ops ----------------------------------------- */

/* Returns true iff `a` specializes `b` (transitively).  Also true
 * when a == b.  Walks up the specializes chain with a depth bound
 * to defang accidental cycles. */
static bool specializesDepth(const Node* a, const Node* b, int depth) {
    if (!a || !b) return false;
    if (a == b)   return true;
    if (depth >= 32) return false;
    if (a->kind != NODE_DEFINITION) return false;
    for (int i = 0; i < a->as.scope.specializes.count; i++) {
        const Node* ref = a->as.scope.specializes.items[i];
        if (!ref || ref->kind != NODE_QUALIFIED_NAME) continue;
        const Node* ancestor = ref->as.qualifiedName.resolved;
        if (specializesDepth(ancestor, b, depth + 1)) return true;
    }
    return false;
}

bool specializesType(const Node* a, const Node* b) {
    return specializesDepth(a, b, 0);
}

static bool specializes(const Node* a, const Node* b) {
    return specializesType(a, b);
}

static bool isNumeric(const Node* t) {
    if (!t) return true;                 /* unknown — be permissive */
    return specializes(t, tNumber);
}

/* Combine two numeric types into the more general one — Real wins
 * over Integer, Number wins over Real.  If types are unrelated,
 * fall back to the first.                                         */
static const Node* joinNumeric(const Node* a, const Node* b) {
    if (!a) return b;
    if (!b) return a;
    if (specializes(a, b)) return b;     /* a is more specific */
    if (specializes(b, a)) return a;
    return a;                            /* unrelated, just pick one */
}

/* ---- forward declarations -------------------------------------- */

static const Node* typeOf(const Node* expr);

/* ---- per-kind typing rules ------------------------------------- */

static const Node* literalType(const Node* lit) {
    switch (lit->as.literal.litKind) {
    case LIT_INT:    return tInteger;
    case LIT_REAL:   return tReal;
    case LIT_BOOL:   return tBoolean;
    case LIT_STRING: return tString;
    }
    return NULL;
}

/* For a qualified name in expression position, the type is what
 * the resolved declaration declares.  Attributes carry types
 * directly.  Usages carry types too.  Definitions cannot appear
 * in expression position — using one would be a type error. */
static const Node* identifierType(const Node* qname) {
    const Node* decl = qname->as.qualifiedName.resolved;
    if (!decl) return NULL;              /* tentatively accepted */
    switch (decl->kind) {
    case NODE_ATTRIBUTE:
        if (decl->as.attribute.types.count > 0) {
            return decl->as.attribute.types.items[0]
                       ->as.qualifiedName.resolved;
        }
        return NULL;
    case NODE_USAGE:
        if (decl->as.usage.types.count > 0) {
            return decl->as.usage.types.items[0]
                       ->as.qualifiedName.resolved;
        }
        return NULL;
    case NODE_DEFINITION:
        /* Using a definition in value position is suspicious.  For
         * now we just say its type is unknown — the surrounding
         * context's check will catch the misuse if it matters. */
        return NULL;
    default:
        return NULL;
    }
}

static const Node* binaryType(const Node* expr) {
    const Node* l = typeOf(expr->as.binary.left);
    const Node* r = typeOf(expr->as.binary.right);
    TokenType op = expr->as.binary.op.type;
    int line = expr->as.binary.op.line;

    switch (op) {
    case TOKEN_PLUS:
    case TOKEN_MINUS:
    case TOKEN_STAR:
    case TOKEN_SLASH:
        if (!isNumeric(l) || !isNumeric(r)) {
            typeError(line,
                      "Operator '%.*s' requires numeric operands; got '%s' and '%s'.",
                      expr->as.binary.op.length, expr->as.binary.op.start,
                      typeName(l), typeName(r));
            return NULL;
        }
        if (!l || !r) return NULL;       /* unknowns propagate */
        return joinNumeric(l, r);

    case TOKEN_LESS:
    case TOKEN_LESS_EQUAL:
    case TOKEN_GREATER:
    case TOKEN_GREATER_EQUAL:
        if (!isNumeric(l) || !isNumeric(r)) {
            typeError(line,
                      "Comparison '%.*s' requires numeric operands; got '%s' and '%s'.",
                      expr->as.binary.op.length, expr->as.binary.op.start,
                      typeName(l), typeName(r));
            return tBoolean;             /* salvage so chained checks continue */
        }
        return tBoolean;

    case TOKEN_EQUAL_EQUAL:
    case TOKEN_BANG_EQUAL:
        /* Permit comparing any two types where one specializes the
         * other (or both are unknown).  Unrelated types get an
         * error.  Without this, == would accept anything. */
        if (l && r && !specializes(l, r) && !specializes(r, l)) {
            typeError(line,
                      "Cannot compare unrelated types '%s' and '%s'.",
                      typeName(l), typeName(r));
        }
        return tBoolean;

    case TOKEN_AND:
    case TOKEN_OR:
        /* Logical connectives: both operands must be boolean.  Unknowns
         * propagate without complaint.                                  */
        if (l && l != tBoolean && !specializes(l, tBoolean)) {
            typeError(line, "Operator '%.*s' requires Boolean operands; "
                            "left is '%s'.",
                      expr->as.binary.op.length, expr->as.binary.op.start,
                      typeName(l));
        }
        if (r && r != tBoolean && !specializes(r, tBoolean)) {
            typeError(line, "Operator '%.*s' requires Boolean operands; "
                            "right is '%s'.",
                      expr->as.binary.op.length, expr->as.binary.op.start,
                      typeName(r));
        }
        return tBoolean;

    default:
        return NULL;
    }
}

static const Node* unaryType(const Node* expr) {
    const Node* t = typeOf(expr->as.unary.operand);
    TokenType op = expr->as.unary.op.type;
    int line = expr->as.unary.op.line;

    switch (op) {
    case TOKEN_MINUS:
        if (!isNumeric(t)) {
            typeError(line, "Unary '-' requires a numeric operand; got '%s'.",
                      typeName(t));
            return NULL;
        }
        return t;
    case TOKEN_BANG:
        if (t && t != tBoolean && !specializes(t, tBoolean)) {
            typeError(line, "Unary '!' requires a Boolean operand; got '%s'.",
                      typeName(t));
        }
        return tBoolean;
    default:
        return NULL;
    }
}

static const Node* typeOf(const Node* expr) {
    if (!expr) return NULL;
    switch (expr->kind) {
    case NODE_LITERAL:        return literalType(expr);
    case NODE_QUALIFIED_NAME: return identifierType(expr);
    case NODE_BINARY:         return binaryType(expr);
    case NODE_UNARY:          return unaryType(expr);
    default:                  return NULL;
    }
}

/* ---- constraint / requirement body checks ---------------------- */

/* The body of a constraint def or an inline constraint usage must
 * type as Boolean.  Unknown types are tolerated (resolution may have
 * deferred a name).                                                  */
static void checkConstraintBody(const Node* body, int line, const char* what) {
    if (!body) return;
    const Node* t = typeOf(body);
    if (t && t != tBoolean && !specializes(t, tBoolean)) {
        typeError(line,
                  "%s body must be Boolean; got '%s'.",
                  what, typeName(t));
    }
}

/* Validate that a constraint usage's type ref points at a constraint
 * def (and similarly for requirement usages).  Mismatches are real
 * errors — `assert constraint c : SomePart` is meaningless.          */
static void checkAssertionRef(const Node* usage) {
    if (usage->as.usage.assertKind == ASSERT_NONE) return;
    if (usage->as.usage.types.count == 0) return;          /* anonymous, ok */
    const Node* tref = usage->as.usage.types.items[0];
    if (!tref || tref->kind != NODE_QUALIFIED_NAME) return;
    const Node* target = tref->as.qualifiedName.resolved;
    if (!target) return;                                    /* unresolved */
    if (target->kind != NODE_DEFINITION) {
        typeError(usage->line,
                  "Asserted reference must point at a constraint or "
                  "requirement def; got '%s'.", typeName(target));
        return;
    }
    DefKind want = usage->as.usage.defKind;
    DefKind got  = target->as.scope.defKind;
    if (want == DEF_CONSTRAINT && got != DEF_CONSTRAINT) {
        typeError(usage->line,
                  "'%s constraint' must reference a constraint def; "
                  "'%.*s' is a %s.",
                  usage->as.usage.assertKind == ASSERT_REQUIRE ? "require"
                : usage->as.usage.assertKind == ASSERT_ASSUME  ? "assume"
                : "assert",
                  target->as.scope.name.length,
                  target->as.scope.name.start,
                  typeName(target));
    }
    if (want == DEF_REQUIREMENT && got != DEF_REQUIREMENT) {
        typeError(usage->line,
                  "'require requirement' must reference a requirement "
                  "def; '%.*s' is a %s.",
                  target->as.scope.name.length,
                  target->as.scope.name.start,
                  typeName(target));
    }
}

/* ---- attribute default-value check ----------------------------- */

static void checkAttribute(const Node* attr, bool insideEnumDef) {
    /* Modifier semantics: `constant` features must carry a value (the
     * fixed value they're constant at), and `derived` features must
     * carry an expression (the rule by which they're derived).  Both
     * are flagged here even if the rest of the check would skip due
     * to missing default — these rules apply unconditionally.       */
    if (!attr->as.attribute.defaultValue) {
        if (attr->as.attribute.isConstant) {
            typeError(attr->line,
                      "'constant' attribute '%.*s' must have a value.",
                      attr->as.attribute.name.length,
                      attr->as.attribute.name.start);
        }
        if (attr->as.attribute.isDerived) {
            typeError(attr->line,
                      "'derived' attribute '%.*s' must have a derivation expression.",
                      attr->as.attribute.name.length,
                      attr->as.attribute.name.start);
        }
        return;
    }

    /* Compute the expression's type — sub-expression checks fire
     * along the way as a side effect. */
    const Node* valType = typeOf(attr->as.attribute.defaultValue);

    /* Look up the declared type, if any. */
    const Node* declType = NULL;
    if (attr->as.attribute.types.count > 0) {
        const Node* typeRef = attr->as.attribute.types.items[0];
        if (typeRef && typeRef->kind == NODE_QUALIFIED_NAME) {
            declType = typeRef->as.qualifiedName.resolved;
        }
    }

    /* Skip the comparison when either side is unknown.  This keeps
     * the typechecker permissive for tentatively-accepted names. */
    if (!declType || !valType) return;

    /* Inside an `enum def`, the value-attribute initializers are
     * tags (Integer/String codes), not values of the enum's type.
     * The parser stamps the enum def as the value's declared type
     * for resolution and member lookup; we relax the compatibility
     * rule here so the literal tag is accepted.  This relaxation
     * only applies to attributes declared INSIDE an enum def — a
     * user-written `attribute hue : Color = 3.14` outside one is
     * still an error.                                                */
    if (insideEnumDef
        && declType->kind == NODE_DEFINITION
        && declType->as.scope.defKind == DEF_ENUM) {
        return;
    }

    if (!specializes(valType, declType)) {
        typeError(attr->line,
                  "Default value of type '%s' cannot initialize attribute '%.*s' of type '%s'.",
                  typeName(valType),
                  attr->as.attribute.name.length,
                  attr->as.attribute.name.start,
                  typeName(declType));
    }
}

/* Validate that a succession reference points at an action usage.
 * Builtin start/done arrive pre-resolved (they're DEF_ACTION usages
 * themselves) so they pass the same check.  NULL refs are silently
 * accepted — that's the `then x;` continuation form.                 */
static void checkSuccessionRef(const Node* qname, int line) {
    if (!qname) return;
    if (qname->kind != NODE_QUALIFIED_NAME) return;
    const Node* target = qname->as.qualifiedName.resolved;
    if (!target) return;          /* tentative — let resolver speak  */
    bool isAction = false;
    if (target->kind == NODE_USAGE && target->as.usage.defKind == DEF_ACTION) {
        isAction = true;
    } else if (target->kind == NODE_DEFINITION
            && target->as.scope.defKind == DEF_ACTION) {
        isAction = true;
    }
    if (!isAction) {
        typeError(line,
                  "Succession reference must point at an action usage; "
                  "got '%s'.", typeName(target));
    }
}

/* ---- whole-program walker -------------------------------------- */

static void walk(const Node* n, bool insideEnumDef) {
    if (!n) return;
    switch (n->kind) {
    case NODE_PROGRAM:
    case NODE_PACKAGE:
        for (int i = 0; i < n->as.scope.memberCount; i++) {
            walk(n->as.scope.members[i], false);
        }
        break;
    case NODE_DEFINITION: {
        bool childContext = (n->as.scope.defKind == DEF_ENUM);
        for (int i = 0; i < n->as.scope.memberCount; i++) {
            walk(n->as.scope.members[i], childContext);
        }
        /* Constraint def body must be Boolean. */
        if (n->as.scope.defKind == DEF_CONSTRAINT) {
            checkConstraintBody(n->as.scope.body, n->line,
                                "Constraint def");
        }
        break;
    }
    case NODE_USAGE:
        for (int i = 0; i < n->as.usage.memberCount; i++) {
            walk(n->as.usage.members[i], false);
        }
        /* Inline body of an anonymous constraint usage must be Boolean. */
        if (n->as.usage.defKind == DEF_CONSTRAINT && n->as.usage.body) {
            checkConstraintBody(n->as.usage.body, n->line,
                                "Inline constraint");
        }
        /* Validate type-ref target for assert/assume/require usages. */
        checkAssertionRef(n);
        break;
    case NODE_ATTRIBUTE:
        checkAttribute(n, insideEnumDef);
        break;
    case NODE_SUCCESSION:
        checkSuccessionRef(n->as.succession.first, n->line);
        for (int i = 0; i < n->as.succession.targets.count; i++) {
            const Node* t = n->as.succession.targets.items[i];
            if (!t) continue;
            if (t->kind == NODE_QUALIFIED_NAME) {
                checkSuccessionRef(t, n->line);
            } else {
                /* Inline action declaration — recurse so its own
                 * members get checked too.                            */
                walk(t, false);
            }
        }
        break;
    default:
        break;
    }
}

/* ---- public entry ---------------------------------------------- */

bool typecheckProgram(const Node* program) {
    errorCount = 0;
    initStdlibCache();
    walk(program, false);
    if (errorCount > 0) {
        fprintf(stderr, "Type checking failed with %d error%s.\n",
                errorCount, errorCount == 1 ? "" : "s");
        return false;
    }
    return true;
}
