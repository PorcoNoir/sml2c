/* sysmlc — ast.h
 *
 * Abstract Syntax Tree.
 *
 * Style: tagged union ("sum type"), one Node struct, NodeKind picks
 * which variant of the inner anonymous union is valid.  This is the
 * common idiom for hand-written C compilers (Lua, Tcl, GCC's older
 * tree code).  Crafting Interpreters' jlox uses Java class hierarchy
 * + visitor; clox's compiler skips the AST entirely and emits bytecode
 * during parse.  We're going AST-first because we'll want to re-walk
 * it later for type checking and unit checking.
 *
 * Tokens carried inside nodes are *value copies* of the scanner's
 * Token.  Token::start still points into the original source buffer,
 * which the caller must keep alive for the lifetime of the AST.
 */
#ifndef SYSMLC_AST_H
#define SYSMLC_AST_H

#include <stdbool.h>
#include "scanner.h"

typedef enum {
    NODE_PROGRAM,         /* top-level container, never written by user */
    NODE_PACKAGE,         /* package Foo { ... }                         */
    NODE_IMPORT,          /* import A::B::C; or import A::B::*;          */
    NODE_DEFINITION,      /* part/port/interface/item/connection/flow/enum def Name { ... } */
    NODE_USAGE,           /* part/port/interface/item/connection/flow/enum name : Type [...] */
    NODE_ATTRIBUTE,       /* attribute name : Type;                      */
    NODE_QUALIFIED_NAME,  /* A::B::C  (used as a type ref or import tgt) */
    NODE_MULTIPLICITY,    /* [n], [lo..hi], [*], [lo..*]                 */
    NODE_DOC,             /* doc keyword form, or slash-star-star form */
    NODE_LITERAL,         /* numeric/string/bool literal in an expression */
    NODE_BINARY,          /* left op right (+, -, *, /, ==, !=, <, ...)   */
    NODE_UNARY,           /* prefix - or !                                */
    NODE_ALIAS,           /* alias Name for A::B::C;                      */
    NODE_COMMENT,         /* comment [Name] [about Refs] {block-comment-body} */
    NODE_DEPENDENCY,      /* dependency [Name] from A,B to C,D;           */
    NODE_SUCCESSION,      /* succession ['name'] [first ref] (then ref)+  */
    NODE_TRANSITION,      /* transition [name] [first src] [accept E] [if G] [do A] then T */
    NODE_LIFECYCLE_ACTION /* entry/do/exit ref;  inside a state body      */
} NodeKind;

/* Which flavor of literal a NODE_LITERAL holds.  String literals don't
 * need a parsed value field — the token's lexeme serves directly. */
typedef enum {
    LIT_INT  = 0,
    LIT_REAL,
    LIT_STRING,
    LIT_BOOL
} LiteralKind;

/* Which family a definition or usage belongs to.  All six kinds share
 * the same surface grammar (`<keyword> def Name {…}` and `<keyword> name
 * : Type;`) so we tag them with a DefKind rather than minting distinct
 * NodeKind values for each.                                             */
typedef enum {
    DEF_PART = 0,
    DEF_PORT,
    DEF_INTERFACE,
    DEF_ITEM,
    DEF_CONNECTION,
    DEF_FLOW,
    DEF_END,                /* `end name : Type;` inside a connection/flow def */
    DEF_DATATYPE,           /* synthetic — only the built-in stdlib uses this */
    DEF_ENUM,               /* `enum def Color { red; green; ... }` and its values */
    DEF_REFERENCE,          /* bare `ref name : T;` — kindless ReferenceUsage */
    DEF_CONSTRAINT,         /* `constraint def C { … }` / `assert constraint c : C` */
    DEF_REQUIREMENT,        /* `requirement def R { … }` / `require requirement r : R` */
    DEF_SUBJECT,            /* `subject e : Engine;` inside a requirement def */
    DEF_ACTION,             /* `action def A { … }` / `action a : A` */
    DEF_STATE               /* `state def S { … }` / `state s : S`, `exhibit state s` */
} DefKind;

/* The `assert` / `assume` / `require` modifier on a constraint or
 * requirement usage.  Bare usages without one of these keywords are
 * not (yet) accepted by the parser — every constraint/requirement
 * usage in v0.x must declare its assertion stance.  None is reserved
 * for non-constraint/requirement usages, where the field is unused. */
typedef enum {
    ASSERT_NONE = 0,
    ASSERT_ASSERT,          /* `assert constraint c : C;`                  */
    ASSERT_ASSUME,          /* `assume constraint c : C;` (in requirement) */
    ASSERT_REQUIRE          /* `require constraint c : C;`                 */
} AssertKind;

/* The kind of lifecycle action attached to a state body — `entry foo;`,
 * `do bar;`, or `exit baz;`.  Each lifecycle action carries one such
 * tag plus a reference to the action it triggers.                      */
typedef enum {
    LIFECYCLE_ENTRY = 0,
    LIFECYCLE_DO,
    LIFECYCLE_EXIT
} LifecycleKind;

/* Port direction modifier.  Currently only port usages can carry one,
 * but we put it on the usage variant uniformly so the parser doesn't
 * need a separate field for it.                                        */
typedef enum {
    DIR_NONE = 0,
    DIR_IN,
    DIR_OUT,
    DIR_INOUT
} Direction;

/* Visibility modifier on a declaration.  VIS_DEFAULT means no modifier
 * appeared in source — SysML treats that as effectively `public` at
 * package scope, but we keep it distinct so the printer can echo
 * source faithfully. */
typedef enum {
    VIS_DEFAULT = 0,      /* no modifier in source            */
    VIS_PUBLIC,
    VIS_PRIVATE,
    VIS_PROTECTED
} Visibility;

/* Dynamic array of Node*.  Used for relationship lists (`: A, B`,
 * `:> X, Y`) and for connector/flow endpoints.                        */
typedef struct {
    struct Node** items;
    int           count;
    int           capacity;
} NodeList;

typedef struct Node Node;

struct Node {
    NodeKind kind;
    int      line;
    union {
        /* PROGRAM, PACKAGE, DEFINITION — all "named scopes" with members.
         * defKind is meaningful only for DEFINITION; the other two leave
         * it at zero (DEF_PART), which is harmless because they're never
         * dispatched through the kind-aware printer.
         *
         * `body` carries the boolean expression of a `constraint def`.
         * It's NULL for every other DEFINITION (and for PROGRAM /
         * PACKAGE).  For constraint defs it sits alongside members:
         * `in p : T;` parameters live in members; the trailing
         * boolean expression lives here.                                */
        struct {
            Token      name;        /* unused for PROGRAM                */
            Visibility visibility;
            DefKind    defKind;
            bool       isAbstract;  /* `abstract part def Vehicle { ... }` */
            Node**     members;
            int        memberCount;
            int        memberCapacity;
            NodeList   specializes; /* `:>`  / `specializes` (DEFINITION) */
            NodeList   redefines;   /* `:>>` / `redefines`   (DEFINITION) */
            Node*      body;        /* constraint def: boolean expression */
        } scope;

        /* USAGE — has optional name (anonymous for bare `connect a to b`),
         * optional type/spec/redef lists, optional multiplicity, optional
         * connector or flow endpoint clause, and an optional body.
         *
         * `assertKind` records the assert/assume/require prefix for
         * constraint and requirement usages.  Other usages leave it
         * at ASSERT_NONE.
         *
         * `body` carries an inline boolean expression for anonymous
         * constraint usages: `assert constraint { x > 0 }`.  NULL when
         * the usage is named or typed instead.                          */
        struct {
            Token      name;        /* length==0 means anonymous          */
            Visibility visibility;
            DefKind    defKind;
            Direction  direction;   /* in/out/inout (port usages mostly)  */
            bool       isDerived;   /* RefPrefix:  `derived`              */
            bool       isAbstract;  /* RefPrefix:  `abstract`             */
            bool       isConstant;  /* RefPrefix:  `constant`             */
            bool       isReference; /* BasicUsagePrefix: `ref` (after referential pass) */
            bool       isReferenceExplicit; /* user wrote `ref` in source           */
            bool       isPerform;   /* `perform action a : T;`            */
            AssertKind assertKind;  /* assert/assume/require              */
            NodeList   types;       /* `: A, B`                           */
            NodeList   specializes; /* `:> X, Y`                          */
            NodeList   redefines;   /* `:>> P, Q`                         */
            Node*      multiplicity;/* `[...]`  NULL if not specified     */
            Node*      defaultValue;/* `= expr`  NULL if not specified    */
            NodeList   ends;        /* connector/flow endpoints (2 elts)  */
            Node*      body;        /* inline boolean expr (constraint)   */
            Node**     members;     /* NULL if no `{ ... }`               */
            int        memberCount;
            int        memberCapacity;
        } usage;

        /* ATTRIBUTE — name + optional type/specializes/redefines.      */
        struct {
            Token      name;
            Visibility visibility;
            bool       isDerived;   /* `derived attribute area = ...;`   */
            bool       isAbstract;  /* `abstract attribute ...`          */
            bool       isConstant;  /* `constant attribute pi = 3.14;`   */
            bool       isReference; /* `ref attribute ...` (after referential pass) */
            bool       isReferenceExplicit; /* user wrote `ref` in source            */
            NodeList   types;       /* `: A, B`                          */
            NodeList   specializes; /* `:> X, Y`                         */
            NodeList   redefines;   /* `:>> P, Q`                        */
            Node*      multiplicity;/* `[...]`  NULL if not specified    */
            Node*      defaultValue;/* `= expr`  NULL if not specified   */
        } attribute;

        /* IMPORT — points to a QUALIFIED_NAME, plus a wildcard flag.    */
        struct {
            Visibility visibility;
            Node*      target;
            bool       wildcard;
        } import;

        /* QUALIFIED_NAME — array of identifier tokens (A::B::C → 3).
         *
         * `resolved` is filled in by the resolver pass: it points to
         * the declaration node this reference binds to (a NODE_PACKAGE,
         * NODE_DEFINITION, NODE_USAGE, or NODE_ATTRIBUTE).  NULL means
         * either "the resolver hasn't run yet" or "the name was tentatively
         * accepted as deferred (e.g. brought in by a wildcard import we
         * don't actually load yet)".  An undefined name produces an error
         * during resolution; it does not leave a NULL `resolved`.
         *
         * `isConjugated` is set when the qualified name was prefixed with
         * `~` (port conjugation, §8.3.12.3-4).  The flag flows through
         * to consumers — connection-end direction-checking flips when
         * the port reference reached its definition through a conjugated
         * type ref.
         *
         * `conjugationParity` is computed by the resolver as it walks
         * a multi-segment chain: each hop through a conjugated type
         * ref toggles the parity.  Consumers (notably the connection
         * checker's flow-direction rule) read this to decide whether
         * the resolved port's effective direction is flipped.       */
        struct {
            Token*      parts;
            int         partCount;
            int         partCapacity;
            const Node* resolved;
            bool        isConjugated;
            bool        conjugationParity;
        } qualifiedName;

        /* MULTIPLICITY — `[n]`, `[lo..hi]`, `[*]`, `[lo..*]`.
         *
         * For `[3]`:        lower=3,  upper=0,  isRange=false
         * For `[1..5]`:     lower=1,  upper=5,  isRange=true
         * For `[0..*]`:     lower=0,  upperWildcard=true, isRange=true
         * For `[*]`:        lowerWildcard=true, isRange=false
         *
         * Once expressions land, lower/upper become Node* expression
         * trees; for now they're just integer literals.               */
        struct {
            long lower;
            long upper;
            bool lowerWildcard;
            bool upperWildcard;
            bool isRange;
        } multiplicity;

        /* DOC — captured body of a doc-keyword form or implicit form.
         * The token lexeme is the inner content with delimiters
         * stripped, so printing is straightforward.                  */
        struct {
            Token body;
        } doc;

        /* LITERAL — numeric/string/bool.  The token holds the lexeme
         * and source line; the value union holds the parsed scalar
         * (string literals read directly from the token).            */
        struct {
            Token       token;
            LiteralKind litKind;
            union {
                long long iv;
                double    rv;
                bool      bv;
            } value;
        } literal;

        /* BINARY — left op right.  The op token records both the
         * operator type (TOKEN_PLUS, TOKEN_STAR, ...) and its source
         * location.                                                 */
        struct {
            Token op;
            Node* left;
            Node* right;
        } binary;

        /* UNARY — prefix `-` or `!` applied to an operand.          */
        struct {
            Token op;
            Node* operand;
        } unary;

        /* ALIAS — `alias Name for A::B::C;`
         * Creates an additional name in the current namespace that
         * refers to the same Element as `target` resolves to. */
        struct {
            Token      name;          /* the alias name introduced     */
            Visibility visibility;
            Node*      target;        /* a NODE_QUALIFIED_NAME         */
        } alias;

        /* COMMENT — `comment [Name] [about RefList] {block-body}`
         * The keyword form of a Comment annotating namespace elements
         * (clause 8.3.4).  Name is optional (length==0 if absent);
         * the about-list is optional (count==0 if absent).            */
        struct {
            Token      name;          /* length==0 if no name          */
            NodeList   about;         /* qualifiedNames; count==0 = no about clause */
            Token      body;          /* block-comment contents (delims stripped) */
        } comment;

        /* DEPENDENCY — `dependency [Name] from A,B to C,D;`
         * A directed relationship from a list of source elements to
         * a list of target elements (clause 8.3.3).  Name optional. */
        struct {
            Token      name;          /* length==0 if no name          */
            Visibility visibility;
            NodeList   sources;       /* qualifiedNames after `from`   */
            NodeList   targets;       /* qualifiedNames after `to`     */
        } dependency;

        /* SUCCESSION — `succession [Name] [first src] then dst (then dst)*;`
         * or the unqualified inline form `first src;` / `then dst;` used
         * inside an action def body.  Models a chain: each item in
         * `targets` is preceded by either `first` (if explicit) or by
         * the previous target in the chain.
         *
         * `first` is the explicit predecessor (may be NULL — for inline
         * `then x;` statements that continue the previous chain).
         * `targets` holds the chain's (then …) targets in order.  Each
         * target node is either:
         *   - a NODE_QUALIFIED_NAME (a name reference, `then loadCargo`)
         *   - a NODE_USAGE         (an inline declaration, `then action a {…}`)
         */
        struct {
            Token      name;          /* length==0 if anonymous         */
            Visibility visibility;
            Node*      first;         /* qname or NULL for continuation */
            NodeList   targets;       /* qnames or inline action usages */
        } succession;

        /* TRANSITION — full form:
         *   transition [Name] [first src] [accept evt] [if guard] [do effect] then tgt;
         * or the short form (used for `transition initial then off;`):
         *   transition first src then tgt;
         *
         * `first`, `accept`, `effect`, `target` are NODE_QUALIFIED_NAME refs
         * (the `effect` may carry inline body in a future expansion).
         * `guard` is an arbitrary expression (NODE_BINARY/UNARY/QUALIFIED_NAME).
         * Any field may be NULL except `target`, which is required.       */
        struct {
            Token      name;          /* length==0 if anonymous          */
            Visibility visibility;
            Node*      first;         /* source state ref                */
            Node*      accept;        /* event ref                       */
            Node*      guard;         /* boolean expression              */
            Node*      effect;        /* effect action ref               */
            Node*      target;        /* target state ref (required)     */
        } transition;

        /* LIFECYCLE_ACTION — `entry ref;`, `do ref;`, `exit ref;` inside
         * a state body.  Carries the kind tag plus a single qname ref
         * to the action invoked.                                        */
        struct {
            LifecycleKind kind;
            Node*         action;     /* qualified-name ref to an action */
        } lifecycleAction;
    } as;
};

/* ---- construction ---- */
Node* astMakeNode(NodeKind kind, int line);

/* For NODE_PROGRAM / NODE_PACKAGE / NODE_PART_DEF: */
void  astAppendScopeMember(Node* scope, Node* member);
/* For NODE_PART_USAGE: */
void  astAppendUsageMember(Node* usage, Node* member);
/* For NODE_QUALIFIED_NAME: */
void  astAppendQualifiedPart(Node* qname, Token part);
/* Append a Node* to any NodeList (relationship lists, ends list). */
void  astListAppend(NodeList* list, Node* item);
/* Set a visibility modifier on whichever variant supports one. */
void  astSetVisibility(Node* node, Visibility v);

/* ---- inspection ---- */
void  astPrint(const Node* root);

#endif /* SYSMLC_AST_H */
