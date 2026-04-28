/* sysmlc — scanner.h
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
#ifndef SYSMLC_SCANNER_H
#define SYSMLC_SCANNER_H

typedef enum {
    /* Single-character punctuation ----------------------------------- */
    TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE,           /* { } */
    TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN,           /* ( ) */
    TOKEN_LEFT_BRACKET, TOKEN_RIGHT_BRACKET,       /* [ ] */
    TOKEN_SEMICOLON, TOKEN_COMMA, TOKEN_DOT,       /* ; , . */
    TOKEN_PLUS, TOKEN_MINUS,                       /* + - */
    TOKEN_STAR, TOKEN_SLASH,                       /* * / */
    TOKEN_STAR_STAR,                               /* ** — recursive-import wildcard / power op */
    TOKEN_BANG,                                    /* ! */
    TOKEN_TILDE,                                   /* ~     port conjugation */
    TOKEN_AT,                                      /* @     metadata application */
    TOKEN_HASH,                                    /* #     short-form metadata */

    /* Tokens that may be one, two, or three characters --------------- */
    TOKEN_COLON, TOKEN_COLON_COLON,                /* : ::   */
    TOKEN_COLON_GREATER,                           /* :>     specializes */
    TOKEN_COLON_GREATER_GREATER,                   /* :>>    redefines   */
    TOKEN_DOT_DOT,                                 /* ..     range op    */
    TOKEN_EQUAL, TOKEN_EQUAL_EQUAL,                /* = == */
    TOKEN_BANG_EQUAL,                              /* != */
    TOKEN_LESS, TOKEN_LESS_EQUAL,                  /* < <= */
    TOKEN_GREATER, TOKEN_GREATER_EQUAL,            /* > >= */

    /* Literals ------------------------------------------------------- */
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING,
    TOKEN_DOC_BODY,                                /* captured comment body */

    /* Keywords (v0.1) ----------------------------------------------- */
    TOKEN_PACKAGE,
    TOKEN_IMPORT,
    TOKEN_PART,
    TOKEN_PORT,
    TOKEN_INTERFACE,
    TOKEN_ITEM,
    TOKEN_CONNECTION,
    TOKEN_FLOW,
    TOKEN_DEF,
    TOKEN_ATTRIBUTE,
    TOKEN_REF,
    TOKEN_ABSTRACT,
    TOKEN_DERIVED,
    TOKEN_CONSTANT,
    TOKEN_SPECIALIZES,
    TOKEN_REDEFINES,
    TOKEN_SUBSETS,                                 /* `subsets X` — like :> for usages */
    TOKEN_PUBLIC,
    TOKEN_PRIVATE,
    TOKEN_PROTECTED,
    TOKEN_DOC,
    TOKEN_COMMENT_KW,
    TOKEN_ABOUT,
    TOKEN_ALIAS,
    TOKEN_FOR,
    TOKEN_DEPENDENCY,
    TOKEN_ENUM,
    TOKEN_IN,
    TOKEN_OUT,
    TOKEN_INOUT,
    TOKEN_CONNECT,
    TOKEN_TO,
    TOKEN_FROM,
    TOKEN_END,
    TOKEN_TRUE, TOKEN_FALSE,

    /* Constraints, requirements (turn 1 of behavioral expansion). ---- */
    TOKEN_CONSTRAINT,
    TOKEN_REQUIREMENT,
    TOKEN_ASSERT,
    TOKEN_ASSUME,
    TOKEN_REQUIRE,
    TOKEN_SUBJECT,

    /* Logical connectives — SysML uses the keyword forms `and` / `or`,
     * not `&&` / `||`.  Slot into the existing Pratt rules table at
     * PREC_AND / PREC_OR.                                              */
    TOKEN_AND,
    TOKEN_OR,

    /* Actions, successions (turn 2 of behavioral expansion).  `start`
     * and `done` are SysML standard library features; we model them as
     * keywords producing built-in references rather than expecting the
     * user to import them.                                            */
    TOKEN_ACTION,
    TOKEN_SUCCESSION,
    TOKEN_FIRST,
    TOKEN_THEN,
    TOKEN_START,
    TOKEN_DONE,

    /* States, transitions (turn 3 of behavioral expansion). The `entry`
     * / `do` / `exit` lifecycle keywords appear inside state bodies
     * pointing at action references; `transition` introduces a new
     * statement form; `accept` / `if` are clauses on transitions.    */
    TOKEN_STATE,
    TOKEN_EXHIBIT,
    TOKEN_TRANSITION,
    TOKEN_ENTRY,
    TOKEN_EXIT,
    TOKEN_DO,
    TOKEN_ACCEPT,
    TOKEN_IF,

    /* `perform [action] <ref>;` declares that an enclosing part is
     * responsible for performing a named action.  Treated as an
     * action usage with the `isPerform` flag set.                    */
    TOKEN_PERFORM,

    /* `return [name] [: T] [= expr];` inside calc/action def bodies.
     * Calc def is a v0.6 feature; for now we just lex the keyword and
     * the parser swallows the statement.                              */
    TOKEN_RETURN,

    /* Calc def — a calculation body that returns a typed value.
     * `calc def F { in p : T; return r : T = expr; }`.                */
    TOKEN_CALC,

    /* `bind X = Y;` — a connection variant equating two referenceable
     * features.  Modeled as a connection usage with `isBind` set.    */
    TOKEN_BIND,

    /* `flow of T from X to Y;` — typed-flow shorthand where `of` is
     * an alternate type keyword used only in flow-usage position.   */
    TOKEN_OF,

    /* `allocate X to Y [ { ... } ];` — connection variant indicating
     * one element is allocated to another.  Modeled as a connection
     * usage with `isAllocate` set.                                  */
    TOKEN_ALLOCATE,

    /* Tier 2 keywords — full AST modeling but limited semantic
     * checking for now.  Parsed via the standard definitionOrUsage
     * path with KindInfo entries for each.                          */
    TOKEN_OCCURRENCE,
    TOKEN_EVENT,
    TOKEN_INDIVIDUAL,
    TOKEN_SNAPSHOT,
    TOKEN_TIMESLICE,
    TOKEN_ALLOCATION,
    TOKEN_VIEW,
    TOKEN_VIEWPOINT,
    TOKEN_RENDERING,
    TOKEN_CONCERN,
    TOKEN_VARIANT,
    TOKEN_VARIATION,
    TOKEN_ACTOR,
    TOKEN_INCLUDE,
    TOKEN_MESSAGE,
    TOKEN_USE,                                     /* `use case ...` introducer */

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

/* Scan the body of a doc comment after a `doc` keyword has been
 * consumed.  Whitespace before the opening slash-star is skipped,
 * the delimiters are stripped, and the inner content is returned
 * as TOKEN_DOC_BODY.  If no slash-star appears, TOKEN_ERROR. */
Token scanDocBody(void);

/* Returns the most-recently-skipped slash-star block-comment body,
 * with delimiters stripped.  type==TOKEN_DOC_BODY when valid, type==0
 * when no block comment was skipped immediately before the current
 * scanner position.  The slot is cleared on each token fetch (start
 * of every scanToken/skipWhitespace pass), and overwritten when a new
 * block is consumed.  Reading takes ownership: subsequent reads
 * return empty until the next block comment is skipped. */
Token takeLastBlockComment(void);

/* Pretty name for debugging / test harness. */
const char* tokenTypeName(TokenType type);

#endif /* SYSMLC_SCANNER_H */
