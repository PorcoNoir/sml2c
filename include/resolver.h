/* sysmlc — resolver.h
 *
 * Semantic analysis pass that runs after parsing and before any
 * downstream consumer (validator, code generator).  It does two
 * jobs in v0.x:
 *
 *   1. Symbol-table construction.  Every named declaration in the AST
 *      is registered in the scope that contains it.
 *
 *   2. Reference resolution.  Every NODE_QUALIFIED_NAME is looked up
 *      segment by segment: the first segment via the scope chain
 *      (with wildcard imports as a fallback), each subsequent
 *      segment as a member of the previous segment's resolution.
 *      The final binding is stamped into the node's `resolved`
 *      field for downstream passes to use.
 *
 * Errors are printed to stderr in the same style as the parser:
 *   [line N] Error: Undefined name 'Foo'.
 *   [line N] Error: 'X' has no member 'Y'.
 *
 * The resolver mutates the AST in place (specifically the `resolved`
 * field).  No new nodes are created.
 *
 * Returns true if no errors were reported, false otherwise.
 */
#ifndef SYSMLC_RESOLVER_H
#define SYSMLC_RESOLVER_H

#include <stdbool.h>
#include "ast.h"

bool resolveProgram(Node* program);

/* Public utility: look up `name` as a (possibly inherited) member of
 * `container`.  The search rules vary by container kind:
 *
 *   PROGRAM, PACKAGE          : direct members only
 *   DEFINITION                : direct members + members of supertypes
 *                               (transitively, via the specializes chain)
 *   USAGE                     : direct body members + members of types
 *                               (the `: T` link) + members of the usage's
 *                               own specializes targets
 *   ATTRIBUTE                 : members of the attribute's type
 *                               (so `attribute x : T` lets you reach T's
 *                                members via x)
 *
 * Returns NULL when no such member exists.  All returned references
 * walk through `qualifiedName.resolved` fields, so this function
 * requires the resolver to have already populated them — call this
 * only after resolveProgram has run.                                */
const Node* lookupMember(const Node* container, Token name);

#endif /* SYSMLC_RESOLVER_H */
