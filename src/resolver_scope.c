/* sysmlc — resolver_scope.c
 *
 * Scope/Symbol primitives plus the lookups built on top of them.
 * See resolver_internal.h for the full split.
 *
 * Functions here used to be `static` when resolver.c was one file;
 * promoted to extern so the walker (resolver.c) can call them.  The
 * resolverErrorCount static is similarly promoted via extern.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "resolver_internal.h"

/* ---- module-level error counter --------------------------------- */

int resolverErrorCount = 0;

void undefinedNameError(int line, Token name) {
    fprintf(stderr, "[line %d] Error: Undefined name '%.*s'.\n",
            line, name.length, name.start);
    resolverErrorCount++;
}

void undefinedMemberError(int line, Token name, const Node* container) {
    Token cname = nodeName(container);
    if (cname.length > 0) {
        fprintf(stderr, "[line %d] Error: '%.*s' has no member '%.*s'.\n",
                line, cname.length, cname.start,
                name.length, name.start);
    } else {
        fprintf(stderr, "[line %d] Error: No member '%.*s' in container.\n",
                line, name.length, name.start);
    }
    resolverErrorCount++;
}

void duplicateNameError(int line, Token name, int prevLine) {
    fprintf(stderr,
            "[line %d] Error: Duplicate declaration of '%.*s' "
            "(previously declared at line %d).\n",
            line, name.length, name.start, prevLine);
    resolverErrorCount++;
}

/* ---- symbol table ----------------------------------------------- */

Symbol* lookupLocal(const Scope* scope, Token name) {
    for (Symbol* s = scope->symbols; s; s = s->next) {
        if (tokensEqual(s->name, name)) return s;
    }
    return NULL;
}

const Node* lookupChain(const Scope* scope, Token name) {
    for (const Scope* s = scope; s; s = s->parent) {
        Symbol* found = lookupLocal(s, name);
        if (found) return found->decl;
        /* Feature inheritance: if this scope was opened for a usage
         * whose declared type carries members, try those next.  This
         * makes `t : UC { ... }` see UC's members by bare name from
         * inside t's body (and recursively, UC's own supertype's).   */
        if (s->inheritedFrom) {
            const Node* m = lookupMember(s->inheritedFrom, name);
            if (m) return m;
        }
    }
    return NULL;
}

/* Anonymous decls (length-zero name tokens) are silently skipped —
 * standalone `connect a to b;` and similar produce nameless usages. */
void declareName(Scope* scope, Token name, const Node* decl) {
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

/* Same idea but silent on conflict: returns true iff a fresh binding
 * was added.  Used by import re-exports and anonymous-redefines
 * aliases — both want the binding only if no real declaration is
 * already in the way.                                              */
bool declareNameIfFree(Scope* scope, Token name, const Node* decl) {
    if (name.length == 0) return false;
    if (lookupLocal(scope, name)) return false;
    Symbol* s = (Symbol*)calloc(1, sizeof(Symbol));
    if (!s) { fprintf(stderr, "Out of memory\n"); exit(70); }
    s->name = name;
    s->decl = decl;
    s->next = scope->symbols;
    scope->symbols = s;
    return true;
}

void freeScope(Scope* scope) {
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

/* Last identifier of a qualified name, i.e. `C` in `A::B::C`.
 * Returns a length-zero token if the node isn't a qname or has no
 * parts.  Used by import-aliasing and redefinition-aliasing logic
 * in declareMembers, where the user-visible name of a re-exported
 * decl is the trailing segment of its source qname.                */
Token qnameLastSegment(const Node* qname) {
    static const Token empty = {0};
    if (!qname || qname->kind != NODE_QUALIFIED_NAME) return empty;
    int n = qname->as.qualifiedName.partCount;
    if (n == 0) return empty;
    return qname->as.qualifiedName.parts[n - 1];
}

/* First item of `list` if it's a qname with a resolved decl, else
 * NULL.  Used by the usage/definition resolver arms to pick the
 * primary inheritance source for `Scope.inheritedFrom` from a
 * `types`/`specializes` list — three near-identical inline blocks
 * collapse to one call.                                            */
const Node* firstResolvedQname(const NodeList* list) {
    if (!list || list->count == 0) return NULL;
    const Node* ref = list->items[0];
    if (!ref || ref->kind != NODE_QUALIFIED_NAME) return NULL;
    return ref->as.qualifiedName.resolved;
}

/* If `n` has a non-empty name token equal to `target`, return `n`
 * (with the const stripped, since callers need a mutable Node*).
 * Otherwise return NULL.  Used by member-scan loops that ask
 * "does this AST node match this name?" — collapses the
 * length-check / tokensEqual / cast triplet down to one call.    */
static Node* nodeNameMatches(const Node* n, Token target) {
    Token nm = nodeName(n);
    if (nm.length > 0 && tokensEqual(nm, target)) return (Node*)n;
    return NULL;
}

/* ---- alias dereferencing ---------------------------------------- *
 *
 * An alias ("alias E for AliasDemo::Engine;") binds a name to a
 * qualified-name reference.  Once that reference is resolved, the
 * alias is functionally a synonym for whatever it targets — every
 * later use of the alias name should "see through" to the real node.
 *
 * We dereference at the lookup boundary rather than eagerly rewriting
 * the AST: the alias node itself stays in the tree (so the printer
 * and JSON emitter can render it), but resolution always returns the
 * underlying target.                                               */
const Node* derefAlias(const Node* n) {
    int depth = 0;
    while (n && n->kind == NODE_ALIAS && depth < 32) {
        if (!n->as.alias.target) return NULL;
        if (n->as.alias.target->kind != NODE_QUALIFIED_NAME) return NULL;
        const Node* next = n->as.alias.target->as.qualifiedName.resolved;
        if (!next) return NULL;     /* alias target unresolved — give up */
        n = next;
        depth++;
    }
    return n;
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

const Node* lookupMemberDepth(const Node* container, Token name, int depth) {
    if (!container || depth >= 32) return NULL;

    /* If `container` is itself an alias, follow it to the real target
     * before searching members.  This makes `E::torque` work for
     * `alias E for Engine`. */
    container = derefAlias(container);
    if (!container) return NULL;

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
        Node* hit = nodeNameMatches(m, name);
        if (hit) return hit;
        /* `then action <name>;` parses as a succession statement whose
         * target is a NODE_USAGE.  The named target should be visible
         * as a member of the enclosing usage/definition, so flatten
         * succession.targets into the search.  Same for `first <name>`. */
        if (m && m->kind == NODE_SUCCESSION) {
            const NodeList* targets = &m->as.succession.targets;
            for (int j = 0; j < targets->count; j++) {
                hit = nodeNameMatches(targets->items[j], name);
                if (hit) return hit;
            }
            hit = nodeNameMatches(m->as.succession.first, name);
            if (hit) return hit;
        }
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

/* ---- transitive re-export search -------------------------------- */

/* Visited-set for the recursive package walk.  Capped at 32 because
 * real SysML files re-export at most a few levels deep; a deeper
 * chain almost certainly indicates a cycle, which we just refuse
 * to follow.                                                       */
#define VISITED_CAP 32
typedef struct { const Node* items[VISITED_CAP]; int count; } VisitedSet;

static bool visitedContains(const VisitedSet* v, const Node* p) {
    for (int i = 0; i < v->count; i++) if (v->items[i] == p) return true;
    return false;
}
static bool visitedAdd(VisitedSet* v, const Node* p) {
    if (v->count >= VISITED_CAP) return false;
    v->items[v->count++] = p;
    return true;
}

/* Search a package for `name`, recursing through its own *public*
 * wildcard imports.  This makes a chain like
 *
 *   package Top { public import A::*; }
 *   package A   { public import B::*; }
 *   package B   { part def Vehicle; }
 *
 * resolve `Vehicle` from Top — the v0.18 fix.  Without recursion the
 * walk stops at A's direct members, none of which is named Vehicle.
 *
 * `private` imports are deliberately not re-exported (consistent
 * with SysML's visibility semantics: they bring names INTO the
 * containing package's scope, but don't add to its public surface).
 *
 * Visibility-default imports are treated as public for now to match
 * the PTC reference file's mixed style; SysML's spec would call this
 * package-private, but the practical effect on a single-file
 * compilation is the same.                                         */
static const Node* searchPackageReExports(const Node* pkg, Token name,
                                          VisitedSet* visited) {
    if (!pkg || pkg->kind != NODE_PACKAGE) return NULL;
    if (visitedContains(visited, pkg)) return NULL;
    if (!visitedAdd(visited, pkg))     return NULL;

    /* Direct members. */
    for (int i = 0; i < pkg->as.scope.memberCount; i++) {
        Node* hit = nodeNameMatches(pkg->as.scope.members[i], name);
        if (hit) return hit;
    }

    /* Re-exported (public / default-visibility) wildcard imports. */
    for (int i = 0; i < pkg->as.scope.memberCount; i++) {
        const Node* mem = pkg->as.scope.members[i];
        if (!mem || mem->kind != NODE_IMPORT) continue;
        if (!mem->as.import.wildcard)         continue;
        if (mem->as.import.visibility == VIS_PRIVATE) continue;
        const Node* tgt = mem->as.import.target;
        if (!tgt || tgt->kind != NODE_QUALIFIED_NAME) continue;
        const Node* resolved = tgt->as.qualifiedName.resolved;
        if (!resolved || resolved->kind != NODE_PACKAGE) continue;
        const Node* hit = searchPackageReExports(resolved, name, visited);
        if (hit) return hit;
    }
    return NULL;
}

/* Look up `name` in any wildcard-imported package whose target was
 * successfully resolved.  Returns the matching member or NULL.
 *
 * Walks the local scope chain outward; for each wildcard import in
 * each scope, dispatches to searchPackageReExports which handles
 * transitive re-exports.                                            */
const Node* searchWildcardImports(const Scope* scope, Token name) {
    for (const Scope* s = scope; s; s = s->parent) {
        for (int i = 0; i < s->wildcardImports.count; i++) {
            const Node* imp = s->wildcardImports.items[i];
            if (!imp || !imp->as.import.target) continue;
            const Node* target = imp->as.import.target->as.qualifiedName.resolved;
            if (!target || target->kind != NODE_PACKAGE) continue;
            VisitedSet visited = { .count = 0 };
            const Node* hit = searchPackageReExports(target, name, &visited);
            if (hit) return hit;
        }
    }
    return NULL;
}

/* True if any wildcard import in the scope chain has an UNRESOLVED
 * target — meaning we don't know what symbols it brings in, so we
 * should be permissive about names that might come from there.    */
bool anyUnresolvedWildcardInScope(const Scope* scope) {
    for (const Scope* s = scope; s; s = s->parent) {
        for (int i = 0; i < s->wildcardImports.count; i++) {
            const Node* imp = s->wildcardImports.items[i];
            if (!imp || !imp->as.import.target) continue;
            if (!imp->as.import.target->as.qualifiedName.resolved) return true;
        }
    }
    return false;
}
