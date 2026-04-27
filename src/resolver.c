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
#include "builtin.h"

/* ---- module-level error counter --------------------------------- */

static int errorCount = 0;

static void undefinedNameError(int line, Token name) {
    fprintf(stderr, "[line %d] Error: Undefined name '%.*s'.\n",
            line, name.length, name.start);
    errorCount++;
}

/* Forward declaration: defined later, but needed by undefinedMemberError
 * to print the container's name in the error message. */
static Token nodeName(const Node* n);

static void undefinedMemberError(int line, Token name, const Node* container) {
    Token cname = nodeName(container);
    if (cname.length > 0) {
        fprintf(stderr, "[line %d] Error: '%.*s' has no member '%.*s'.\n",
                line, cname.length, cname.start,
                name.length, name.start);
    } else {
        fprintf(stderr, "[line %d] Error: No member '%.*s' in container.\n",
                line, name.length, name.start);
    }
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
 * member in the inner scope.  Imports update wildcardImports and
 * eagerly resolve their target so wildcard search works. */
static void declareMembers(Scope* inner, Node** members, int count);

/* ---- forward declarations --------------------------------------- */

static void resolveNode(Node* n, Scope* current);
static void resolveQualifiedName(Node* qname, Scope* current);
static void resolveExpression(Node* expr, Scope* current);
static void resolveNodeList(NodeList* list, Scope* current);

/* Variant that doesn't report errors on failure — used for import
 * targets, where we tolerate "unknown library" and just leave the
 * resolution NULL.                                                  */
static void tryResolveQualifiedName(Node* qname, Scope* current);

/* ---- declareMembers (defined here so it can call try-resolve) --- */

static void declareMembers(Scope* inner, Node** members, int count) {
    for (int i = 0; i < count; i++) {
        Node* m = members[i];
        if (m->kind == NODE_IMPORT) {
            /* Resolve the target package now, against the OUTER scope:
             * import targets refer to siblings of our containing
             * scope, not to anything inside us.                       */
            if (m->as.import.target) {
                Scope* outer = inner->parent ? inner->parent : inner;
                tryResolveQualifiedName(m->as.import.target, outer);
            }
            if (m->as.import.wildcard) {
                astListAppend(&inner->wildcardImports, m);
            }
            /* Non-wildcard imports would bring the trailing segment
             * into scope; deferred until we add aliasing support. */
            continue;
        }
        Token name = nodeName(m);
        if (name.length > 0) declareName(inner, name, m);
    }
}

/* ---- member-of-type lookup ------------------------------------- *
 *
 * Given a container node, look up a name as one of its (possibly
 * inherited) members.  The walk shape depends on the container kind:
 *
 *   PACKAGE / PROGRAM : direct members only.
 *   DEFINITION        : direct members, then recurse through the
 *                       specializes chain.
 *   USAGE             : direct body members, then through the types
 *                       (`: T`) chain, then through the usage's own
 *                       specializes (`:>` on a usage).
 *   ATTRIBUTE         : navigate through the attribute's type
 *                       (so `attribute x : T` lets you reach T's
 *                        members via `x.something`).
 *
 * Cycles in the inheritance graph are defanged with a depth bound. */

static const Node* lookupMemberDepth(const Node* container, Token name, int depth);

/* Walk a list of supertype/type references, calling lookupMemberDepth
 * on each one's resolved target. */
static const Node* searchInheritedMembers(const NodeList* list, Token name, int depth) {
    if (depth >= 32) return NULL;
    for (int i = 0; i < list->count; i++) {
        const Node* ref = list->items[i];
        if (!ref || ref->kind != NODE_QUALIFIED_NAME) continue;
        const Node* ancestor = ref->as.qualifiedName.resolved;
        if (!ancestor) continue;
        const Node* found = lookupMemberDepth(ancestor, name, depth + 1);
        if (found) return found;
    }
    return NULL;
}

static const Node* lookupMemberDepth(const Node* container, Token name, int depth) {
    if (!container || depth >= 32) return NULL;

    /* Step 1 — look at the container's own direct members, if any. */
    Node** members = NULL;
    int    memberCount = 0;
    switch (container->kind) {
    case NODE_PROGRAM:
    case NODE_PACKAGE:
    case NODE_DEFINITION:
        members = container->as.scope.members;
        memberCount = container->as.scope.memberCount;
        break;
    case NODE_USAGE:
        members = container->as.usage.members;
        memberCount = container->as.usage.memberCount;
        break;
    default:
        break;     /* attributes, qualified names, etc. have no direct members */
    }
    for (int i = 0; i < memberCount; i++) {
        Node* m = members[i];
        Token mName = nodeName(m);
        if (mName.length > 0 && tokensEqual(mName, name)) return m;
    }

    /* Step 2 — follow inheritance / type links into other containers. */
    switch (container->kind) {
    case NODE_DEFINITION:
        return searchInheritedMembers(&container->as.scope.specializes, name, depth);
    case NODE_USAGE: {
        const Node* r = searchInheritedMembers(&container->as.usage.types, name, depth);
        if (r) return r;
        return searchInheritedMembers(&container->as.usage.specializes, name, depth);
    }
    case NODE_ATTRIBUTE:
        return searchInheritedMembers(&container->as.attribute.types, name, depth);
    default:
        return NULL;
    }
}

const Node* lookupMember(const Node* container, Token name) {
    return lookupMemberDepth(container, name, 0);
}

/* ---- wildcard import search ------------------------------------- */

/* Look up `name` in any wildcard-imported package whose target was
 * successfully resolved.  Returns the matching member or NULL.    */
static const Node* searchWildcardImports(const Scope* scope, Token name) {
    for (const Scope* s = scope; s; s = s->parent) {
        for (int i = 0; i < s->wildcardImports.count; i++) {
            const Node* imp = s->wildcardImports.items[i];
            if (!imp || !imp->as.import.target) continue;
            const Node* target = imp->as.import.target->as.qualifiedName.resolved;
            if (!target || target->kind != NODE_PACKAGE) continue;
            for (int j = 0; j < target->as.scope.memberCount; j++) {
                Node* mem = target->as.scope.members[j];
                Token memName = nodeName(mem);
                if (memName.length > 0 && tokensEqual(memName, name)) {
                    return mem;
                }
            }
        }
    }
    return NULL;
}

/* True if any wildcard import in the scope chain has an UNRESOLVED
 * target — meaning we don't know what symbols it brings in, so we
 * should be permissive about names that might come from there.    */
static bool anyUnresolvedWildcardInScope(const Scope* scope) {
    for (const Scope* s = scope; s; s = s->parent) {
        for (int i = 0; i < s->wildcardImports.count; i++) {
            const Node* imp = s->wildcardImports.items[i];
            if (!imp || !imp->as.import.target) continue;
            if (!imp->as.import.target->as.qualifiedName.resolved) return true;
        }
    }
    return false;
}

/* ---- resolution drivers ----------------------------------------- */

static void resolveQualifiedNameImpl(Node* qname, Scope* current, bool report) {
    if (!qname || qname->kind != NODE_QUALIFIED_NAME) return;
    int n = qname->as.qualifiedName.partCount;
    if (n == 0) return;
    /* Idempotent: if someone (the builtin, or a previous pass) already
     * filled this in, don't clobber. */
    if (qname->as.qualifiedName.resolved) return;

    /* (1) Resolve the first segment via the scope chain, falling back
     * to wildcard imports. */
    Token first = qname->as.qualifiedName.parts[0];
    const Node* node = lookupChain(current, first);
    if (!node) node = searchWildcardImports(current, first);

    if (!node) {
        /* First segment unresolved.  Two tentative-accept paths
         * preserve behavior for cases where we can't blame the user:
         *   - multi-segment names whose head might be a top-level
         *     package we don't load
         *   - any name when an unresolved wildcard import is in scope */
        if (n > 1) return;
        if (anyUnresolvedWildcardInScope(current)) return;
        if (report) undefinedNameError(qname->line, first);
        return;
    }

    /* (2) Walk subsequent segments as members of the previous
     * resolution.  A failure mid-chain is an error: we know enough
     * to say the user got the path wrong. */
    for (int i = 1; i < n; i++) {
        Token seg = qname->as.qualifiedName.parts[i];
        const Node* member = lookupMember(node, seg);
        if (!member) {
            if (report) undefinedMemberError(qname->line, seg, node);
            return;
        }
        node = member;
    }

    qname->as.qualifiedName.resolved = node;
}

static void resolveQualifiedName(Node* qname, Scope* current) {
    resolveQualifiedNameImpl(qname, current, true);
}

static void tryResolveQualifiedName(Node* qname, Scope* current) {
    resolveQualifiedNameImpl(qname, current, false);
}

static void resolveNodeList(NodeList* list, Scope* current) {
    for (int i = 0; i < list->count; i++) {
        resolveQualifiedName(list->items[i], current);
    }
}

/* Resolve only the multi-segment qnames in a list, skipping any
 * single-segment ones.  Used for redefines: single-segment redef
 * targets (`:>> torque`) need supertype-based lookup, not local
 * scope lookup, so the redefinition checker handles those.  But
 * multi-segment targets (`:>> Engine::torque`) are just regular
 * qualified-name lookups that the resolver knows how to do via
 * lookupMember — so we do them here and the redef checker can
 * focus on validation rather than lookup. */
static void resolveMultiSegmentRedefs(NodeList* list, Scope* current) {
    for (int i = 0; i < list->count; i++) {
        Node* qname = list->items[i];
        if (!qname || qname->kind != NODE_QUALIFIED_NAME) continue;
        if (qname->as.qualifiedName.partCount > 1) {
            resolveQualifiedName(qname, current);
        }
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
        /* Specializes points at other top-level names — outer scope. */
        resolveNodeList(&n->as.scope.specializes, current);
        /* Single-segment redefs are handled by the redefinition pass
         * (they need supertype lookup, not local scope).  Multi-segment
         * ones are regular qualified-name lookups, so we do them here
         * and the redef pass just validates. */
        resolveMultiSegmentRedefs(&n->as.scope.redefines, current);

        Scope inner = { .parent = current, .what = "definition" };
        resolveScopeBody(&inner, n->as.scope.members, n->as.scope.memberCount);
        freeScope(&inner);
        break;
    }

    case NODE_USAGE: {
        /* Type, spec, and endpoint refs resolve in OUTER scope.
         * Redefines: same split as definitions — see above. */
        resolveNodeList(&n->as.usage.types,       current);
        resolveNodeList(&n->as.usage.specializes, current);
        resolveMultiSegmentRedefs(&n->as.usage.redefines, current);
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
        resolveMultiSegmentRedefs(&n->as.attribute.redefines, current);
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

    /* Inject the synthetic standard library so wildcard imports of
     * `ScalarValues::*` find real symbols.  We cast away const because
     * declareName needs a non-const Node* — but we never mutate the
     * builtin tree afterwards.                                      */
    Node* stdlib = (Node*)builtinScalarValuesPackage();
    declareName(&root, stdlib->as.scope.name, stdlib);

    /* Declare user's top-level decls. */
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
