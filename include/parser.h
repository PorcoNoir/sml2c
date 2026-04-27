/* sml2c — parser.h */
#ifndef SML2C_PARSER_H
#define SML2C_PARSER_H

#include <stdbool.h>
#include "ast.h"

/* Parse the source initialized in the scanner and return the AST root,
 * or NULL if a fatal error happened.  Even on a non-NULL return, the
 * caller should check parserHadError() before using the tree —
 * panic-mode recovery may have produced a tree with stub nodes. */
Node* parse(void);
bool  parserHadError(void);

#endif /* SML2C_PARSER_H */
