/* sysmlc — redefchecker.h
 *
 * Validates `:>>` (redefines) clauses on attributes.
 *
 * For every attribute with a non-empty redefines list, the checker:
 *   1. Looks up each redef target in the enclosing definition's
 *      transitive specializes chain.  This is the proper lookup
 *      procedure for redefinition — local-scope lookup would
 *      self-resolve, since the redefining attribute is also named
 *      the same.
 *   2. Stamps the resolution into the qualifiedName's `resolved`
 *      field, fixing up what the resolver intentionally skipped.
 *   3. Verifies the target is itself an attribute (you can't
 *      redefine a definition or non-feature node).
 *   4. Verifies the redefining attribute's declared type specializes
 *      the redefined target's type — narrowing is allowed,
 *      widening is not.
 *
 * v0.x scope: only single-segment redef targets (e.g. `:>> torque`)
 * are checked.  Multi-segment targets (`:>> Engine::torque`) are
 * silently accepted; they need member-of-type resolution that the
 * project doesn't yet provide.
 *
 * Usage redefinitions (`part p :>> q`) are also out of scope for this
 * pass — their lookup procedure differs from attribute redefinition
 * (siblings in enclosing scope rather than members of supertypes).
 *
 * Returns true on success, false if any error was reported.
 */
#ifndef SYSMLC_REDEFCHECKER_H
#define SYSMLC_REDEFCHECKER_H

#include <stdbool.h>
#include "ast.h"

bool checkRedefinitions(Node* program);

#endif /* SYSMLC_REDEFCHECKER_H */
