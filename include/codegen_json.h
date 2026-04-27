/* sysmlc — codegen_json.h
 *
 * Emits the resolved AST as pretty-printed JSON.  A faithful dump:
 * every NodeKind, every variant field, plus the resolution targets
 * the resolver/redef-checker stamped onto qualified names.
 *
 * Useful as a debugging tool, as input to external tooling that
 * doesn't want to re-parse SysML, and as the project's first real
 * "back-end" — the moment we go from "consume source" to "produce
 * something downstream."
 */
#ifndef SYSMLC_CODEGEN_JSON_H
#define SYSMLC_CODEGEN_JSON_H

#include <stdio.h>
#include "ast.h"

/* Emit `program` as JSON to `out`.  Output ends with a trailing
 * newline.  The function never errors — the AST is just walked. */
void emitJson(FILE* out, const Node* program);

#endif /* SYSMLC_CODEGEN_JSON_H */
