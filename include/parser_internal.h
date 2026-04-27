/* sysmlc — parser_internal.h  (private)
 *
 * Internal interface shared by the three parser source files:
 *   - parser_common.c — Parser struct, error reporting, token machinery,
 *                       multiplicity, qualifiedName, list helpers.
 *   - parser_expr.c   — Pratt expression parser.
 *   - parser_decl.c   — declaration grammar + parse() entry point.
 *
 * NOT part of the public API — consumers see only include/parser.h.
 *
 * The split is purely organizational; behavior is identical to the
 * single-file version that preceded it.  The Parser state and a few
 * common helpers are now `extern` rather than `static`, so the linker
 * can see them across translation units, but they remain effectively
 * file-local in spirit (only the three parser files include this).
 */
#ifndef SYSMLC_PARSER_INTERNAL_H
#define SYSMLC_PARSER_INTERNAL_H

#include <stdbool.h>
#include "scanner.h"
#include "ast.h"

/* ------------------------------------------------- Parser state */

typedef struct {
    Token current;
    Token previous;
    bool  hadError;
    bool  panicMode;
} Parser;

/* The single Parser instance.  Lives in parser_common.c. */
extern Parser parser;

/* ------------------------------------------------- error reporting */

void errorAt(const Token* token, const char* message);
void error(const char* message);            /* errorAt(parser.previous) */
void errorAtCurrent(const char* message);   /* errorAt(parser.current)  */

/* ------------------------------------------------- token machinery */

void advance(void);
bool check(TokenType type);
bool match(TokenType type);
void consume(TokenType type, const char* message);
void synchronize(void);

/* ------------------------------------------------- common parser helpers */

/* Convert a TOKEN_NUMBER's lexeme to a long. */
long tokenToLong(Token t);

/* Parse a multiplicity bracket — caller has consumed '['. */
Node* parseMultiplicity(void);

/* Parse `qualifiedName (',' qualifiedName)*` and append into `list`. */
void appendQualifiedNameList(NodeList* list);

/* Parse a single qualified name (with optional `~` conjugation prefix). */
Node* qualifiedName(void);

/* ------------------------------------------------- feature relationships */

typedef struct {
    NodeList types;
    NodeList specializes;
    NodeList redefines;
    Node*    multiplicity;
} FeatureRels;

/* Parse the optional `: T :> S :>> R [m]` tail shared by usages and
 * attributes.  Lives in parser_decl.c since it's a declaration-side
 * construct, but parser_expr.c doesn't need it. */
FeatureRels parseFeatureRelationships(void);

/* ------------------------------------------------- expression entry */

/* Top-level expression parser; lives in parser_expr.c. */
Node* expression(void);

/* ------------------------------------------------- declaration entry */

/* The declaration dispatcher; lives in parser_decl.c.  Used by
 * the package, definition, and usage body parsers when they
 * recurse into nested members. */
Node* declaration(void);

#endif /* SYSMLC_PARSER_INTERNAL_H */
