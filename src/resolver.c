/* sysmlc — resolver.c
 *
 * Two-phase scoped symbol-table walker.  See resolver.h for purpose;
 * see resolver_internal.h for the split with resolver_scope.c, which
 * holds the Scope/Symbol primitives, member-of-type lookup, alias
 * deref, and wildcard search.  This file is just the walker.
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
 * Multi-segment names (A::B::C) resolve segment by segment via
 * lookupMember (in resolver_scope.c), tracking conjugation parity
 * through the chain for downstream consumers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "resolver.h"
#include "resolver_internal.h"
#include "builtin.h"

/* ---- declareMembers + forward decls ----------------------------- */

static void declareMembers(Scope* inner, Node** members, int count);
static void resolveNode(Node* n, Scope* current);
static void resolveQualifiedName(Node* qname, Scope* current);
static void resolveExpression(Node* expr, Scope* current);
static void resolveNodeList(NodeList* list, Scope* current);

/* Variant that doesn't report errors on failure — used for import
 * targets, where we tolerate "unknown library" and just leave the
 * resolution NULL.                                                  */
static void tryResolveQualifiedName(Node* qname, Scope* current);

/* Three single-purpose passes over the member array.
 *
 *   1. declareNamedMembers — strong bindings for every member that
 *      has a name token.  Runs first so passes 2 and 3 can rely on
 *      every real decl in the scope being locatable.
 *
 *   2. registerImports — resolves each import's target qname against
 *      the inner scope (so an import naming a sibling pass-1 decl
 *      finds it), appends wildcard imports to inner->wildcardImports,
 *      and soft-aliases non-wildcard imports under the qname's last
 *      segment.
 *
 *   3. aliasAnonymousRedefines — soft-aliases anonymous USAGE nodes
 *      (no name token) whose `redefines` list is non-empty, under
 *      the redefined name.  Lets `perform A::B redefines foo;` be
 *      reachable as `foo` from sibling code that wants the local
 *      copy of foo rather than the inherited stub.
 *
 * Both alias passes use declareNameIfFree, so a real pass-1 decl
 * always wins on name conflict.  This makes the relative ordering
 * of passes 2 and 3 unimportant for correctness; the chosen order
 * (imports first, redefines last) just reflects "redefines aliases
 * are the most local thing in this scope, so they're the last
 * fallback to register". */

static void declareNamedMembers(Scope* inner, Node** members, int count) {
    for (int i = 0; i < count; i++) {
        Node* m = members[i];
        if (!m || m->kind == NODE_IMPORT) continue;
        Token name = nodeName(m);
        if (name.length > 0) declareName(inner, name, m);
    }
}

static void registerImports(Scope* inner, Node** members, int count) {
    for (int i = 0; i < count; i++) {
        Node* m = members[i];
        if (!m || m->kind != NODE_IMPORT) continue;
        if (m->as.import.target) {
            tryResolveQualifiedName(m->as.import.target, inner);
        }
        if (m->as.import.wildcard) {
            astListAppend(&inner->wildcardImports, m);
            continue;
        }
        /* Non-wildcard `import A::B::C;` — soft-alias C. */
        const Node* tgt      = m->as.import.target;
        const Node* resolved = tgt ? tgt->as.qualifiedName.resolved : NULL;
        if (resolved) {
            declareNameIfFree(inner, qnameLastSegment(tgt), resolved);
        }
    }
}

static void aliasAnonymousRedefines(Scope* inner, Node** members, int count) {
    for (int i = 0; i < count; i++) {
        Node* m = members[i];
        if (!m || m->kind != NODE_USAGE) continue;
        if (nodeName(m).length > 0) continue;     /* only anonymous */
        if (m->as.usage.redefines.count == 0) continue;
        const Node* rref = m->as.usage.redefines.items[0];
        declareNameIfFree(inner, qnameLastSegment(rref), m);
    }
}

static void declareMembers(Scope* inner, Node** members, int count) {
    declareNamedMembers     (inner, members, count);
    registerImports         (inner, members, count);
    aliasAnonymousRedefines (inner, members, count);
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
     * to say the user got the path wrong.
     *
     * Track conjugation parity along the way: if at any hop the
     * previous segment's resolved feature has a conjugated type
     * reference, the parity toggles.  Consumers (notably the flow
     * direction checker) read this to flip port directions when
     * they're reached through a conjugated type.                  */
    bool parity = false;
    for (int i = 1; i < n; i++) {
        /* Before stepping into the next member, check whether the
         * current node is a feature whose type ref is conjugated.
         * If so, toggle parity for this hop.                       */
        const NodeList* prevTypes = NULL;
        if (node->kind == NODE_USAGE)         prevTypes = &node->as.usage.types;
        else if (node->kind == NODE_ATTRIBUTE) prevTypes = &node->as.attribute.types;
        if (prevTypes && prevTypes->count > 0) {
            const Node* tref = prevTypes->items[0];
            if (tref && tref->kind == NODE_QUALIFIED_NAME
                     && tref->as.qualifiedName.isConjugated) {
                parity = !parity;
            }
        }

        Token seg = qname->as.qualifiedName.parts[i];
        const Node* member = lookupMember(node, seg);
        if (!member) {
            if (report) undefinedMemberError(qname->line, seg, node);
            return;
        }
        node = member;
    }

    /* If the qname itself starts with a `~` (top-level conjugation
     * marker), include that in the parity.                          */
    if (qname->as.qualifiedName.isConjugated) parity = !parity;
    qname->as.qualifiedName.conjugationParity = parity;

    /* Final dereference: if the resolved node is an alias, follow it
     * to the underlying target.  This ensures every consumer of the
     * resolution sees the real declaration, not the alias.           */
    const Node* deref = derefAlias(node);
    if (deref) node = deref;

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
    case NODE_CALL:
        /* Resolve the callee expression (typically a qualified name
         * pointing at a calc def) and each argument expression.  We
         * don't yet check arity or argument types — that needs the
         * typed AST coming in v0.10+.                                */
        resolveExpression(expr->as.call.callee, current);
        for (int i = 0; i < expr->as.call.args.count; i++) {
            resolveExpression(expr->as.call.args.items[i], current);
        }
        break;
    case NODE_MEMBER_ACCESS:
        /* Resolve the target expression; the `member` token can't be
         * resolved without knowing the target's type, so we leave it
         * for the typechecker.  This means `vehicle.engine.mass` does
         * NOT currently produce an "undefined" error if `mass` is
         * misspelled — that's a known v0.10 gap.                     */
        resolveExpression(expr->as.memberAccess.target, current);
        break;
    default:
        /* Other node kinds shouldn't appear in expression position;
         * the parser produces only the kinds above.                  */
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
        inner.inheritedFrom = firstResolvedQname(&n->as.scope.specializes);
        resolveScopeBody(&inner, n->as.scope.members, n->as.scope.memberCount);
        /* Constraint def body expressions reference parameters declared
         * as members; resolve the body inside the inner scope so those
         * references bind correctly.                                   */
        if (n->as.scope.body) {
            resolveExpression(n->as.scope.body, &inner);
        }
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
        resolveExpression(n->as.usage.defaultValue, current);
        /* Inline constraint body: `assert constraint { x > 0 }`.
         * Resolved against the outer scope, since these usages don't
         * introduce parameters of their own.                           */
        resolveExpression(n->as.usage.body, current);

        if (n->as.usage.memberCount > 0) {
            Scope inner = { .parent = current, .what = "usage" };
            /* Inherit from the usage's declared type or, failing
             * that, its first specializes — both bring in features
             * that should be visible by bare name inside the body. */
            inner.inheritedFrom = firstResolvedQname(&n->as.usage.types);
            if (!inner.inheritedFrom) {
                inner.inheritedFrom = firstResolvedQname(&n->as.usage.specializes);
            }
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

    case NODE_ALIAS:
        /* An alias's target is a qname interpreted in the alias's
         * enclosing scope.  Resolving here also feeds the
         * derefAlias() chain followed at every later use site. */
        resolveQualifiedName(n->as.alias.target, current);
        break;

    case NODE_COMMENT:
        /* `comment about A, B` — each `about` target is resolved
         * normally.  An anonymous untargeted comment has nothing
         * to resolve. */
        resolveNodeList(&n->as.comment.about, current);
        break;

    case NODE_DEPENDENCY:
        /* Both source and target lists hold qualified-name refs in
         * the dependency's enclosing scope. */
        resolveNodeList(&n->as.dependency.sources, current);
        resolveNodeList(&n->as.dependency.targets, current);
        break;

    case NODE_SUCCESSION: {
        /* The `first` ref and each qname target resolve in the
         * enclosing scope.  Inline action declarations (USAGE nodes)
         * recurse so their own type refs and members are processed.
         * `start`/`done` arrive pre-resolved by the parser, so the
         * lookup is a no-op for them — resolveQualifiedName notices
         * `resolved` is already set and skips the lookup.            */
        if (n->as.succession.first) {
            resolveQualifiedName(n->as.succession.first, current);
        }
        for (int i = 0; i < n->as.succession.targets.count; i++) {
            Node* t = n->as.succession.targets.items[i];
            if (!t) continue;
            if (t->kind == NODE_QUALIFIED_NAME) {
                resolveQualifiedName(t, current);
            } else {
                resolveNode(t, current);
            }
        }
        break;
    }

    case NODE_TRANSITION: {
        /* Each named ref resolves in the enclosing scope.  The guard
         * (an expression) walks via resolveExpression.                */
        if (n->as.transition.first)  resolveQualifiedName(n->as.transition.first,  current);
        if (n->as.transition.accept) resolveQualifiedName(n->as.transition.accept, current);
        if (n->as.transition.effect) resolveQualifiedName(n->as.transition.effect, current);
        if (n->as.transition.target) resolveQualifiedName(n->as.transition.target, current);
        if (n->as.transition.guard)  resolveExpression(n->as.transition.guard,     current);
        break;
    }

    case NODE_LIFECYCLE_ACTION:
        if (n->as.lifecycleAction.action) {
            resolveQualifiedName(n->as.lifecycleAction.action, current);
        }
        break;

    case NODE_RETURN:
        resolveNodeList(&n->as.ret.types,       current);
        resolveNodeList(&n->as.ret.specializes, current);
        resolveExpression(n->as.ret.defaultValue, current);
        break;

    /* Leaves — no inner refs to resolve. */
    case NODE_DOC:
    case NODE_QUALIFIED_NAME:
    case NODE_MULTIPLICITY:
    case NODE_LITERAL:
    case NODE_BINARY:
    case NODE_UNARY:
    case NODE_CALL:
    case NODE_MEMBER_ACCESS:
        break;
    }
}

/* ---- public entry point ----------------------------------------- */

bool resolveProgram(Node* program) {
    resolverErrorCount = 0;
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

    if (resolverErrorCount > 0) {
        fprintf(stderr, "Resolution failed with %d error%s.\n",
                resolverErrorCount, resolverErrorCount == 1 ? "" : "s");
        return false;
    }
    return true;
}
