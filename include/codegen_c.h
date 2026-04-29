/* sysmlc — codegen_c.h
 *
 * Emits the typed AST as a C header.  First-slice scope:
 *
 *   - Top-level attributes:
 *       static const TYPE NAME = EXPR;
 *
 *   - Part / item / attribute defs whose direct members are all
 *     attributes of supported primitive type:
 *       typedef struct { ... } NAME;
 *
 *   - Everything else is emitted as a comment line of the form
 *       skipped: NAME (reason)
 *     so the output remains a hint of what was lost rather than
 *     silent truncation.
 *
 * Primitive type mapping:
 *
 *     Real     to  double
 *     Integer  to  long long
 *     Boolean  to  bool
 *     String   to  const char*
 *
 * Source of truth for types is the typechecker's cached
 * inferredType (v0.14) on expression nodes plus the resolver's
 * resolvedTo pointer on type-position qualified names.  Codegen
 * never recomputes types; it just reads.
 *
 * Output is a self-contained translation unit: includes <stdbool.h>
 * and <stdint.h> at the top, no further dependencies.  cc
 * -fsyntax-only -x c -  should accept any program we emit; the
 * test-c make target verifies this.
 */
#ifndef SYSMLC_CODEGEN_C_H
#define SYSMLC_CODEGEN_C_H

#include <stdio.h>
#include "ast.h"

/* Emit `program` as C to `out`.  Output ends with a trailing newline.
 * Never errors — the AST is just walked.                            */
void emitC(FILE* out, const Node* program);

#endif /* SYSMLC_CODEGEN_C_H */
