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

bool tokensEqual(Token a, Token b) {
    if (a.length != b.length) return false;
    return memcmp(a.start, b.start, (size_t)a.length) == 0;
}

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

/* Pull the declaring name out of any node kind that introduces one.
 * Returns a length-zero token for anonymous or non-naming nodes.    */
Token nodeName(const Node* n) {
    static const Token empty = {0};
    if (!n) return empty;
    switch (n->kind) {
    case NODE_PACKAGE:
    case NODE_DEFINITION: return n->as.scope.name;
    case NODE_USAGE:      return n->as.usage.name;
    case NODE_ATTRIBUTE:  return n->as.attribute.name;
    case NODE_ALIAS:      return n->as.alias.name;
    case NODE_COMMENT:    return n->as.comment.name;     /* may be empty */
    case NODE_DEPENDENCY: return n->as.dependency.name;  /* may be empty */
    default:              return empty;
    }
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
const Node* searchWildcardImports(const Scope* scope, Token name) {
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
