/* sysmlc — resolver.c
 *
 * Two-phase scoped symbol table walker.  See resolver.h for purpose.
 *
 * Algorithm sketch:
 *
 *   For each construct that introduces a new scope (program, package,
 *   definition, usage-with-body):
 *
 *     phase 1 — declare every named child member in the new scope, so
 *               sibling references work regardless of source order.
 *     phase 2 — recurse into each member, resolving its references
 *               against the now-populated scope.
 *
 * Single-segment names walk the scope chain outward.  If they're not
 * found locally and any ancestor scope has at least one wildcard
 * import (`import X::*`), we accept them tentatively — they're
 * presumably from a library we don't yet load.
 *
 * Multi-segment names (A::B::C) currently resolve only their first
 * segment.  Proper member-of-type resolution requires type information
 * we don't compute yet; that's the next pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "resolver.h"

/* ---- module-level error counter --------------------------------- */

static int errorCount = 0;

static void undefinedNameError(int line, Token name) {
    fprintf(stderr, "[line %d] Error: Undefined name '%.*s'.\n",
            line, name.length, name.start);
    errorCount++;
}

static void duplicateNameError(int line, Token name, int prevLine) {
    fprintf(stderr,
            "[line %d] Error: Duplicate declaration of '%.*s' "
            "(previously declared at line %d).\n",
            line, name.length, name.start, prevLine);
    errorCount++;
}

/* ---- symbol table ----------------------------------------------- */

typedef struct Symbol {
    Token          name;     /* token from the declaration                */
    const Node*    decl;     /* the declaring node                        */
    struct Symbol* next;     /* simple linked list — small N for now      */
} Symbol;

typedef struct Scope {
    struct Scope* parent;
    Symbol*       symbols;
    /* Wildcard imports declared in this scope, e.g. `import X::*`.
     * Stored as a NodeList so we can reuse astListAppend without a
     * bespoke list type.                                               */
    NodeList      wildcardImports;
    const char*   what;      /* "program", "package", "definition", … */
} Scope;

static bool tokensEqual(Token a, Token b) {
    if (a.length != b.length) return false;
    return memcmp(a.start, b.start, (size_t)a.length) == 0;
}

static Symbol* lookupLocal(const Scope* scope, Token name) {
    for (Symbol* s = scope->symbols; s; s = s->next) {
        if (tokensEqual(s->name, name)) return s;
    }
    return NULL;
}

static const Node* lookupChain(const Scope* scope, Token name) {
    for (const Scope* s = scope; s; s = s->parent) {
        Symbol* found = lookupLocal(s, name);
        if (found) return found->decl;
    }
    return NULL;
}

static bool anyWildcardInScope(const Scope* scope) {
    for (const Scope* s = scope; s; s = s->parent) {
        if (s->wildcardImports.count > 0) return true;
    }
    return false;
}

/* Anonymous decls (length-zero name tokens) are silently skipped —
 * standalone `connect a to b;` and similar produce nameless usages. */
static void declareName(Scope* scope, Token name, const Node* decl) {
    if (name.length == 0) return;
    Symbol* prev = lookupLocal(scope, name);
    if (prev) {
        duplicateNameError(decl->line, name, prev->decl->line);
        return;
    }
    Symbol* s = (Symbol*)calloc(1, sizeof(Symbol));
    if (!s) { fprintf(stderr, "Out of memory\n"); exit(70); }
    s->name = name;
    s->decl = decl;
    s->next = scope->symbols;
    scope->symbols = s;
}

static void freeScope(Scope* scope) {
    Symbol* s = scope->symbols;
    while (s) {
        Symbol* next = s->next;
        free(s);
        s = next;
    }
    scope->symbols = NULL;
    free(scope->wildcardImports.items);
    scope->wildcardImports.items    = NULL;
    scope->wildcardImports.count    = 0;
    scope->wildcardImports.capacity = 0;
}

/* ---- name extractors -------------------------------------------- */

/* Pull the declaring name out of any node kind that introduces one.
 * Returns a length-zero token for anonymous or non-naming nodes.    */
static Token nodeName(const Node* n) {
    static const Token empty = {0};
    if (!n) return empty;
    switch (n->kind) {
    case NODE_PACKAGE:
    case NODE_DEFINITION: return n->as.scope.name;
    case NODE_USAGE:      return n->as.usage.name;
    case NODE_ATTRIBUTE:  return n->as.attribute.name;
    default:              return empty;
    }
}

/* Phase-1 pre-pass: walk a member array and declare every named
 * member in the inner scope.  Imports update wildcardImports. */
static void declareMembers(Scope* inner, Node** members, int count) {
    for (int i = 0; i < count; i++) {
        Node* m = members[i];
        if (m->kind == NODE_IMPORT) {
            if (m->as.import.wildcard) {
                astListAppend(&inner->wildcardImports, m);
            }
            /* Non-wildcard imports would bring the trailing segment
             * into scope; deferred until we have a library loader. */
            continue;
        }
        Token name = nodeName(m);
        if (name.length > 0) declareName(inner, name, m);
    }
}

/* ---- forward declarations --------------------------------------- */

static void resolveNode(Node* n, Scope* current);
static void resolveQualifiedName(Node* qname, Scope* current);
static void resolveExpression(Node* expr, Scope* current);
static void resolveNodeList(NodeList* list, Scope* current);

/* ---- resolution drivers ----------------------------------------- */

static void resolveQualifiedName(Node* qname, Scope* current) {
    if (!qname || qname->kind != NODE_QUALIFIED_NAME) return;
    if (qname->as.qualifiedName.partCount == 0) return;

    Token first = qname->as.qualifiedName.parts[0];
    const Node* found = lookupChain(current, first);
    if (found) {
        qname->as.qualifiedName.resolved = found;
        return;
    }

    /* Multi-segment names with an unknown first segment may refer to
     * a top-level package we don't yet load.  Accept tentatively. */
    if (qname->as.qualifiedName.partCount > 1) return;

    /* Single-segment fallthrough: if any wildcard import is in scope,
     * the name might come from there.  Accept tentatively. */
    if (anyWildcardInScope(current)) return;

    undefinedNameError(qname->line, first);
}

static void resolveNodeList(NodeList* list, Scope* current) {
    for (int i = 0; i < list->count; i++) {
        resolveQualifiedName(list->items[i], current);
    }
}

static void resolveExpression(Node* expr, Scope* current) {
    if (!expr) return;
    switch (expr->kind) {
    case NODE_LITERAL:
        break;
    case NODE_QUALIFIED_NAME:
        resolveQualifiedName(expr, current);
        break;
    case NODE_BINARY:
        resolveExpression(expr->as.binary.left,  current);
        resolveExpression(expr->as.binary.right, current);
        break;
    case NODE_UNARY:
        resolveExpression(expr->as.unary.operand, current);
        break;
    default:
        /* Other node kinds shouldn't appear in expression position;
         * the parser produces only the four above.                 */
        break;
    }
}

/* Recursively visit a scoped construct: declare members in inner,
 * recurse into members with inner, then tear inner down. */
static void resolveScopeBody(Scope* inner, Node** members, int count) {
    declareMembers(inner, members, count);
    for (int i = 0; i < count; i++) {
        resolveNode(members[i], inner);
    }
}

static void resolveNode(Node* n, Scope* current) {
    if (!n) return;
    switch (n->kind) {

    case NODE_PROGRAM:
        /* Members already declared by resolveProgram. */
        for (int i = 0; i < n->as.scope.memberCount; i++) {
            resolveNode(n->as.scope.members[i], current);
        }
        break;

    case NODE_PACKAGE: {
        Scope inner = { .parent = current, .what = "package" };
        resolveScopeBody(&inner, n->as.scope.members, n->as.scope.memberCount);
        freeScope(&inner);
        break;
    }

    case NODE_DEFINITION: {
        /* Spec/redef references look up in the OUTER scope — they
         * point to other top-level names, not to anything inside us. */
        resolveNodeList(&n->as.scope.specializes, current);
        resolveNodeList(&n->as.scope.redefines,   current);

        Scope inner = { .parent = current, .what = "definition" };
        resolveScopeBody(&inner, n->as.scope.members, n->as.scope.memberCount);
        freeScope(&inner);
        break;
    }

    case NODE_USAGE: {
        /* Type, spec, redef, and endpoint refs resolve in OUTER scope. */
        resolveNodeList(&n->as.usage.types,       current);
        resolveNodeList(&n->as.usage.specializes, current);
        resolveNodeList(&n->as.usage.redefines,   current);
        resolveNodeList(&n->as.usage.ends,        current);

        if (n->as.usage.memberCount > 0) {
            Scope inner = { .parent = current, .what = "usage" };
            resolveScopeBody(&inner, n->as.usage.members,
                             n->as.usage.memberCount);
            freeScope(&inner);
        }
        break;
    }

    case NODE_ATTRIBUTE:
        resolveNodeList(&n->as.attribute.types,       current);
        resolveNodeList(&n->as.attribute.specializes, current);
        resolveNodeList(&n->as.attribute.redefines,   current);
        resolveExpression(n->as.attribute.defaultValue, current);
        break;

    case NODE_IMPORT:
        /* Already handled by the parent's declareMembers pre-pass. */
        break;

    /* Leaves — no inner refs to resolve. */
    case NODE_DOC:
    case NODE_QUALIFIED_NAME:
    case NODE_MULTIPLICITY:
    case NODE_LITERAL:
    case NODE_BINARY:
    case NODE_UNARY:
        break;
    }
}

/* ---- public entry point ----------------------------------------- */

bool resolveProgram(Node* program) {
    errorCount = 0;
    if (!program) return true;

    Scope root = { .parent = NULL, .what = "program" };
    /* Declare top-level packages (and imports, though they're rare
     * outside packages) before recursing, so packages can refer to
     * each other in either source order. */
    declareMembers(&root, program->as.scope.members, program->as.scope.memberCount);
    for (int i = 0; i < program->as.scope.memberCount; i++) {
        resolveNode(program->as.scope.members[i], &root);
    }
    freeScope(&root);

    if (errorCount > 0) {
        fprintf(stderr, "Resolution failed with %d error%s.\n",
                errorCount, errorCount == 1 ? "" : "s");
        return false;
    }
    return true;
}
