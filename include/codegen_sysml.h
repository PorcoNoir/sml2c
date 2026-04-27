/* sysmlc — codegen_sysml.h
 *
 * Pretty-prints the resolved AST back as canonical SysML v2 source.
 * The output is parseable and, when re-parsed, produces an AST
 * structurally equivalent to the input.  Round-trip support is the
 * primary correctness criterion for this back-end.
 *
 * Style choices that aren't dictated by the grammar:
 *   - 4-space indentation
 *   - members on separate lines, blank-line-separated when nontrivial
 *   - relationship clauses joined with ", " (single space)
 *   - imports rendered with `::*` for wildcard
 *   - feature modifiers in canonical order: derived abstract constant ref
 *   - `::` always for qualified-name separators (we drop dots that the
 *     parser tolerates as input)
 *   - expressions parenthesised by structural precedence — every binary
 *     gets parens around it for unambiguous round-trip
 *
 * Things deliberately not preserved:
 *   - source comments (// line, slash-star block) are discarded at parse
 *   - whitespace and blank-line layout of the original
 *   - choice between `::` and `.` separators in qualified names
 *
 * Doc comments (slash-star-star ... star-slash) and `comment` declarations
 * ARE preserved — they live in the AST and round-trip cleanly.
 */
#ifndef SYSMLC_CODEGEN_SYSML_H
#define SYSMLC_CODEGEN_SYSML_H

#include <stdio.h>
#include "ast.h"

/* Emit `program` as canonical SysML to `out`.  Output ends with a
 * trailing newline.  The function never errors — the AST is just
 * walked. */
void emitSysml(FILE* out, const Node* program);

#endif /* SYSMLC_CODEGEN_SYSML_H */
