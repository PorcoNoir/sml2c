/* sysmlc — typechecker.h
 *
 * Bottom-up expression typing pass.  Runs after the resolver and
 * relies on every reference's `resolved` field being filled in.
 *
 * v0.x scope:
 *   - For each NODE_ATTRIBUTE that has a defaultValue, infer the
 *     type of the expression and check that it specializes the
 *     attribute's declared type.
 *   - Inside expressions, check that:
 *       arithmetic operators (+ - * /) take numeric operands
 *       comparison operators (< <= > >=) take numeric operands
 *       equality operators (== !=) take compatible operands
 *       unary '-' takes a numeric operand
 *       unary '!' takes a boolean operand
 *
 * When a sub-expression's type cannot be determined (because the
 * resolver tentatively accepted a name from an unloaded library),
 * the typechecker is permissive — it propagates "unknown" up the
 * tree rather than reporting a spurious error.
 *
 * Errors are written to stderr in the format used elsewhere:
 *   [line N] Type error: ...
 *
 * Returns true if no errors were reported.
 */
#ifndef SYSMLC_TYPECHECKER_H
#define SYSMLC_TYPECHECKER_H

#include <stdbool.h>
#include "ast.h"

bool typecheckProgram(const Node* program);

#endif /* SYSMLC_TYPECHECKER_H */
