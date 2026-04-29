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
    /* If the scope belongs to a usage/definition body whose declared
     * type or supertype carries inherited features, this points at
     * that type/supertype's NODE_DEFINITION (or NODE_USAGE).  Name
     * lookup falls back to lookupMember on this node when the local
     * symbol table doesn't have a hit, so a feature inherited from
     * the type — for instance an `include use case foo;` declared in
     * UC, when the current scope belongs to `t : UC` — is reachable
     * by its bare name from inside the body.                         */
    const Node*   inheritedFrom;
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

Symbol*     lookupLocal(const Scope* scope, Token name);
const Node* lookupChain(const Scope* scope, Token name);
void        declareName(Scope* scope, Token name, const Node* decl);
/* Variant of declareName that's silent on conflict: if `name` is
 * already declared in `scope`, leave the existing binding alone and
 * return false.  Used for soft declarations (re-exports, redefinition
 * aliases) where a real explicit decl always wins. */
bool        declareNameIfFree(Scope* scope, Token name, const Node* decl);
void        freeScope(Scope* scope);

/* ------------------------------------------------- name extraction */

/* tokensEqual and nodeName live in ast.h — they're not specific to
 * the resolver pass.  Re-declared here only as a courtesy reminder
 * that the resolver's scope primitives use them.                    */

/* Last identifier of a qualified-name node, or an empty token if the
 * node isn't a qname or has no parts. */
Token qnameLastSegment(const Node* qname);

/* First item of `list` if it's a qname with a resolved decl, else
 * NULL.  Used to pick a usage's primary inheritance source from
 * its types[] / specializes[] list. */
const Node* firstResolvedQname(const NodeList* list);

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
