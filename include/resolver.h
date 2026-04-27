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
 *      in the scope chain (with wildcard imports as a fallback).
 *      The resolution result is stamped into the node's `resolved`
 *      field for downstream passes to use.
 *
 * Errors are printed to stderr in the same style as the parser:
 *   [line N] Error: Undefined name 'Foo'.
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

#endif /* SYSMLC_RESOLVER_H */
