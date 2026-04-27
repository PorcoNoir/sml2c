/* sysmlc — redefchecker.c
 *
 * Implementation of the redefinition checker.  See redefchecker.h.
 *
 * Algorithm:
 *
 *   walk(node, enclosing):
 *     case PROGRAM, PACKAGE: walk children with enclosing=NULL
 *     case DEFINITION, USAGE: walk children with enclosing=node
 *     case ATTRIBUTE: if redefines non-empty and enclosing != NULL,
 *                     check each redef target
 *
 *   findInSupertypes(owner, name) walks owner's specializes (and
 *   for usages, also `types`) transitively, searching each
 *   ancestor's direct members for `name`.  Cycles are defanged by
 *   a depth bound.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "redefchecker.h"
#include "typechecker.h"   /* for specializesType */

/* ---- module-level state ---------------------------------------- */

static int errorCount = 0;

static void redefError(int line, const char* fmt, ...) {
    fprintf(stderr, "[line %d] Redefinition error: ", line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    errorCount++;
}

/* ---- small AST helpers ----------------------------------------- */

static bool tokensEqual(Token a, Token b) {
    if (a.length != b.length) return false;
    return memcmp(a.start, b.start, (size_t)a.length) == 0;
}

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

/* ---- supertype search ------------------------------------------ */

/* Forward declarations for the mutually-recursive walk. */
static const Node* searchAncestorMembers(const Node* node, Token name, int depth);

/* Search a list of supertype references — for each resolved target,
 * try its direct members and then recurse into its own supertypes. */
static const Node* searchListAncestors(const NodeList* list, Token name, int depth) {
    if (depth >= 32) return NULL;
    for (int i = 0; i < list->count; i++) {
        const Node* ref = list->items[i];
        if (!ref || ref->kind != NODE_QUALIFIED_NAME) continue;
        const Node* ancestor = ref->as.qualifiedName.resolved;
        if (!ancestor) continue;
        const Node* found = searchAncestorMembers(ancestor, name, depth + 1);
        if (found) return found;
    }
    return NULL;
}

/* Look at a node's direct named members for one matching `name`. */
static const Node* searchDirectMembers(Node** members, int count, Token name) {
    for (int i = 0; i < count; i++) {
        Node* m = members[i];
        Token mName = nodeName(m);
        if (mName.length > 0 && tokensEqual(mName, name)) return m;
    }
    return NULL;
}

/* Search a node's direct members, then recurse into ITS supertypes.
 * Used as the recursive step from searchListAncestors.            */
static const Node* searchAncestorMembers(const Node* node, Token name, int depth) {
    if (!node || depth >= 32) return NULL;
    if (node->kind == NODE_DEFINITION) {
        const Node* m = searchDirectMembers(node->as.scope.members,
                                            node->as.scope.memberCount, name);
        if (m) return m;
        return searchListAncestors(&node->as.scope.specializes, name, depth);
    }
    if (node->kind == NODE_USAGE) {
        const Node* m = searchDirectMembers(node->as.usage.members,
                                            node->as.usage.memberCount, name);
        if (m) return m;
        const Node* r = searchListAncestors(&node->as.usage.types, name, depth);
        if (r) return r;
        return searchListAncestors(&node->as.usage.specializes, name, depth);
    }
    return NULL;
}

/* For a feature inside `owner`, look up `name` in owner's strict
 * supertypes — that is, NOT in owner's own members, but in whatever
 * owner specializes (and for usages, also their types).            */
static const Node* findInSupertypes(const Node* owner, Token name) {
    if (!owner) return NULL;
    if (owner->kind == NODE_DEFINITION) {
        return searchListAncestors(&owner->as.scope.specializes, name, 0);
    }
    if (owner->kind == NODE_USAGE) {
        const Node* r = searchListAncestors(&owner->as.usage.types, name, 0);
        if (r) return r;
        return searchListAncestors(&owner->as.usage.specializes, name, 0);
    }
    return NULL;
}

/* ---- per-attribute check --------------------------------------- */

/* Pull the declared type of a feature node, if any.  Returns the
 * resolved type-definition pointer or NULL if untyped/unresolved. */
static const Node* declaredType(const Node* n) {
    if (!n) return NULL;
    const NodeList* types = NULL;
    if (n->kind == NODE_ATTRIBUTE) types = &n->as.attribute.types;
    else if (n->kind == NODE_USAGE) types = &n->as.usage.types;
    if (!types || types->count == 0) return NULL;
    const Node* ref = types->items[0];
    if (!ref || ref->kind != NODE_QUALIFIED_NAME) return NULL;
    return ref->as.qualifiedName.resolved;
}

static const char* nameLexeme(Token t, char* buf, size_t bufsize) {
    size_t n = (size_t)t.length;
    if (n >= bufsize) n = bufsize - 1;
    memcpy(buf, t.start, n);
    buf[n] = '\0';
    return buf;
}

/* Check one redefines target against the enclosing owner's supertypes. */
static void checkOneRedef(Node* targetQname, const Node* owner, const Node* redefiningAttr) {
    if (!targetQname || targetQname->kind != NODE_QUALIFIED_NAME) return;
    int n = targetQname->as.qualifiedName.partCount;
    if (n == 0) return;

    /* Multi-segment redef targets need member-of-type resolution,
     * which the project hasn't built yet.  Skip silently. */
    if (n > 1) return;

    Token targetName = targetQname->as.qualifiedName.parts[0];
    const Node* found = findInSupertypes(owner, targetName);

    char nameBuf[128];
    nameLexeme(targetName, nameBuf, sizeof nameBuf);

    if (!found) {
        redefError(targetQname->line,
                   "Cannot redefine '%s': no such feature in any supertype.",
                   nameBuf);
        return;
    }

    /* Stamp the resolution that the resolver intentionally skipped. */
    targetQname->as.qualifiedName.resolved = found;

    /* The redefined target must be a feature — attribute or usage.
     * Datatype/part definitions are specialized via :>, not redefined. */
    if (found->kind != NODE_ATTRIBUTE && found->kind != NODE_USAGE) {
        redefError(targetQname->line,
                   "Cannot redefine '%s': target is not a feature.",
                   nameBuf);
        return;
    }

    /* For now we only check attribute-on-attribute redefinition's
     * type compatibility.  Mixed-kind redefs (e.g. usage redefining
     * an attribute) are flagged here as a category mismatch.       */
    if (redefiningAttr->kind == NODE_ATTRIBUTE && found->kind != NODE_ATTRIBUTE) {
        redefError(targetQname->line,
                   "Attribute cannot redefine non-attribute feature '%s'.",
                   nameBuf);
        return;
    }

    /* Type compatibility — narrowing only.  If either side has no
     * declared type, skip (the redefining attribute may be inheriting
     * its type from the redefined target, which is allowed). */
    const Node* redefType = declaredType(redefiningAttr);
    const Node* targetType = declaredType(found);
    if (redefType && targetType && !specializesType(redefType, targetType)) {
        char redefBuf[128], targetBuf[128];
        nameLexeme(redefType->as.scope.name, redefBuf, sizeof redefBuf);
        nameLexeme(targetType->as.scope.name, targetBuf, sizeof targetBuf);
        redefError(redefiningAttr->line,
                   "Redefining '%s' with type '%s' is incompatible with redefined type '%s'.",
                   nameBuf, redefBuf, targetBuf);
    }
}

/* Iterate an attribute's redefines list, checking each entry. */
static void checkAttributeRedefs(Node* attr, const Node* owner) {
    NodeList* redefs = &attr->as.attribute.redefines;
    for (int i = 0; i < redefs->count; i++) {
        checkOneRedef(redefs->items[i], owner, attr);
    }
}

/* ---- whole-program walker -------------------------------------- */

static void walk(Node* n, const Node* enclosing) {
    if (!n) return;
    switch (n->kind) {
    case NODE_PROGRAM:
    case NODE_PACKAGE:
        for (int i = 0; i < n->as.scope.memberCount; i++) {
            walk(n->as.scope.members[i], NULL);
        }
        break;

    case NODE_DEFINITION:
        for (int i = 0; i < n->as.scope.memberCount; i++) {
            walk(n->as.scope.members[i], n);
        }
        break;

    case NODE_USAGE:
        for (int i = 0; i < n->as.usage.memberCount; i++) {
            walk(n->as.usage.members[i], n);
        }
        break;

    case NODE_ATTRIBUTE:
        if (enclosing && n->as.attribute.redefines.count > 0) {
            checkAttributeRedefs(n, enclosing);
        }
        break;

    default:
        break;
    }
}

/* ---- public entry ---------------------------------------------- */

bool checkRedefinitions(Node* program) {
    errorCount = 0;
    walk(program, NULL);
    if (errorCount > 0) {
        fprintf(stderr, "Redefinition checking failed with %d error%s.\n",
                errorCount, errorCount == 1 ? "" : "s");
        return false;
    }
    return true;
}
