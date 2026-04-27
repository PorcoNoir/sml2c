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
#include "resolver.h"      /* for lookupMember */
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

/* For a feature inside `owner`, look up `name` in owner's strict
 * supertypes — that is, NOT in owner's own members, but in whatever
 * owner specializes (and for usages, also in their types).  We
 * iterate the owner's direct supertype references and call the
 * resolver's lookupMember on each — which already handles the
 * transitive walk through that supertype's own ancestors.        */
static const Node* findInSupertypes(const Node* owner, Token name) {
    if (!owner) return NULL;
    const NodeList* specializes = NULL;
    const NodeList* types       = NULL;
    if (owner->kind == NODE_DEFINITION) {
        specializes = &owner->as.scope.specializes;
    } else if (owner->kind == NODE_USAGE) {
        types       = &owner->as.usage.types;
        specializes = &owner->as.usage.specializes;
    } else {
        return NULL;
    }
    if (types) {
        for (int i = 0; i < types->count; i++) {
            const Node* ref = types->items[i];
            if (!ref || ref->kind != NODE_QUALIFIED_NAME) continue;
            const Node* ancestor = ref->as.qualifiedName.resolved;
            const Node* found = lookupMember(ancestor, name);
            if (found) return found;
        }
    }
    if (specializes) {
        for (int i = 0; i < specializes->count; i++) {
            const Node* ref = specializes->items[i];
            if (!ref || ref->kind != NODE_QUALIFIED_NAME) continue;
            const Node* ancestor = ref->as.qualifiedName.resolved;
            const Node* found = lookupMember(ancestor, name);
            if (found) return found;
        }
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

/* Walk a list of supertype references, recursing transitively.  Return
 * true if some ancestor in the chain is named `qualifier` AND has
 * `name` reaching `target` via lookupMember.  Used to verify that a
 * multi-segment redef target like `Q::name` actually walks through
 * `Q` to reach the resolved member.                                 */
static bool walkAndMatchQualifier(const NodeList* list, Token qualifier,
                                  Token name, const Node* target, int depth) {
    if (depth >= 32) return false;
    for (int i = 0; i < list->count; i++) {
        const Node* ref = list->items[i];
        if (!ref || ref->kind != NODE_QUALIFIED_NAME) continue;
        const Node* ancestor = ref->as.qualifiedName.resolved;
        if (!ancestor) continue;

        Token aName = nodeName(ancestor);
        if (aName.length > 0 && tokensEqual(aName, qualifier)) {
            const Node* found = lookupMember(ancestor, name);
            if (found == target) return true;
        }

        /* Recurse into the ancestor's own supertypes/types. */
        if (ancestor->kind == NODE_DEFINITION) {
            if (walkAndMatchQualifier(&ancestor->as.scope.specializes,
                                      qualifier, name, target, depth + 1))
                return true;
        } else if (ancestor->kind == NODE_USAGE) {
            if (walkAndMatchQualifier(&ancestor->as.usage.types,
                                      qualifier, name, target, depth + 1))
                return true;
            if (walkAndMatchQualifier(&ancestor->as.usage.specializes,
                                      qualifier, name, target, depth + 1))
                return true;
        }
    }
    return false;
}

/* Top-level entry: does `target` (named `name`) lie in a supertype
 * of `owner` accessible via the named `qualifier`?                  */
static bool qualifierMatches(const Node* owner, Token qualifier,
                             Token name, const Node* target) {
    if (!owner) return false;
    if (owner->kind == NODE_DEFINITION) {
        return walkAndMatchQualifier(&owner->as.scope.specializes,
                                     qualifier, name, target, 0);
    }
    if (owner->kind == NODE_USAGE) {
        if (walkAndMatchQualifier(&owner->as.usage.types,
                                  qualifier, name, target, 0)) return true;
        return walkAndMatchQualifier(&owner->as.usage.specializes,
                                     qualifier, name, target, 0);
    }
    return false;
}

/* Check one redefines target against the enclosing owner's supertypes. */
static void checkOneRedef(Node* targetQname, const Node* owner, const Node* redefiningAttr) {
    if (!targetQname || targetQname->kind != NODE_QUALIFIED_NAME) return;
    int n = targetQname->as.qualifiedName.partCount;
    if (n == 0) return;

    /* Last segment is always the redef name; earlier segments form
     * the qualifier path. */
    Token displayName = targetQname->as.qualifiedName.parts[n - 1];
    char nameBuf[128];
    nameLexeme(displayName, nameBuf, sizeof nameBuf);

    const Node* found = findInSupertypes(owner, displayName);
    if (!found) {
        redefError(targetQname->line,
                   "Cannot redefine '%s': no such feature in any supertype.",
                   nameBuf);
        return;
    }

    /* Qualifier-prefix validation: for `Q::name`, verify that `Q` is
     * an ancestor through which `name` reaches `found`.  Currently
     * handles single-qualifier (n==2); deeper paths are accepted
     * tentatively as we don't yet validate full nested qualifiers. */
    if (n == 2) {
        Token qualifier = targetQname->as.qualifiedName.parts[0];
        if (!qualifierMatches(owner, qualifier, displayName, found)) {
            char qualBuf[128];
            nameLexeme(qualifier, qualBuf, sizeof qualBuf);
            redefError(targetQname->line,
                       "Cannot redefine '%s::%s': '%s' is not a supertype "
                       "from which '%s' is reachable.",
                       qualBuf, nameBuf, qualBuf, nameBuf);
            return;
        }
    }

    /* Stamp the resolution. */
    targetQname->as.qualifiedName.resolved = found;

    /* The redefined target must be a feature — attribute or usage.
     * Datatype/part definitions are specialized via :>, not redefined. */
    if (found->kind != NODE_ATTRIBUTE && found->kind != NODE_USAGE) {
        redefError(targetQname->line,
                   "Cannot redefine '%s': target is not a feature.",
                   nameBuf);
        return;
    }

    /* Mixed-kind redefs (e.g. attribute redefining a usage) are flagged
     * as a category mismatch.                                        */
    if (redefiningAttr->kind == NODE_ATTRIBUTE && found->kind != NODE_ATTRIBUTE) {
        redefError(targetQname->line,
                   "Attribute cannot redefine non-attribute feature '%s'.",
                   nameBuf);
        return;
    }

    /* Type compatibility — narrowing only.  Skipped when either side
     * has no declared type (the redefining feature may inherit its
     * type from the redefined target).
     *
     * Multi-qualifier paths (n > 2) skip the compat check because the
     * full path isn't yet validated. */
    bool fullyValidated = (n <= 2);
    const Node* redefType = declaredType(redefiningAttr);
    const Node* targetType = declaredType(found);
    if (fullyValidated && redefType && targetType
            && !specializesType(redefType, targetType)) {
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
