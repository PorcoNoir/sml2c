/* sysmlc — parser_decl.c
 *
 * Declaration grammar.  See parser_internal.h for the full split:
 * this file owns the top-level grammar rules — packages, definitions,
 * usages, attributes, imports, plus the docDecl/aliasDecl/commentDecl/
 * dependencyDecl forms — and the declaration() dispatcher that's the
 * top of the recursive-descent tree.
 *
 * parser_common.c provides the token machinery and shared helpers
 * (qualifiedName, parseMultiplicity, appendQualifiedNameList,
 * tokenToLong).  parser_expr.c provides the Pratt expression parser;
 * we call it via the public expression() entry.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"            /* parse(), parserHadError() — public surface */
#include "parser_internal.h"

/*  parseFeatureRelationships — the optional `: T :> S :>> R [m]` tail
 *  shared by attributes and usages.  Each clause appears at most once;
 *  duplicates report errors but the parser continues.                 */
FeatureRels parseFeatureRelationships(void) {
    FeatureRels r = {0};
    for (;;) {
        if (match(TOKEN_LEFT_BRACKET)) {
            if (r.multiplicity) error("Duplicate multiplicity.");
            r.multiplicity = parseMultiplicity();
        } else if (match(TOKEN_COLON)) {
            if (r.types.count > 0) error("Duplicate type clause.");
            appendQualifiedNameList(&r.types);
        } else if (match(TOKEN_COLON_GREATER) || match(TOKEN_SPECIALIZES)) {
            if (r.specializes.count > 0) error("Duplicate 'specializes' clause.");
            appendQualifiedNameList(&r.specializes);
        } else if (match(TOKEN_COLON_GREATER_GREATER) || match(TOKEN_REDEFINES)) {
            if (r.redefines.count > 0) error("Duplicate 'redefines' clause.");
            appendQualifiedNameList(&r.redefines);
        } else {
            break;
        }
    }
    return r;
}

/*  importDecl ::= "import" IDENTIFIER ("::" IDENTIFIER)* ("::" "*")? ";"  */
static Node* importDecl(void) {
    int line = parser.previous.line;            /* 'import' was just consumed */

    Node* qn = astMakeNode(NODE_QUALIFIED_NAME, line);
    bool wildcard = false;

    if (!check(TOKEN_IDENTIFIER)) {
        errorAtCurrent("Expected name after 'import'.");
    } else {
        advance();
        astAppendQualifiedPart(qn, parser.previous);
        while (check(TOKEN_COLON_COLON)) {
            advance();                          /* eat ::            */
            if (check(TOKEN_STAR)) {
                advance();
                wildcard = true;
                break;                          /* :: * must terminate */
            }
            if (!check(TOKEN_IDENTIFIER)) {
                error("Expected name or '*' after '::'.");
                break;
            }
            advance();
            astAppendQualifiedPart(qn, parser.previous);
        }
    }

    consume(TOKEN_SEMICOLON, "Expected ';' after import.");

    Node* imp = astMakeNode(NODE_IMPORT, line);
    imp->as.import.target = qn;
    imp->as.import.wildcard = wildcard;
    return imp;
}

/*  attributeDecl ::= "attribute" IDENTIFIER featureRelationships?
 *                    ( "=" expression )? ";"                          */
static Node* attributeDecl(void) {
    int line = parser.previous.line;            /* 'attribute' just consumed */
    consume(TOKEN_IDENTIFIER, "Expected attribute name.");
    Token name = parser.previous;

    FeatureRels rels = parseFeatureRelationships();

    /* Optional default value: `= expression`.  Anything that
     * `expression()` recognizes is fair game — arithmetic, comparison,
     * grouped subexpressions, identifier references.                  */
    Node* defaultValue = NULL;
    if (match(TOKEN_EQUAL)) {
        defaultValue = expression();
    }

    consume(TOKEN_SEMICOLON, "Expected ';' after attribute.");

    Node* a = astMakeNode(NODE_ATTRIBUTE, line);
    a->as.attribute.name         = name;
    a->as.attribute.types        = rels.types;
    a->as.attribute.specializes  = rels.specializes;
    a->as.attribute.redefines    = rels.redefines;
    a->as.attribute.multiplicity = rels.multiplicity;
    a->as.attribute.defaultValue = defaultValue;
    return a;
}

/*  partDef-style and partUsage-style rules — generalized.
 *
 *  Multiple kinds (part / port / interface / item / connection / flow /
 *  end / enum / reference) all share the same surface grammar.  We
 *  capture what varies between them in a small KindInfo struct and let
 *  one set of parsing functions consume it.                            */
typedef struct {
    DefKind     kind;             /* AST tag stamped on the result      */
    bool        allowsDirection;  /* can the usage take in/out/inout?   */
    bool        defAllowed;       /* does this kind have a `def` form?  */
    bool        allowsAnonymous;  /* may the name be omitted?           */
    const char* humanName;        /* "part", "port", … for messages    */
} KindInfo;

static const KindInfo kPart       = { DEF_PART,       false, true,  false, "part"       };
static const KindInfo kPort       = { DEF_PORT,       true,  true,  false, "port"       };
static const KindInfo kInterface  = { DEF_INTERFACE,  false, true,  false, "interface"  };
static const KindInfo kItem       = { DEF_ITEM,       false, true,  false, "item"       };
static const KindInfo kConnection = { DEF_CONNECTION, false, true,  true,  "connection" };
static const KindInfo kFlow       = { DEF_FLOW,       false, true,  true,  "flow"       };
static const KindInfo kEnd        = { DEF_END,        false, false, false, "end"        };
static const KindInfo kEnum       = { DEF_ENUM,       false, true,  false, "enum"       };
/* ReferenceUsage has no `def` form (defAllowed=false): it's a Usage
 * only.  No keyword to consume — caller invokes usage() directly,
 * not definitionOrUsage().                                          */
static const KindInfo kReference  = { DEF_REFERENCE,  false, false, false, "reference"  };

/* Parse a "connect a to b" or "from a to b" clause.  Caller has
 * already consumed the opening keyword (CONNECT or FROM).  Appends
 * exactly two qualified-name nodes to the given list.                */
static void parseEndsClause(NodeList* ends, const char* hint) {
    astListAppend(ends, qualifiedName());
    consume(TOKEN_TO, "Expected 'to' in this clause.");
    (void)hint;
    astListAppend(ends, qualifiedName());
}

/*  definition ::= "def" IDENTIFIER featureRelationships? "{" declaration* "}"
 *  Caller has already consumed the kind keyword (e.g. "part", "port").   */
static Node* definition(const KindInfo* k) {
    int line = parser.previous.line;            /* kind keyword just consumed */
    advance();                                  /* eat 'def'                  */
    consume(TOKEN_IDENTIFIER, "Expected definition name.");
    Token name = parser.previous;

    FeatureRels rels = parseFeatureRelationships();

    consume(TOKEN_LEFT_BRACE, "Expected '{' after definition name.");

    Node* def = astMakeNode(NODE_DEFINITION, line);
    def->as.scope.name        = name;
    def->as.scope.defKind     = k->kind;
    def->as.scope.specializes = rels.specializes;
    def->as.scope.redefines   = rels.redefines;
    /* rels.types and rels.multiplicity are silently dropped on a
     * definition; semantic check will warn.                            */

    /* Enum def bodies have a different shape: each member is
     * `IDENTIFIER ( "=" expression )? ";"`.  The optional initializer
     * lets values carry an explicit code (e.g. `stop = 0;` for
     * traffic-light enumerations).                                    */
    if (k->kind == DEF_ENUM) {
        while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
            if (!match(TOKEN_IDENTIFIER)) {
                errorAtCurrent("Expected enumeration value name.");
                advance();
                if (parser.panicMode) synchronize();
                continue;
            }
            Token vname = parser.previous;
            int vline = vname.line;
            Node* initializer = NULL;
            if (match(TOKEN_EQUAL)) {
                initializer = expression();
            }
            consume(TOKEN_SEMICOLON, "Expected ';' after enumeration value.");
            /* Each enum value is modeled as an Attribute carrying the
             * enclosing enum as its declared type.  The synthetic type
             * reference is a one-segment qualified name with `resolved`
             * pre-filled to the enum def itself — so by the time later
             * passes see it, it's already bound and they can treat the
             * value uniformly with `attribute red : Color = ...`.    */
            Node* typeRef = astMakeNode(NODE_QUALIFIED_NAME, vline);
            astAppendQualifiedPart(typeRef, def->as.scope.name);
            typeRef->as.qualifiedName.resolved = def;

            Node* v = astMakeNode(NODE_ATTRIBUTE, vline);
            v->as.attribute.name         = vname;
            v->as.attribute.defaultValue = initializer;
            astListAppend(&v->as.attribute.types, typeRef);
            astAppendScopeMember(def, v);
        }
    } else {
        while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
            Node* m = declaration();
            if (m) astAppendScopeMember(def, m);
            if (parser.panicMode) synchronize();
        }
    }
    consume(TOKEN_RIGHT_BRACE, "Expected '}' to close definition.");
    return def;
}

/*  usage ::= IDENTIFIER? featureRelationships? endpointsClause?
 *           ( "=" expression )? ( "{" declaration* "}" | ";" )
 *
 *  The name is optional only for kinds whose KindInfo says so
 *  (currently connection and flow, which support anonymous "connect a
 *  to b" / "from a to b" forms).  The endpoint clause is parsed only
 *  for connection (`connect`) or flow (`from`) kinds.                */
static Node* usage(const KindInfo* k, Direction dir) {
    int line = parser.previous.line;

    /* ---- optional name ---------------------------------------------- */
    Token name = (Token){0};
    if (check(TOKEN_IDENTIFIER)) {
        advance();
        name = parser.previous;
    } else if (!k->allowsAnonymous
               || (k->kind == DEF_CONNECTION && !check(TOKEN_CONNECT))
               || (k->kind == DEF_FLOW       && !check(TOKEN_FROM))) {
        consume(TOKEN_IDENTIFIER, "Expected name.");
    }
    /* (Otherwise name stays zero-length — anonymous usage.)            */

    FeatureRels rels = parseFeatureRelationships();

    /* ---- optional endpoint clause ----------------------------------- */
    NodeList ends = {0};
    if (k->kind == DEF_CONNECTION && match(TOKEN_CONNECT)) {
        parseEndsClause(&ends, "connector");
    } else if (k->kind == DEF_FLOW && match(TOKEN_FROM)) {
        parseEndsClause(&ends, "flow");
    }

    Node* u = astMakeNode(NODE_USAGE, line);
    u->as.usage.name         = name;
    u->as.usage.defKind      = k->kind;
    u->as.usage.direction    = dir;
    u->as.usage.types        = rels.types;
    u->as.usage.specializes  = rels.specializes;
    u->as.usage.redefines    = rels.redefines;
    u->as.usage.multiplicity = rels.multiplicity;
    u->as.usage.ends         = ends;

    /* Optional initializer: `= expression` before the closing `;`.
     * Currently used by bare-ref usages (`ref nominalTorque : Real = 100;`)
     * but allowed on any kind for symmetry with attribute usages.  */
    if (match(TOKEN_EQUAL)) {
        u->as.usage.defaultValue = expression();
    }

    if (match(TOKEN_LEFT_BRACE)) {
        while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
            Node* m = declaration();
            if (m) astAppendUsageMember(u, m);
            if (parser.panicMode) synchronize();
        }
        consume(TOKEN_RIGHT_BRACE, "Expected '}' to close usage body.");
    } else {
        consume(TOKEN_SEMICOLON, "Expected ';' or '{' after usage.");
    }
    return u;
}

/*  FeatureModifiers — the four flags from the SysML v2 RefPrefix +
 *  BasicUsagePrefix grammar (per the OMG SysML v2 textual notation
 *  and the Pilot Implementation Xtext grammar).  The canonical
 *  prefix order is positional, not free:
 *
 *      direction? derived? (abstract|variation)? constant? ref?
 *
 *  We absorb `direction` separately upstream (in declaration()), and
 *  defer `variation` until later.  So this helper consumes the
 *  remaining four in their fixed order: derived, abstract, constant,
 *  ref.  Each is at-most-once; the parser does not reorder them.    */
typedef struct {
    bool isDerived;
    bool isAbstract;
    bool isConstant;
    bool isReference;
} FeatureModifiers;

static FeatureModifiers parseFeatureModifiers(void) {
    FeatureModifiers m = {0};
    if (match(TOKEN_DERIVED))  m.isDerived  = true;
    if (match(TOKEN_ABSTRACT)) m.isAbstract = true;
    if (match(TOKEN_CONSTANT)) m.isConstant = true;
    if (match(TOKEN_REF))      m.isReference = true;
    return m;
}

/* True if any modifier flag is set — used to detect modifiers on
 * declarations that don't accept any. */
static bool hasAnyModifier(FeatureModifiers m) {
    return m.isDerived || m.isAbstract || m.isConstant || m.isReference;
}

/*  Dispatch: caller has consumed the kind keyword (and any direction
 *  prefix).  If the next token is `def`, we're a definition; otherwise
 *  a usage.  A direction prefix is only meaningful on a usage; on a
 *  definition it's an error.  Kinds that don't have a `def` form
 *  (notably `end`) reject `def` here.                                 */
static Node* definitionOrUsage(const KindInfo* k, Direction dir, FeatureModifiers m) {
    if (check(TOKEN_DEF)) {
        if (!k->defAllowed) {
            errorAtCurrent("This kind has no 'def' form.");
            advance();              /* eat 'def' so we don't loop      */
        }
        if (dir != DIR_NONE) {
            error("Direction modifier is not valid on a definition.");
        }
        /* Definitions accept only `abstract`; the value-bearing flags
         * (derived, constant) and `ref` are usage-only.               */
        if (m.isDerived) {
            error("'derived' is not valid on a definition.");
        }
        if (m.isConstant) {
            error("'constant' is not valid on a definition.");
        }
        if (m.isReference) {
            error("'ref' is not valid on a definition.");
        }
        Node* d = definition(k);
        if (d) d->as.scope.isAbstract = m.isAbstract;
        return d;
    }
    if (dir != DIR_NONE && !k->allowsDirection) {
        error("Direction modifier is not valid on this kind of usage.");
    }
    Node* u = usage(k, dir);
    if (u && u->kind == NODE_USAGE) {
        u->as.usage.isDerived   = m.isDerived;
        u->as.usage.isAbstract  = m.isAbstract;
        u->as.usage.isConstant  = m.isConstant;
        u->as.usage.isReference = m.isReference;
    }
    return u;
}

/*  Standalone "connect a to b ;" — no preceding `connection` keyword.
 *  Produces an anonymous connection usage.                            */
static Node* connectStatement(void) {
    int line = parser.previous.line;            /* TOKEN_CONNECT consumed */
    Node* u = astMakeNode(NODE_USAGE, line);
    u->as.usage.defKind = DEF_CONNECTION;
    parseEndsClause(&u->as.usage.ends, "connector");
    consume(TOKEN_SEMICOLON, "Expected ';' after connect statement.");
    return u;
}

/*  packageDecl ::= "package" IDENTIFIER "{" declaration* "}"          */
static Node* packageDecl(void) {
    int line = parser.previous.line;            /* 'package' just consumed */
    consume(TOKEN_IDENTIFIER, "Expected package name.");
    Token name = parser.previous;
    consume(TOKEN_LEFT_BRACE, "Expected '{' after package name.");

    Node* p = astMakeNode(NODE_PACKAGE, line);
    p->as.scope.name = name;

    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        Node* m = declaration();
        if (m) astAppendScopeMember(p, m);
        if (parser.panicMode) synchronize();
    }
    consume(TOKEN_RIGHT_BRACE, "Expected '}' to close package.");
    return p;
}

/*  docDecl ::= "doc" SLASH_STAR body STAR_SLASH    explicit keyword form
 *           |  SLASH_STAR_STAR body STAR_SLASH      implicit form
 *
 *  Both produce a NODE_DOC carrying the captured body.               */
static Node* docDecl(void) {
    int line = parser.current.line;
    Token body;

    if (match(TOKEN_DOC)) {
        /* The advance() inside match() saw previous == TOKEN_DOC and
         * routed through scanDocBody, so parser.current should now be
         * the body (or an error token, already reported).            */
        if (parser.current.type != TOKEN_DOC_BODY) {
            return NULL;                /* error already reported     */
        }
        body = parser.current;
        advance();
    } else if (match(TOKEN_DOC_BODY)) {
        body = parser.previous;
    } else {
        errorAtCurrent("Expected doc.");
        advance();
        return NULL;
    }

    Node* d = astMakeNode(NODE_DOC, line);
    d->as.doc.body = body;
    return d;
}

/*  aliasDecl ::= "alias" IDENTIFIER "for" qualifiedName ";"
 *
 * Creates a named alias for an existing element.  At parse time we
 * just record the name and the qualified-name target; the resolver
 * binds the alias name in the enclosing scope and resolves the
 * target reference.                                                */
static Node* aliasDecl(void) {
    int line = parser.previous.line;            /* 'alias' just consumed */
    consume(TOKEN_IDENTIFIER, "Expected alias name.");
    Token name = parser.previous;
    consume(TOKEN_FOR, "Expected 'for' after alias name.");
    Node* target = qualifiedName();
    consume(TOKEN_SEMICOLON, "Expected ';' after alias.");

    Node* a = astMakeNode(NODE_ALIAS, line);
    a->as.alias.name       = name;
    a->as.alias.visibility = VIS_DEFAULT;
    a->as.alias.target     = target;
    return a;
}

/*  commentDecl ::= "comment" IDENTIFIER? ( "about" qualifiedName ("," qualifiedName)* )?
 *                 BLOCK_COMMENT_BODY
 *
 * The body is recovered from the scanner's rolling save of the most
 * recently-skipped slash-star block.  This works regardless of where
 * the body falls relative to the optional name and about-list:
 * skipWhitespace stashes the body whenever it eats one, and we read
 * it after parsing the header.  Both the name and the about-list are
 * optional. */
static Node* commentDecl(void) {
    int line = parser.previous.line;            /* 'comment' just consumed */

    Token name = {0};
    if (check(TOKEN_IDENTIFIER)) {
        advance();
        name = parser.previous;
    }

    NodeList about = {0};
    if (match(TOKEN_ABOUT)) {
        appendQualifiedNameList(&about);
    }

    /* The body should have been skipped by one of the advances above
     * (or, for the bare anonymous form, by the advance that consumed
     * the keyword itself).  Pull it from the rolling save. */
    Token body = takeLastBlockComment();
    if (body.type != TOKEN_DOC_BODY) {
        errorAtCurrent("Expected a block comment after 'comment'.");
    }

    Node* c = astMakeNode(NODE_COMMENT, line);
    c->as.comment.name  = name;
    c->as.comment.about = about;
    c->as.comment.body  = body;
    return c;
}

/*  dependencyDecl ::= "dependency" IDENTIFIER? "from" qualifiedNameList "to" qualifiedNameList ";"
 *
 * A directed relationship from the source list to the target list. */
static Node* dependencyDecl(void) {
    int line = parser.previous.line;            /* 'dependency' just consumed */

    Token name = {0};
    if (check(TOKEN_IDENTIFIER)) {
        advance();
        name = parser.previous;
    }

    consume(TOKEN_FROM, "Expected 'from' in dependency.");
    NodeList sources = {0};
    appendQualifiedNameList(&sources);

    consume(TOKEN_TO, "Expected 'to' in dependency.");
    NodeList targets = {0};
    appendQualifiedNameList(&targets);

    consume(TOKEN_SEMICOLON, "Expected ';' after dependency.");

    Node* d = astMakeNode(NODE_DEPENDENCY, line);
    d->as.dependency.name       = name;
    d->as.dependency.visibility = VIS_DEFAULT;
    d->as.dependency.sources    = sources;
    d->as.dependency.targets    = targets;
    return d;
}

/*  declaration ::= visibility? direction? featureModifier*
 *                  ( docDecl | packageDecl | importDecl
 *                  | partDecl | portDecl | interfaceDecl | itemDecl
 *                  | connectionDecl | flowDecl | endDecl | enumDecl
 *                  | connectStatement | attributeDecl
 *                  | aliasDecl | commentDecl | dependencyDecl
 *                  | bareReferenceUsage )
 *
 *  visibility       ::= "public" | "private" | "protected"
 *  direction        ::= "in" | "out" | "inout"
 *  featureModifier  ::= "derived" | "abstract" | "constant" | "ref"
 *
 *  Visibility, direction, and the four feature modifiers are absorbed
 *  here as optional prefixes and threaded into the resulting node.
 *  Each grammar rule below stays ignorant of them.  Per the OMG SysML
 *  v2 spec, the modifiers are positional, not free-order — see
 *  parseFeatureModifiers.  Modifiers that aren't valid on the chosen
 *  kind (e.g. `derived` on a package) become errors here.            */
Node* declaration(void) {
    /* Doc forms come first — they don't take any prefix. */
    if (check(TOKEN_DOC) || check(TOKEN_DOC_BODY)) return docDecl();

    Visibility vis = VIS_DEFAULT;
    if      (match(TOKEN_PUBLIC))    vis = VIS_PUBLIC;
    else if (match(TOKEN_PRIVATE))   vis = VIS_PRIVATE;
    else if (match(TOKEN_PROTECTED)) vis = VIS_PROTECTED;

    Direction dir = DIR_NONE;
    if      (match(TOKEN_IN))    dir = DIR_IN;
    else if (match(TOKEN_OUT))   dir = DIR_OUT;
    else if (match(TOKEN_INOUT)) dir = DIR_INOUT;

    FeatureModifiers mods = parseFeatureModifiers();

    Node* result = NULL;
    if (match(TOKEN_PACKAGE)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on a package.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on a package.");
        result = packageDecl();
    } else if (match(TOKEN_IMPORT)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on an import.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on an import.");
        result = importDecl();
    } else if (match(TOKEN_ALIAS)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on an alias.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on an alias.");
        result = aliasDecl();
        if (result) result->as.alias.visibility = vis;
        vis = VIS_DEFAULT;          /* already applied */
    } else if (match(TOKEN_COMMENT_KW)) {
        if (vis != VIS_DEFAULT) error("Visibility modifier is not valid on a comment.");
        if (dir != DIR_NONE) error("Direction modifier is not valid on a comment.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on a comment.");
        result = commentDecl();
    } else if (match(TOKEN_DEPENDENCY)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on a dependency.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on a dependency.");
        result = dependencyDecl();
        if (result) result->as.dependency.visibility = vis;
        vis = VIS_DEFAULT;
    } else if (match(TOKEN_PART))       result = definitionOrUsage(&kPart,       dir, mods);
    else if (match(TOKEN_PORT))         result = definitionOrUsage(&kPort,       dir, mods);
    else if (match(TOKEN_INTERFACE))    result = definitionOrUsage(&kInterface,  dir, mods);
    else if (match(TOKEN_ITEM))         result = definitionOrUsage(&kItem,       dir, mods);
    else if (match(TOKEN_CONNECTION))   result = definitionOrUsage(&kConnection, dir, mods);
    else if (match(TOKEN_FLOW))         result = definitionOrUsage(&kFlow,       dir, mods);
    else if (match(TOKEN_END))          result = definitionOrUsage(&kEnd,        dir, mods);
    else if (match(TOKEN_ENUM))         result = definitionOrUsage(&kEnum,       dir, mods);
    else if (match(TOKEN_CONNECT)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on connect.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on connect.");
        result = connectStatement();
    }
    else if (match(TOKEN_ATTRIBUTE)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on an attribute.");
        result = attributeDecl();
        /* Attributes accept all four modifiers (they're usages). */
        if (result && result->kind == NODE_ATTRIBUTE) {
            result->as.attribute.isDerived   = mods.isDerived;
            result->as.attribute.isAbstract  = mods.isAbstract;
            result->as.attribute.isConstant  = mods.isConstant;
            result->as.attribute.isReference = mods.isReference;
        }
    } else {
        /* Bare-ref form (§8.3.6.3 ReferenceUsage): `ref name : T = expr;`
         * with no kind keyword.  We get here when parseFeatureModifiers
         * consumed `ref`, none of the kind keywords matched, and the
         * next token is the usage's name.                              */
        if (mods.isReference && check(TOKEN_IDENTIFIER)) {
            Node* u = usage(&kReference, dir);
            if (u && u->kind == NODE_USAGE) {
                u->as.usage.isDerived   = mods.isDerived;
                u->as.usage.isAbstract  = mods.isAbstract;
                u->as.usage.isConstant  = mods.isConstant;
                u->as.usage.isReference = true;
            }
            astSetVisibility(u, vis);
            return u;
        }

        if (vis != VIS_DEFAULT) {
            errorAtCurrent("Visibility modifier must be followed by a declaration.");
        } else if (dir != DIR_NONE) {
            errorAtCurrent("Direction modifier must be followed by a declaration.");
        } else if (hasAnyModifier(mods)) {
            errorAtCurrent("Feature modifier must be followed by a declaration.");
        } else {
            errorAtCurrent("Expected declaration.");
        }
        advance();                  /* make progress so we don't loop */
        return NULL;
    }

    /* Visibility belongs only to declarations that can carry it; the
     * helper silently drops it for kinds that can't.                  */
    astSetVisibility(result, vis);
    return result;
}

/* ===================================================== entry point */

Node* parse(void) {
    parser.hadError = false;
    parser.panicMode = false;
    advance();                          /* prime the pump            */

    Node* root = astMakeNode(NODE_PROGRAM, 1);
    while (!check(TOKEN_EOF)) {
        Node* d = declaration();
        if (d) astAppendScopeMember(root, d);
        if (parser.panicMode) synchronize();
    }
    return root;
}

bool parserHadError(void) { return parser.hadError; }
