/* sml2c — scanner.h
 *
 * Lexical analyzer for a subset of SysML v2 (.sysml files).
 *
 * Mirrors the structure of clox's scanner from Crafting Interpreters Ch. 16:
 *   - one-shot scanToken() pulled lazily by the parser,
 *   - tokens hold (start, length) into the source buffer (no string copy),
 *   - global static scanner state (single-threaded compiler).
 *
 * v0.1 token set: enough for `package`, `import`, `part def`, `part`,
 * `attribute`, plus the literals/operators we'll need for expressions later.
 */
#ifndef SML2C_SCANNER_H
#define SML2C_SCANNER_H

typedef enum {
    /* Single-character punctuation ----------------------------------- */
    TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE,           /* { } */
    TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN,           /* ( ) */
    TOKEN_LEFT_BRACKET, TOKEN_RIGHT_BRACKET,       /* [ ] */
    TOKEN_SEMICOLON, TOKEN_COMMA, TOKEN_DOT,       /* ; , . */
    TOKEN_PLUS, TOKEN_MINUS,                       /* + - */
    TOKEN_STAR, TOKEN_SLASH,                       /* * / */
    TOKEN_BANG,                                    /* ! */

    /* Tokens that may be one or two characters ----------------------- */
    TOKEN_COLON, TOKEN_COLON_COLON,                /* : :: */
    TOKEN_EQUAL, TOKEN_EQUAL_EQUAL,                /* = == */
    TOKEN_BANG_EQUAL,                              /* != */
    TOKEN_LESS, TOKEN_LESS_EQUAL,                  /* < <= */
    TOKEN_GREATER, TOKEN_GREATER_EQUAL,            /* > >= */

    /* Literals ------------------------------------------------------- */
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING,

    /* Keywords (v0.1) ----------------------------------------------- */
    TOKEN_PACKAGE,
    TOKEN_IMPORT,
    TOKEN_PART,
    TOKEN_DEF,
    TOKEN_ATTRIBUTE,
    TOKEN_REF,
    TOKEN_TRUE, TOKEN_FALSE,

    /* Special -------------------------------------------------------- */
    TOKEN_ERROR,
    TOKEN_EOF
} TokenType;

typedef struct {
    TokenType   type;
    const char* start;   /* points into the original source buffer    */
    int         length;  /* number of bytes, NOT null-terminated     */
    int         line;    /* 1-based, for diagnostics                  */
} Token;

/* The source buffer must outlive every Token returned. */
void  initScanner(const char* source);
Token scanToken(void);

/* Pretty name for debugging / test harness. */
const char* tokenTypeName(TokenType type);

#endif /* SML2C_SCANNER_H */