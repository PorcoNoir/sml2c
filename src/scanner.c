/* sysmlc — scanner.c */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "scanner.h"

/* ---------------------------------------------------------------- state */

typedef struct {
    const char* start;    /* start of the token currently being scanned   */
    const char* current;  /* next character to consume                    */
    int         line;     /* current source line                          */
    /* One-slot rolling save of the most-recently-skipped block
     * comment body.  skipWhitespace clears this on entry and writes it
     * when a block is consumed.  takeLastBlockComment() returns and
     * clears it.  The parser uses this to recover bodies that the
     * scanner ate before commentDecl could ask for them. */
    Token       lastBlock;
} Scanner;

static Scanner scanner;

void initScanner(const char* source) {
    scanner.start     = source;
    scanner.current   = source;
    scanner.line      = 1;
    scanner.lastBlock = (Token){0};
}

Token takeLastBlockComment(void) {
    Token t = scanner.lastBlock;
    scanner.lastBlock = (Token){0};
    return t;
}

/* ---------------------------------------------------------- low-level IO */

static bool isAtEnd(void)        { return *scanner.current == '\0'; }
static char advance(void)        { return *scanner.current++;       }
static char peek(void)           { return *scanner.current;         }
static char peekNext(void)       { return isAtEnd() ? '\0' : scanner.current[1]; }

static bool match(char expected) {
    if (isAtEnd() || *scanner.current != expected) return false;
    scanner.current++;
    return true;
}

static bool isDigit(char c) { return c >= '0' && c <= '9'; }
static bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/* ---------------------------------------------------- token construction */

static Token makeToken(TokenType type) {
    Token t;
    t.type   = type;
    t.start  = scanner.start;
    t.length = (int)(scanner.current - scanner.start);
    t.line   = scanner.line;
    return t;
}

static Token errorToken(const char* msg) {
    Token t;
    t.type   = TOKEN_ERROR;
    t.start  = msg;
    t.length = (int)strlen(msg);
    t.line   = scanner.line;
    return t;
}

/* ----------------------------------------------- whitespace and comments
 *
 * SysML v2 has // line comments, slash-star block comments, and
 * slash-double-star doc comments.  We swallow the first two here so
 * scanToken() always starts at a real character.  The doc-comment
 * form is NOT swallowed — we leave it in place so scanToken() can
 * capture it as a TOKEN_DOC_BODY.  An empty block comment
 * (slash-star-star-slash) is still swallowed as a regular block
 * comment.
 */
static void skipWhitespace(void) {
    /* Each scanToken() begins with one skipWhitespace() pass, so it's
     * the right place to reset the rolling block-comment save: anything
     * earlier than this pass is no longer adjacent to the next token. */
    scanner.lastBlock = (Token){0};

    for (;;) {
        char c = peek();
        switch (c) {
        case ' ': case '\r': case '\t':
            advance();
            break;
        case '\n':
            scanner.line++;
            advance();
            break;
        case '/':
            if (peekNext() == '/') {
                while (peek() != '\n' && !isAtEnd()) advance();
            } else if (peekNext() == '*') {
                /* Distinguish slash-star-star doc comment from a plain
                 * slash-star block comment.  An empty block comment
                 * (slash-star-star-slash, four chars) is treated as a
                 * regular empty block comment, not a doc comment. */
                if (scanner.current[2] == '*' && scanner.current[3] != '/') {
                    return;                     /* leave for scanToken to capture */
                }
                int blockLine = scanner.line;
                advance(); advance();           /* consume "/" "*" */
                const char* bodyStart = scanner.current;
                const char* bodyEnd   = bodyStart;
                while (!isAtEnd()) {
                    if (peek() == '*' && peekNext() == '/') {
                        bodyEnd = scanner.current;
                        advance(); advance();   /* consume "*" "/" */
                        break;
                    }
                    if (peek() == '\n') scanner.line++;
                    advance();
                }
                /* Save the body content (without delimiters) into the
                 * rolling slot.  Last block in the run wins, which is
                 * what commentDecl wants. */
                scanner.lastBlock.type   = TOKEN_DOC_BODY;
                scanner.lastBlock.start  = bodyStart;
                scanner.lastBlock.length = (int)(bodyEnd - bodyStart);
                scanner.lastBlock.line   = blockLine;
            } else {
                return;                         /* a real / division op */
            }
            break;
        default:
            return;
        }
    }
}

/* ------------------------------------------------ identifiers & keywords
 *
 * We scan the whole word, then check whether the lexeme matches one of
 * our keywords.  For 8 keywords a flat memcmp chain is fine.  Crafting
 * Interpreters Ch.16.4 uses a hand-rolled trie for performance; we'll
 * switch to that once the keyword set grows past ~20.
 */
static TokenType identifierType(void) {
    int len = (int)(scanner.current - scanner.start);

    #define KW(s, tok) \
        do { if (len == (int)(sizeof(s) - 1) && \
                 memcmp(scanner.start, s, sizeof(s) - 1) == 0) return tok; } while (0)

    KW("package",     TOKEN_PACKAGE);
    KW("import",      TOKEN_IMPORT);
    KW("part",        TOKEN_PART);
    KW("port",        TOKEN_PORT);
    KW("interface",   TOKEN_INTERFACE);
    KW("item",        TOKEN_ITEM);
    KW("connection",  TOKEN_CONNECTION);
    KW("flow",        TOKEN_FLOW);
    KW("def",         TOKEN_DEF);
    KW("attribute",   TOKEN_ATTRIBUTE);
    KW("ref",         TOKEN_REF);
    KW("abstract",    TOKEN_ABSTRACT);
    KW("derived",     TOKEN_DERIVED);
    KW("constant",    TOKEN_CONSTANT);
    KW("specializes", TOKEN_SPECIALIZES);
    KW("redefines",   TOKEN_REDEFINES);
    KW("public",      TOKEN_PUBLIC);
    KW("private",     TOKEN_PRIVATE);
    KW("protected",   TOKEN_PROTECTED);
    KW("doc",         TOKEN_DOC);
    KW("comment",     TOKEN_COMMENT_KW);
    KW("about",       TOKEN_ABOUT);
    KW("alias",       TOKEN_ALIAS);
    KW("for",         TOKEN_FOR);
    KW("dependency",  TOKEN_DEPENDENCY);
    KW("enum",        TOKEN_ENUM);
    KW("in",          TOKEN_IN);
    KW("out",         TOKEN_OUT);
    KW("inout",       TOKEN_INOUT);
    KW("connect",     TOKEN_CONNECT);
    KW("to",          TOKEN_TO);
    KW("from",        TOKEN_FROM);
    KW("end",         TOKEN_END);
    KW("true",        TOKEN_TRUE);
    KW("false",       TOKEN_FALSE);

    #undef KW
    return TOKEN_IDENTIFIER;
}

static Token identifier(void) {
    while (isAlpha(peek()) || isDigit(peek())) advance();
    return makeToken(identifierType());
}

/* -------------------------------------------------------------- numbers
 *
 * Integer:  123
 * Real:     123.456    (digit required after '.', so "3.foo" stays "3" "." "foo")
 *
 * We do NOT yet handle:
 *   - sign (parser handles unary -),
 *   - exponent form (1.5e3),
 *   - SysML quantity-with-units literals like `5 [kg]` (that's syntax,
 *     not a single token: NUMBER LBRACKET IDENT RBRACKET).
 */
static Token number(void) {
    while (isDigit(peek())) advance();
    if (peek() == '.' && isDigit(peekNext())) {
        advance();                    /* consume '.' */
        while (isDigit(peek())) advance();
    }
    return makeToken(TOKEN_NUMBER);
}

/* -------------------------------------------------------------- strings
 *
 * "..." with newlines allowed (we increment .line so error messages are
 * still sensible).  No escape processing in v0.1 — we'll add \" \n etc.
 * when a feature actually needs them.
 */
static Token string(void) {
    while (peek() != '"' && !isAtEnd()) {
        if (peek() == '\n') scanner.line++;
        advance();
    }
    if (isAtEnd()) return errorToken("Unterminated string.");
    advance();                        /* closing quote */
    return makeToken(TOKEN_STRING);
}

/* ----------------------------------------------------------- doc bodies
 *
 * Two paths reach a TOKEN_DOC_BODY.  In both, the token's lexeme spans
 * just the inner content — the slash-star and star-slash delimiters
 * are stripped, so the AST stores cleanly-printable text.
 *
 *   Path A (implicit slash-star-star form):  scanToken() detects the
 *                                   leading double star and calls
 *                                   captureCommentInterior.
 *   Path B (explicit doc keyword form):  the parser calls scanDocBody()
 *                                   after consuming the doc keyword.
 */
static Token captureCommentInterior(int openDelim, int closeDelim) {
    /* Caller has consumed the opening "/" "*" (and any leading "*"s for
     * the doc-comment form).  scanner.current points at the first byte
     * of the body. */
    (void)openDelim; (void)closeDelim;          /* reserved for future use */
    const char* bodyStart = scanner.current;
    while (!isAtEnd()) {
        if (peek() == '*' && peekNext() == '/') {
            int len = (int)(scanner.current - bodyStart);
            advance(); advance();               /* consume */ ;
            Token t;
            t.type   = TOKEN_DOC_BODY;
            t.start  = bodyStart;
            t.length = len;
            t.line   = scanner.line;
            return t;
        }
        if (peek() == '\n') scanner.line++;
        advance();
    }
    return errorToken("Unterminated doc comment.");
}

Token scanDocBody(void) {
    /* Skip whitespace and line comments (but NOT block comments — the
     * very next slash-star is what we want to capture).               */
    for (;;) {
        char c = peek();
        if      (c == ' ' || c == '\r' || c == '\t') advance();
        else if (c == '\n')   { scanner.line++; advance(); }
        else if (c == '/' && peekNext() == '/') {
            while (peek() != '\n' && !isAtEnd()) advance();
        }
        else break;
    }
    scanner.start = scanner.current;
    if (peek() != '/' || peekNext() != '*') {
        return errorToken("Expected '/* description */' after 'doc'.");
    }
    advance(); advance();                       /* consume opening / and * */
    /* The explicit form may use slash-star or slash-star-star;
     * tolerate the extra leading star either way. */
    if (peek() == '*' && peekNext() != '/') advance();
    return captureCommentInterior(0, 0);
}

/* --------------------------------------------------------- main entry */

Token scanToken(void) {
    skipWhitespace();
    scanner.start = scanner.current;

    if (isAtEnd()) return makeToken(TOKEN_EOF);

    /* Implicit doc-comment form (slash-star-star) that skipWhitespace
     * left for us.  We re-check here because skipWhitespace's contract
     * is just "didn't consume it".                                    */
    if (peek() == '/' && peekNext() == '*'
        && scanner.current[2] == '*' && scanner.current[3] != '/') {
        advance(); advance(); advance();        /* consume opening 3 chars */
        return captureCommentInterior(0, 0);
    }

    char c = advance();
    if (isAlpha(c)) return identifier();
    if (isDigit(c)) return number();

    switch (c) {
    case '(': return makeToken(TOKEN_LEFT_PAREN);
    case ')': return makeToken(TOKEN_RIGHT_PAREN);
    case '{': return makeToken(TOKEN_LEFT_BRACE);
    case '}': return makeToken(TOKEN_RIGHT_BRACE);
    case '[': return makeToken(TOKEN_LEFT_BRACKET);
    case ']': return makeToken(TOKEN_RIGHT_BRACKET);
    case ';': return makeToken(TOKEN_SEMICOLON);
    case ',': return makeToken(TOKEN_COMMA);
    case '.': return makeToken(match('.') ? TOKEN_DOT_DOT : TOKEN_DOT);
    case '+': return makeToken(TOKEN_PLUS);
    case '-': return makeToken(TOKEN_MINUS);
    case '*': return makeToken(TOKEN_STAR);
    case '/': return makeToken(TOKEN_SLASH);   /* comments handled above */
    case ':':
        if (match(':')) return makeToken(TOKEN_COLON_COLON);
        if (match('>')) {
            if (match('>')) return makeToken(TOKEN_COLON_GREATER_GREATER);
            return makeToken(TOKEN_COLON_GREATER);
        }
        return makeToken(TOKEN_COLON);
    case '=': return makeToken(match('=') ? TOKEN_EQUAL_EQUAL   : TOKEN_EQUAL);
    case '!': return makeToken(match('=') ? TOKEN_BANG_EQUAL    : TOKEN_BANG);
    case '~': return makeToken(TOKEN_TILDE);
    case '<': return makeToken(match('=') ? TOKEN_LESS_EQUAL    : TOKEN_LESS);
    case '>': return makeToken(match('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
    case '"': return string();
    }

    return errorToken("Unexpected character.");
}

/* -------------------------------------------------------- pretty names */

const char* tokenTypeName(TokenType type) {
    switch (type) {
    case TOKEN_LEFT_BRACE:     return "LBRACE";
    case TOKEN_RIGHT_BRACE:    return "RBRACE";
    case TOKEN_LEFT_PAREN:     return "LPAREN";
    case TOKEN_RIGHT_PAREN:    return "RPAREN";
    case TOKEN_LEFT_BRACKET:   return "LBRACKET";
    case TOKEN_RIGHT_BRACKET:  return "RBRACKET";
    case TOKEN_SEMICOLON:      return "SEMI";
    case TOKEN_COMMA:          return "COMMA";
    case TOKEN_DOT:            return "DOT";
    case TOKEN_DOT_DOT:        return "DOT_DOT";
    case TOKEN_PLUS:           return "PLUS";
    case TOKEN_MINUS:          return "MINUS";
    case TOKEN_STAR:           return "STAR";
    case TOKEN_SLASH:          return "SLASH";
    case TOKEN_BANG:           return "BANG";
    case TOKEN_TILDE:          return "TILDE";
    case TOKEN_COLON:          return "COLON";
    case TOKEN_COLON_COLON:    return "COLON_COLON";
    case TOKEN_COLON_GREATER:        return "COLON_GREATER";
    case TOKEN_COLON_GREATER_GREATER:return "COLON_GREATER_GREATER";
    case TOKEN_EQUAL:          return "EQUAL";
    case TOKEN_EQUAL_EQUAL:    return "EQUAL_EQUAL";
    case TOKEN_BANG_EQUAL:     return "BANG_EQUAL";
    case TOKEN_LESS:           return "LESS";
    case TOKEN_LESS_EQUAL:     return "LESS_EQUAL";
    case TOKEN_GREATER:        return "GREATER";
    case TOKEN_GREATER_EQUAL:  return "GREATER_EQUAL";
    case TOKEN_IDENTIFIER:     return "IDENT";
    case TOKEN_NUMBER:         return "NUMBER";
    case TOKEN_STRING:         return "STRING";
    case TOKEN_DOC_BODY:       return "DOC_BODY";
    case TOKEN_PACKAGE:        return "PACKAGE";
    case TOKEN_IMPORT:         return "IMPORT";
    case TOKEN_PART:           return "PART";
    case TOKEN_PORT:           return "PORT";
    case TOKEN_INTERFACE:      return "INTERFACE";
    case TOKEN_ITEM:           return "ITEM";
    case TOKEN_CONNECTION:     return "CONNECTION";
    case TOKEN_FLOW:           return "FLOW";
    case TOKEN_DEF:            return "DEF";
    case TOKEN_ATTRIBUTE:      return "ATTRIBUTE";
    case TOKEN_REF:            return "REF";
    case TOKEN_ABSTRACT:       return "ABSTRACT";
    case TOKEN_DERIVED:        return "DERIVED";
    case TOKEN_CONSTANT:       return "CONSTANT";
    case TOKEN_SPECIALIZES:    return "SPECIALIZES";
    case TOKEN_REDEFINES:      return "REDEFINES";
    case TOKEN_PUBLIC:         return "PUBLIC";
    case TOKEN_PRIVATE:        return "PRIVATE";
    case TOKEN_PROTECTED:      return "PROTECTED";
    case TOKEN_DOC:            return "DOC";
    case TOKEN_COMMENT_KW:     return "COMMENT_KW";
    case TOKEN_ABOUT:          return "ABOUT";
    case TOKEN_ALIAS:          return "ALIAS";
    case TOKEN_FOR:            return "FOR";
    case TOKEN_DEPENDENCY:     return "DEPENDENCY";
    case TOKEN_ENUM:           return "ENUM";
    case TOKEN_IN:             return "IN";
    case TOKEN_OUT:            return "OUT";
    case TOKEN_INOUT:          return "INOUT";
    case TOKEN_CONNECT:        return "CONNECT";
    case TOKEN_TO:             return "TO";
    case TOKEN_FROM:           return "FROM";
    case TOKEN_END:            return "END";
    case TOKEN_TRUE:           return "TRUE";
    case TOKEN_FALSE:          return "FALSE";
    case TOKEN_ERROR:          return "ERROR";
    case TOKEN_EOF:            return "EOF";
    }
    return "UNKNOWN";
}
