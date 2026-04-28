/* sysmlc — parser_common.c
 *
 * Parser state and the building blocks every other parser file calls.
 * See parser_internal.h for the full split.  Functions here used to be
 * `static` when parser.c was one file; promoted to extern so the other
 * two files can call them.
 *
 * Error model: panic mode (Crafting Interpreters Ch.6.5).  On the first
 * error inside a rule, we set panicMode and stop reporting *new* errors;
 * top-level loops detect panicMode and call synchronize() to skip until
 * a known-safe token (a semicolon, a close-brace, or a keyword that
 * begins a fresh declaration), then clear the flag and continue.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser_internal.h"

/* The single Parser instance.  Declared in parser_internal.h. */
Parser parser;

/* -------------------------------------------------- error reporting */

void errorAt(const Token* token, const char* message) {
    if (parser.panicMode) return;       /* swallow cascading errors  */
    parser.panicMode = true;
    fprintf(stderr, "[line %d] Error", token->line);
    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type != TOKEN_ERROR) {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }
    fprintf(stderr, ": %s\n", message);
    parser.hadError = true;
}

void error(const char* message)        { errorAt(&parser.previous, message); }
void errorAtCurrent(const char* m)     { errorAt(&parser.current,  m);       }

/* ------------------------------------------------- token machinery */

void advance(void) {
    parser.previous = parser.current;

    /* Context-sensitive lexing: if we just consumed `doc`, the next
     * thing in the source is a comment-style description body that
     * scanToken's skipWhitespace would normally throw away.  Route
     * through the specialised entry point instead.                   */
    if (parser.previous.type == TOKEN_DOC) {
        parser.current = scanDocBody();
        if (parser.current.type == TOKEN_ERROR) {
            errorAtCurrent(parser.current.start);
        }
        return;
    }

    for (;;) {
        parser.current = scanToken();
        if (parser.current.type != TOKEN_ERROR) break;
        /* Scanner reports its error message in the token's lexeme. */
        errorAtCurrent(parser.current.start);
    }
}

bool check(TokenType type) { return parser.current.type == type; }

bool match(TokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

void consume(TokenType type, const char* message) {
    if (check(type)) { advance(); return; }
    errorAtCurrent(message);
}

/* --------------------------------------------------- panic recovery
 *
 * Walk forward until we find a token where it's safe to start parsing
 * a fresh declaration: just past a ';', at a '}' that closes our
 * scope, or at a keyword that begins a new top-level form.
 */
void synchronize(void) {
    parser.panicMode = false;
    while (!check(TOKEN_EOF)) {
        if (parser.previous.type == TOKEN_SEMICOLON) return;
        switch (parser.current.type) {
        case TOKEN_PACKAGE:
        case TOKEN_IMPORT:
        case TOKEN_PART:
        case TOKEN_PORT:
        case TOKEN_INTERFACE:
        case TOKEN_ITEM:
        case TOKEN_CONNECTION:
        case TOKEN_FLOW:
        case TOKEN_END:
        case TOKEN_CONNECT:
        case TOKEN_ATTRIBUTE:
        case TOKEN_DOC:
        case TOKEN_COMMENT_KW:
        case TOKEN_ALIAS:
        case TOKEN_DEPENDENCY:
        case TOKEN_ENUM:
        case TOKEN_CONSTRAINT:
        case TOKEN_REQUIREMENT:
        case TOKEN_ASSERT:
        case TOKEN_ASSUME:
        case TOKEN_REQUIRE:
        case TOKEN_SUBJECT:
        case TOKEN_ACTION:
        case TOKEN_SUCCESSION:
        case TOKEN_FIRST:
        case TOKEN_THEN:
        case TOKEN_STATE:
        case TOKEN_EXHIBIT:
        case TOKEN_TRANSITION:
        case TOKEN_ENTRY:
        case TOKEN_EXIT:
        case TOKEN_DO:
        case TOKEN_PERFORM:
        case TOKEN_RETURN:
        case TOKEN_CALC:
        case TOKEN_ABSTRACT:    /* feature modifiers can begin a decl */
        case TOKEN_DERIVED:
        case TOKEN_CONSTANT:
        case TOKEN_REF:
        case TOKEN_RIGHT_BRACE:
            return;
        default: ;
        }
        advance();
    }
}

/* -------------------------------------- common parser helpers */

/* Convert a TOKEN_NUMBER's lexeme to a long.  We copy into a local
 * buffer because the token isn't null-terminated. */
long tokenToLong(Token t) {
    char buf[64];
    int n = (t.length < 63) ? t.length : 63;
    memcpy(buf, t.start, (size_t)n);
    buf[n] = '\0';
    return strtol(buf, NULL, 10);
}

/*  multiplicity ::= "[" ( NUMBER | "*" ) ( ".." ( NUMBER | "*" ) )? "]"
 *  Caller has already consumed the opening '['.                       */
Node* parseMultiplicity(void) {
    int line = parser.previous.line;
    Node* m = astMakeNode(NODE_MULTIPLICITY, line);
    /* All fields zeroed by astMakeNode's calloc. */

    /* ---- lower bound (or sole value) -------------------------------- */
    if (match(TOKEN_STAR)) {
        m->as.multiplicity.lowerWildcard = true;
    } else if (match(TOKEN_NUMBER)) {
        m->as.multiplicity.lower = tokenToLong(parser.previous);
    } else {
        errorAtCurrent("Expected number or '*' in multiplicity.");
    }

    /* ---- optional ".." upper bound ---------------------------------- */
    if (match(TOKEN_DOT_DOT)) {
        m->as.multiplicity.isRange = true;
        if (match(TOKEN_STAR)) {
            m->as.multiplicity.upperWildcard = true;
        } else if (match(TOKEN_NUMBER)) {
            m->as.multiplicity.upper = tokenToLong(parser.previous);
        } else {
            errorAtCurrent("Expected number or '*' after '..'.");
        }
    }

    consume(TOKEN_RIGHT_BRACKET, "Expected ']' to close multiplicity.");
    return m;
}

/* Helper: consume `qualifiedName (',' qualifiedName)*` into a list. */
void appendQualifiedNameList(NodeList* list) {
    astListAppend(list, qualifiedName());
    while (match(TOKEN_COMMA)) {
        astListAppend(list, qualifiedName());
    }
}

/*  qualifiedName ::= [ "~" ] IDENTIFIER ( ("::" | ".") IDENTIFIER )*
 *
 *  The optional `~` prefix marks port conjugation (§8.3.12.3).  The
 *  parser accepts it on any qname; semantic validity ("only on a port
 *  type ref") is enforced by later passes.                            */
Node* qualifiedName(void) {
    Node* q = astMakeNode(NODE_QUALIFIED_NAME, parser.current.line);
    if (match(TOKEN_TILDE)) {
        q->as.qualifiedName.isConjugated = true;
    }
    if (!check(TOKEN_IDENTIFIER)) {
        errorAtCurrent("Expected name.");
        return q;
    }
    advance();
    astAppendQualifiedPart(q, parser.previous);
    while (check(TOKEN_COLON_COLON) || check(TOKEN_DOT)) {
        advance();                              /* eat :: or .       */
        if (!check(TOKEN_IDENTIFIER)) {
            error("Expected name after '::' or '.'.");
            break;
        }
        advance();
        astAppendQualifiedPart(q, parser.previous);
    }
    return q;
}
