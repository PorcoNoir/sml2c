/* sysmlc — resolver_internal.h  (private)
 *
 * Internal interface shared by the two resolver source files:
 *   - resolver_scope.c — Scope/Symbol primitives, error reporting,
 *                        member-of-type lookup, alias deref, wildcard
 *                        import search.  The "data layer."
 *   - resolver.c       — resolution drivers and phase orchestration.
 *                        The walker.
 *
 * NOT part of the public API — consumers see only include/resolver.h.
 *
 * The split is purely organizational; behavior is identical to the
 * single-file version that preceded it.
 */
#ifndef SYSMLC_RESOLVER_INTERNAL_H
#define SYSMLC_RESOLVER_INTERNAL_H

#include <stdbool.h>
#include "ast.h"
#include "scanner.h"

/* ------------------------------------------------- symbol table */

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

/* ------------------------------------------------- error counter */

/* Lives in resolver_scope.c.  Reset by resolveProgram before each
 * pass; incremented by every error-reporting helper.                */
extern int resolverErrorCount;

void undefinedNameError(int line, Token name);
void undefinedMemberError(int line, Token name, const Node* container);
void duplicateNameError(int line, Token name, int prevLine);

/* ------------------------------------------------- scope primitives */

bool        tokensEqual(Token a, Token b);
Symbol*     lookupLocal(const Scope* scope, Token name);
const Node* lookupChain(const Scope* scope, Token name);
void        declareName(Scope* scope, Token name, const Node* decl);
void        freeScope(Scope* scope);

/* ------------------------------------------------- name extraction */

Token nodeName(const Node* n);

/* ------------------------------------------------- member-of-type lookup */

/* Public entry point (also exported via resolver.h). */
const Node* lookupMember(const Node* container, Token name);

/* Internal entry with depth bound for cycle prevention.  Used by
 * resolveQualifiedNameImpl when walking multi-segment chains.       */
const Node* lookupMemberDepth(const Node* container, Token name, int depth);

/* ------------------------------------------------- wildcard imports */

const Node* searchWildcardImports(const Scope* scope, Token name);
bool        anyUnresolvedWildcardInScope(const Scope* scope);

/* ------------------------------------------------- alias deref */

/* Walk an alias chain (`alias E for X` followed by `alias F for E`...)
 * to its underlying target.  Returns the final non-alias node, or
 * NULL if the chain hits an unresolved alias or exceeds depth 32.   */
const Node* derefAlias(const Node* n);

#endif /* SYSMLC_RESOLVER_INTERNAL_H */
