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
#include "builtin.h"           /* builtinStart() / builtinDone() pseudo-actions */

/* Forward declarations needed by attributeDecl() and other early
 * functions that call helpers defined later in the file.            */
static void skipBracedBlock(void);
static void skipMetadataPrefix(void);

/*  parseFeatureRelationships — the optional `: T :> S :>> R [m]` tail
 *  shared by attributes and usages.  Each clause appears at most once;
 *  duplicates report errors but the parser continues.                 */
FeatureRels parseFeatureRelationships(void) {
    FeatureRels r = {0};
    for (;;) {
        if (match(TOKEN_LEFT_BRACKET)) {
            if (r.multiplicity) error("Duplicate multiplicity.");
            r.multiplicity = parseMultiplicity();
            /* Trailing multiplicity modifiers `nonunique` / `ordered`
             * may appear in either order, possibly both:
             *     attribute partMasses [*] nonunique :> ISQ::mass;
             *     state failureModes [*] nonunique;
             *     part wheels [4] ordered nonunique;
             * We parse-and-skip them; they don't have AST slots yet.   */
            while (match(TOKEN_NONUNIQUE) || match(TOKEN_ORDERED)) { /* eat */ }
        } else if (match(TOKEN_COLON)) {
            if (r.types.count > 0) error("Duplicate type clause.");
            appendQualifiedNameList(&r.types);
        } else if (match(TOKEN_COLON_GREATER) || match(TOKEN_SPECIALIZES)
                || match(TOKEN_SUBSETS)) {
            if (r.specializes.count > 0) error("Duplicate 'specializes/subsets' clause.");
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
            if (check(TOKEN_STAR) || check(TOKEN_STAR_STAR)) {
                advance();
                wildcard = true;
                break;                          /* :: * (or **) terminates */
            }
            if (!check(TOKEN_IDENTIFIER)) {
                error("Expected name, '*', or '**' after '::'.");
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

/*  attributeDecl ::= "attribute" [IDENTIFIER] featureRelationships?
 *                    ( "=" expression )? ";"
 *
 *  The name is optional when the attribute carries `redefines` or
 *  `specializes` clauses — a common SysML idiom for inheriting and
 *  overriding a feature without re-declaring its name:
 *      attribute :>> fuelMass = 50 [kg];
 *      attribute redefines mass = 75 [kg];                            */
static Node* attributeDecl(void) {
    int line = parser.previous.line;            /* 'attribute' just consumed */
    Token name = {0};
    if (check(TOKEN_IDENTIFIER)) {
        advance();
        name = parser.previous;
    }
    /* If we don't have a name, the next token must introduce a relationship
     * (`:`, `:>`, `:>>`, `redefines`, `subsets`) — otherwise the user
     * really did forget the name.                                      */
    if (name.length == 0
        && !check(TOKEN_COLON) && !check(TOKEN_COLON_GREATER)
        && !check(TOKEN_COLON_GREATER_GREATER) && !check(TOKEN_REDEFINES)
        && !check(TOKEN_SPECIALIZES) && !check(TOKEN_SUBSETS)
        && !check(TOKEN_LEFT_BRACKET)) {
        errorAtCurrent("Expected attribute name.");
    }

    FeatureRels rels = parseFeatureRelationships();

    /* Optional default value, written either `= expression` or the
     * SysML keyword form `default expression`.  Both produce the same
     * AST shape.                                                      */
    Node* defaultValue = NULL;
    if (match(TOKEN_EQUAL) || match(TOKEN_DEFAULT)) {
        defaultValue = expression();
    }

    /* Attributes may carry an inline body to declare overrides for
     * inherited features:
     *     attribute spatialCF: CartesianSpatial3dCoordinateFrame[1]
     *         { :>> mRefs = (m, m, m); }
     * We parse-and-skip the body for now (the AST has no slot for
     * attribute-internal members); just consume tokens to a balanced
     * `}` so the rest of the enclosing scope still parses.            */
    if (match(TOKEN_LEFT_BRACE)) {
        skipBracedBlock();
    } else {
        consume(TOKEN_SEMICOLON, "Expected ';' after attribute.");
    }

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

static const KindInfo kPart       = { DEF_PART,       true,  true,  false, "part"       };
static const KindInfo kPort       = { DEF_PORT,       true,  true,  false, "port"       };
static const KindInfo kInterface  = { DEF_INTERFACE,  false, true,  false, "interface"  };
static const KindInfo kItem       = { DEF_ITEM,       true,  true,  false, "item"       };
static const KindInfo kConnection = { DEF_CONNECTION, false, true,  true,  "connection" };
static const KindInfo kFlow       = { DEF_FLOW,       false, true,  true,  "flow"       };
static const KindInfo kEnd        = { DEF_END,        false, false, false, "end"        };
static const KindInfo kEnum       = { DEF_ENUM,       false, true,  false, "enum"       };
static const KindInfo kConstraint = { DEF_CONSTRAINT, false, true,  true,  "constraint" };
static const KindInfo kRequirement= { DEF_REQUIREMENT,false, true,  false, "requirement"};
static const KindInfo kAction     = { DEF_ACTION,     false, true,  false, "action"     };
static const KindInfo kState      = { DEF_STATE,      false, true,  false, "state"      };
static const KindInfo kCalc       = { DEF_CALC,       false, true,  false, "calc"       };
static const KindInfo kAttributeDef= { DEF_ATTRIBUTE_DEF, false, true, false, "attribute" };
static const KindInfo kOccurrence = { DEF_OCCURRENCE, false, true,  false, "occurrence" };
static const KindInfo kIndividual = { DEF_INDIVIDUAL, false, true,  false, "individual" };
static const KindInfo kSnapshot   = { DEF_SNAPSHOT,   false, true,  false, "snapshot"   };
static const KindInfo kTimeslice  = { DEF_TIMESLICE,  false, true,  false, "timeslice"  };
static const KindInfo kAllocation = { DEF_ALLOCATION, false, true,  false, "allocation" };
static const KindInfo kView       = { DEF_VIEW,       false, true,  false, "view"       };
static const KindInfo kViewpoint  = { DEF_VIEWPOINT,  false, true,  false, "viewpoint"  };
static const KindInfo kRendering  = { DEF_RENDERING,  false, true,  false, "rendering"  };
static const KindInfo kConcern    = { DEF_CONCERN,    false, true,  false, "concern"    };
static const KindInfo kVariant    = { DEF_VARIANT,    false, true,  false, "variant"    };
static const KindInfo kVariation  = { DEF_VARIATION,  false, true,  false, "variation"  };
static const KindInfo kActor      = { DEF_ACTOR,      false, true,  false, "actor"      };
static const KindInfo kUseCase    = { DEF_USE_CASE,   false, true,  false, "use case"   };
static const KindInfo kInclude    = { DEF_INCLUDE,    false, true,  false, "include"    };
static const KindInfo kMessage    = { DEF_MESSAGE,    false, true,  true,  "message"    };
static const KindInfo kMetadata   = { DEF_METADATA,   false, true,  false, "metadata"   };
static const KindInfo kVerification = { DEF_VERIFICATION, false, true, false, "verification" };
static const KindInfo kObjective  = { DEF_OBJECTIVE,  false, true,  true,  "objective"  };
__attribute__((unused))
static const KindInfo kSatisfy    = { DEF_SATISFY,    false, true,  true,  "satisfy"    };
static const KindInfo kAnalysis   = { DEF_ANALYSIS,   false, true,  false, "analysis"   };
static const KindInfo kVerify     = { DEF_VERIFY,     false, true,  false, "verify"     };
static const KindInfo kStakeholder= { DEF_STAKEHOLDER,false, true,  false, "stakeholder"};
static const KindInfo kRender     = { DEF_RENDER,     false, true,  false, "render"     };
/* `subject e : Engine;` doesn't use the definitionOrUsage() dispatcher;
 * subjectStatement() handles it directly, so there's no KindInfo for
 * it.  The DEF_SUBJECT enum value is still useful as a tag on the
 * resulting USAGE node.                                             */
/* ReferenceUsage has no `def` form (defAllowed=false): it's a Usage
 * only.  No keyword to consume — caller invokes usage() directly,
 * not definitionOrUsage().                                          */
static const KindInfo kReference  = { DEF_REFERENCE,  false, false, false, "reference"  };

/* Parse a feature-reference path used as a connect/flow endpoint:
 *
 *      [ '[' expr ']' ]?  IDENT ( ('.' | '::') IDENT )*
 *
 * The optional bracket prefix is a multiplicity that some real SysML
 * code attaches to a connect end (`connect [1] X.Y to [1] A.B`).
 * We accept and silently consume it for now — the AST has no slot
 * for per-end multiplicities yet.                                    */
static Node* dottedReference(void) {
    /* Optional leading `[expr]` — eat it bracket-balanced. */
    if (match(TOKEN_LEFT_BRACKET)) {
        int depth = 1;
        while (depth > 0 && !check(TOKEN_EOF)) {
            if      (match(TOKEN_LEFT_BRACKET))  depth++;
            else if (match(TOKEN_RIGHT_BRACKET)) depth--;
            else advance();
        }
    }
    consume(TOKEN_IDENTIFIER, "Expected reference name.");
    Node* q = astMakeNode(NODE_QUALIFIED_NAME, parser.previous.line);
    astAppendQualifiedPart(q, parser.previous);
    while (check(TOKEN_DOT) || check(TOKEN_COLON_COLON)) {
        advance();
        consume(TOKEN_IDENTIFIER, "Expected name after '.' or '::'.");
        astAppendQualifiedPart(q, parser.previous);
    }
    /* Optional inline refinement: `name :> X.Y` or `name :>> X.Y`
     * or `name redefines X.Y`.  Used in connect-end contexts to
     * declare a one-shot anonymous redefinition.  We parse the
     * trailing qname for syntactic completeness but discard it for
     * now — the AST has no slot for per-end refinements.            */
    if (check(TOKEN_COLON_GREATER) || check(TOKEN_COLON_GREATER_GREATER)
     || check(TOKEN_REDEFINES)     || check(TOKEN_SUBSETS)
     || check(TOKEN_SPECIALIZES)) {
        advance();      /* eat the relationship token */
        /* Eat one dotted reference and discard. */
        if (check(TOKEN_IDENTIFIER)) {
            advance();
            while (check(TOKEN_DOT) || check(TOKEN_COLON_COLON)) {
                advance();
                if (check(TOKEN_IDENTIFIER)) advance();
            }
        }
    }
    return q;
}

/* Parse a "connect a to b" or "from a to b" clause.  Caller has
 * already consumed the opening keyword (CONNECT or FROM).  Appends
 * exactly two reference nodes to the given list.                     */
static void parseEndsClause(NodeList* ends, const char* hint) {
    (void)hint;
    astListAppend(ends, dottedReference());
    consume(TOKEN_TO, "Expected 'to' in this clause.");
    astListAppend(ends, dottedReference());
}

/*  definition ::= "def" [ "<" IDENT ">" ] IDENTIFIER featureRelationships?
 *                 "{" declaration* "}"
 *
 *  The optional `<short>` annotation introduces an alternate name for
 *  the def, used by metadata libraries.  We parse it but currently
 *  drop it — there's no AST slot yet.
 *
 *  Caller has already consumed the kind keyword (e.g. "part", "port").  */
static Node* definition(const KindInfo* k) {
    int line = parser.previous.line;            /* kind keyword just consumed */
    advance();                                  /* eat 'def'                  */
    /* Optional `<short>` alternate-name annotation. */
    if (match(TOKEN_LESS)) {
        if (check(TOKEN_IDENTIFIER)) advance();
        consume(TOKEN_GREATER, "Expected '>' to close alternate-name annotation.");
    }
    consume(TOKEN_IDENTIFIER, "Expected definition name.");
    Token name = parser.previous;

    FeatureRels rels = parseFeatureRelationships();

    Node* def = astMakeNode(NODE_DEFINITION, line);
    def->as.scope.name        = name;
    def->as.scope.defKind     = k->kind;
    def->as.scope.specializes = rels.specializes;
    def->as.scope.redefines   = rels.redefines;
    /* rels.types and rels.multiplicity are silently dropped on a
     * definition; semantic check will warn.                            */

    /* Bare-form definition: `part def Cylinder;` with no body.  Common
     * in real SysML for sentinel/marker defs.  Just terminate.        */
    if (match(TOKEN_SEMICOLON)) {
        return def;
    }
    consume(TOKEN_LEFT_BRACE, "Expected '{' or ';' after definition name.");

    /* Enum def bodies have a different shape: each member is
     * `IDENTIFIER ( "=" expression )? ";"`.  The optional initializer
     * lets values carry an explicit code (e.g. `stop = 0;` for
     * traffic-light enumerations).                                    */
    if (k->kind == DEF_ENUM) {
        while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
            /* Optional bare `enum` keyword introducer for anonymous
             * enum values: `enum = 60 [mm];`.  Eat it; the value is
             * still a regular enum value, just with no name.          */
            bool anon = false;
            if (match(TOKEN_ENUM)) anon = true;

            Token vname = (Token){0};
            if (!anon) {
                if (!match(TOKEN_IDENTIFIER)) {
                    errorAtCurrent("Expected enumeration value name.");
                    advance();
                    if (parser.panicMode) synchronize();
                    continue;
                }
                vname = parser.previous;
            }
            int vline = vname.length > 0 ? vname.line : parser.previous.line;
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
    } else if (k->kind == DEF_CONSTRAINT) {
        /* Constraint def body: `(in|out)? feature*` followed by a
         * single trailing boolean expression.  Parameters are usages
         * (typically with a direction), terminated by ';'.  The body
         * expression has no trailing ';' — it runs to the closing '}'. */
        while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
            /* Heuristic: if the current token starts a declaration
             * (direction, visibility, modifier, kind keyword,
             * `attribute`, `doc`, `comment`), parse one.  Anything
             * else is the start of the body expression.              */
            if (check(TOKEN_IN) || check(TOKEN_OUT) || check(TOKEN_INOUT)
             || check(TOKEN_PUBLIC) || check(TOKEN_PRIVATE) || check(TOKEN_PROTECTED)
             || check(TOKEN_DERIVED) || check(TOKEN_ABSTRACT)
             || check(TOKEN_CONSTANT) || check(TOKEN_REF)
             || check(TOKEN_ATTRIBUTE) || check(TOKEN_DOC) || check(TOKEN_DOC_BODY)
             || check(TOKEN_COMMENT_KW)) {
                Node* m = declaration();
                if (m) astAppendScopeMember(def, m);
                if (parser.panicMode) synchronize();
                continue;
            }
            /* Otherwise, parse the body expression (single per def). */
            if (def->as.scope.body) {
                error("A constraint def may have only one body expression.");
            }
            def->as.scope.body = expression();
            /* Loop again — only the closing '}' or EOF should follow. */
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

    /* Metadata annotations may precede the name in a usage too:
     *     end #logical logicalEnd;
     *     end #original ::> vehicleSpecification.vehicleMassRequirement;
     * Eat them before continuing.                                     */
    skipMetadataPrefix();

    /* ---- optional `<short>` alternate-name annotation -------------- */
    if (match(TOKEN_LESS)) {
        /* Lexeme can be a quoted-name (`<'1'>`) or bare identifier;
         * either way we eat one token then expect `>`.                 */
        if (check(TOKEN_IDENTIFIER)) advance();
        consume(TOKEN_GREATER, "Expected '>' to close alternate-name annotation.");
    }

    /* ---- optional name ---------------------------------------------- */
    Token name = (Token){0};
    Node* anonFlowSource = NULL;        /* set if we reinterpret a name */
    if (check(TOKEN_IDENTIFIER)) {
        advance();
        name = parser.previous;
        /* In flow/message contexts, if the next token is `.`, the
         * "name" we just consumed was actually the first segment of a
         * dotted source reference: `flow a.b to c.d;`.  Build a qname
         * starting from this identifier and treat the flow as
         * anonymous with an implicit `from`-style source ref.          */
        if ((k->kind == DEF_FLOW || k->kind == DEF_MESSAGE)
            && (check(TOKEN_DOT) || check(TOKEN_TO))) {
            anonFlowSource = astMakeNode(NODE_QUALIFIED_NAME, name.line);
            astAppendQualifiedPart(anonFlowSource, name);
            while (match(TOKEN_DOT)) {
                consume(TOKEN_IDENTIFIER, "Expected name after '.'.");
                astAppendQualifiedPart(anonFlowSource, parser.previous);
            }
            name = (Token){0};       /* drop the misread name */
        }
        /* In verify-context (and similar viewpoint kinds), if the
         * "name" we just consumed is followed by `.`, it's actually
         * a dotted reference being verified, not a name.  Reinterpret
         * by building a qname and stashing it; we'll merge it into
         * rels.specializes after parseFeatureRelationships() runs.    */
        if (k->kind == DEF_VERIFY && check(TOKEN_DOT)) {
            anonFlowSource = astMakeNode(NODE_QUALIFIED_NAME, name.line);
            astAppendQualifiedPart(anonFlowSource, name);
            while (match(TOKEN_DOT)) {
                consume(TOKEN_IDENTIFIER, "Expected name after '.'.");
                astAppendQualifiedPart(anonFlowSource, parser.previous);
            }
            name = (Token){0};
        }
    } else {
        /* A bare relationship token (`:`, `:>`, `:>>`, `redefines`,
         * `subsets`, `specializes`, `[`) implies an anonymous usage —
         * common SysML idiom for "redefine / refine an inherited
         * feature without renaming it":
         *     port :>> lugNutCompositePort { … }
         *     attribute redefines mass = 75 [kg];
         * Otherwise enforce the named form per the kind's rules.    */
        bool anonRelOk = check(TOKEN_COLON)
                      || check(TOKEN_COLON_GREATER)
                      || check(TOKEN_COLON_GREATER_GREATER)
                      || check(TOKEN_REDEFINES) || check(TOKEN_SPECIALIZES)
                      || check(TOKEN_SUBSETS)
                      || check(TOKEN_LEFT_BRACKET)
                      /* `attribute = expr;` — anonymous initializer.       */
                      || check(TOKEN_EQUAL)
                      || check(TOKEN_DEFAULT)
                      /* `constraint { expr }` — anonymous inline constraint. */
                      || (k->kind == DEF_CONSTRAINT && check(TOKEN_LEFT_BRACE))
                      /* `connection { end ... }` — anonymous connection
                       * with body (no `connect` clause).  Used with
                       * `#metadata connection { ... }` annotations.       */
                      || ((k->kind == DEF_CONNECTION || k->kind == DEF_INTERFACE)
                          && check(TOKEN_LEFT_BRACE));
        if (!anonRelOk
            && (!k->allowsAnonymous
                || (k->kind == DEF_CONNECTION && !check(TOKEN_CONNECT))
                || (k->kind == DEF_INTERFACE  && !check(TOKEN_CONNECT))
                || (k->kind == DEF_FLOW       && !check(TOKEN_FROM)
                                              && !check(TOKEN_OF))
                || (k->kind == DEF_MESSAGE    && !check(TOKEN_FROM)
                                              && !check(TOKEN_OF)))) {
            consume(TOKEN_IDENTIFIER, "Expected name.");
        }
    }
    /* (Otherwise name stays zero-length — anonymous usage.)            */

    FeatureRels rels = parseFeatureRelationships();

    /* `flow of T from X to Y;` and `message of <name>[:T] from X to Y;`
     * — `of` introduces an item parameter on the flow/message.  In
     * the simplest form `of T` is just a type qname; for messages it
     * frequently carries a name as well: `message of c:IgnitionCmd
     * from a to b;`.  We splice the type into rels.types so downstream
     * code treats it identically to a `: T` clause; the optional name
     * is currently dropped (no AST slot for message item params).    */
    if ((k->kind == DEF_FLOW || k->kind == DEF_MESSAGE)
        && match(TOKEN_OF)) {
        if (k->kind == DEF_MESSAGE && check(TOKEN_IDENTIFIER)) {
            /* Peek for `name : Type`.  Eat the name; if `:` follows,
             * the type is the next qname list, otherwise the name we
             * just ate IS the type qname.                            */
            advance();          /* eat first identifier */
            Token nameOrType = parser.previous;
            if (match(TOKEN_COLON)) {
                appendQualifiedNameList(&rels.types);
            } else {
                /* It was the type, not a name.  Build a one-segment qname.  */
                Node* q = astMakeNode(NODE_QUALIFIED_NAME, nameOrType.line);
                astAppendQualifiedPart(q, nameOrType);
                while (match(TOKEN_COLON_COLON)) {
                    consume(TOKEN_IDENTIFIER, "Expected name after '::'.");
                    astAppendQualifiedPart(q, parser.previous);
                }
                astListAppend(&rels.types, q);
            }
        } else {
            appendQualifiedNameList(&rels.types);
        }
    }

    /* ---- optional endpoint clause ----------------------------------- */
    NodeList ends = {0};
    if (anonFlowSource && k->kind == DEF_VERIFY) {
        /* Reinterpreted name — not a flow source, but a verify target.
         * Splice into rels.specializes so it appears on the usage.    */
        astListAppend(&rels.specializes, anonFlowSource);
        anonFlowSource = NULL;
    }
    if (anonFlowSource) {
        /* Pre-built source ref from the name reinterpretation above.   */
        astListAppend(&ends, anonFlowSource);
        if (match(TOKEN_TO)) {
            astListAppend(&ends, dottedReference());
        }
    } else if ((k->kind == DEF_CONNECTION || k->kind == DEF_INTERFACE)
        && match(TOKEN_CONNECT)) {
        parseEndsClause(&ends, "connector");
    } else if ((k->kind == DEF_FLOW || k->kind == DEF_MESSAGE)
               && match(TOKEN_FROM)) {
        parseEndsClause(&ends, "flow");
    } else if (k->kind == DEF_ALLOCATION && match(TOKEN_ALLOCATE)) {
        /* `allocation <name> : T allocate <src> to <tgt> { ... }`
         * — an allocation usage carrying its own allocate clause.    */
        parseEndsClause(&ends, "allocation");
    } else if (k->kind == DEF_FLOW
               && (check(TOKEN_IDENTIFIER) || check(TOKEN_LEFT_BRACKET))
               && name.length == 0
               && rels.types.count == 0) {
        /* Anonymous unnamed flow short-form: `flow a.b to c.d;`
         * (no `from` keyword).  Real SysML uses this constantly.
         * The `from` is implicit; the first ref is the source.       */
        astListAppend(&ends, dottedReference());
        if (match(TOKEN_TO)) {
            astListAppend(&ends, dottedReference());
        }
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

    /* Action-usage tail clauses we don't yet model:
     *
     *   action trigger1 accept ignitionCmd:T via portRef;
     *   action sendStatus send es via portRef { ... }
     *   action a parallel { ... }
     *   exhibit state s parallel { ... }
     *
     * Parse-and-skip everything after the keyword until the next `;`
     * or `{` so the rest of the body stays parseable.                 */
    if ((k->kind == DEF_ACTION || k->kind == DEF_STATE)
        && (check(TOKEN_ACCEPT) || check(TOKEN_IDENTIFIER))) {
        bool isSend = check(TOKEN_IDENTIFIER) && parser.current.length == 4
                   && memcmp(parser.current.start, "send", 4) == 0;
        bool isParallel = check(TOKEN_IDENTIFIER) && parser.current.length == 8
                       && memcmp(parser.current.start, "parallel", 8) == 0;
        if (check(TOKEN_ACCEPT) || isSend || isParallel) {
            advance();      /* eat the tail keyword */
            while (!check(TOKEN_SEMICOLON) && !check(TOKEN_LEFT_BRACE)
                   && !check(TOKEN_EOF)) {
                advance();
            }
        }
    }

    /* Optional initializer: `= expression` before the closing `;`.
     * Currently used by bare-ref usages (`ref nominalTorque : Real = 100;`)
     * but allowed on any kind for symmetry with attribute usages.  */
    if (match(TOKEN_EQUAL)) {
        u->as.usage.defaultValue = expression();
    }

    /* Constraint usages with an inline expression body: `constraint
     * { mass > 0 }` or `constraint c : C { mass > 0 }`.  The body is
     * a single boolean expression, not a list of member declarations.
     * No trailing `;` is required.                                    */
    if (k->kind == DEF_CONSTRAINT && match(TOKEN_LEFT_BRACE)) {
        u->as.usage.body = expression();
        consume(TOKEN_RIGHT_BRACE, "Expected '}' to close inline constraint body.");
        return u;
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
    /* Constraint usages historically required an `assert`/`assume`/
     * `require` prefix (a v0.2 design choice).  Real SysML files use
     * the bare form constantly — `constraint { x > 0 }`,
     * `constraint c : C;` — so we now accept both.  The asserted
     * forms are handled separately by assertedConstraintOrRequirement
     * and reach this path only when no assertion keyword was seen.   */
    Node* u = usage(k, dir);
    if (u && u->kind == NODE_USAGE) {
        u->as.usage.isDerived           = m.isDerived;
        u->as.usage.isAbstract          = m.isAbstract;
        u->as.usage.isConstant          = m.isConstant;
        u->as.usage.isReference         = m.isReference;
        u->as.usage.isReferenceExplicit = m.isReference;
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

/* `bind X = Y;` — connect-like equality between two feature references.
 * Stored as a connection usage with isBind=true and ends = [lhs, rhs].
 * The `=` is part of the syntax, not a default-value assignment.     */
static Node* bindStatement(void) {
    int line = parser.previous.line;            /* TOKEN_BIND consumed   */
    Node* u = astMakeNode(NODE_USAGE, line);
    u->as.usage.defKind = DEF_CONNECTION;
    u->as.usage.isBind  = true;
    astListAppend(&u->as.usage.ends, dottedReference());
    consume(TOKEN_EQUAL, "Expected '=' in bind statement.");
    astListAppend(&u->as.usage.ends, dottedReference());
    consume(TOKEN_SEMICOLON, "Expected ';' after bind statement.");
    return u;
}

/* `allocate X to Y [ { body } ];` — connection variant indicating an
 * allocation relationship, optionally with a body of cross-references.
 * Body content is parse-and-skipped for now (not modeled in AST).    */
static Node* allocateStatement(void) {
    int line = parser.previous.line;            /* TOKEN_ALLOCATE eaten  */
    Node* u = astMakeNode(NODE_USAGE, line);
    u->as.usage.defKind    = DEF_CONNECTION;
    u->as.usage.isAllocate = true;
    /* Optional name: `allocate myAlloc : SomeType allocate X to Y` is
     * sometimes seen, but the simpler `allocate X to Y` form is more
     * common.  We support both: an identifier here means the allocate
     * has a name, otherwise it's anonymous.  Distinguish by lookahead:
     * if the second token after IDENTIFIER is `to`, it's an end ref.   */
    /* For v0.7 we just go with anonymous form. */
    parseEndsClause(&u->as.usage.ends, "allocate");
    /* Optional body — parse-and-skip until matching `}`.              */
    if (match(TOKEN_LEFT_BRACE)) {
        skipBracedBlock();
        return u;
    }
    consume(TOKEN_SEMICOLON, "Expected ';' or '{' after allocate ends.");
    return u;
}

/*  assertion ::= ("assert" | "assume" | "require")
 *                  ( "constraint" | "requirement" )
 *                  ( IDENTIFIER (": " qualifiedName)? ";"
 *                    | "{" expression "}"           // anon inline
 *                    | ":" qualifiedName ";" )      // unnamed typed
 *
 *  The caller has already consumed the assertion keyword and stored it
 *  in `kw`.  This helper picks up from `constraint`/`requirement` and
 *  builds the usage.                                                   */
static Node* assertedConstraintOrRequirement(AssertKind kw, int kwLine) {
    /* Choose which kind of usage we're parsing. */
    bool isReq = false;
    if (match(TOKEN_REQUIREMENT))      isReq = true;
    else if (match(TOKEN_CONSTRAINT))  isReq = false;
    else if (check(TOKEN_IDENTIFIER)) {
        /* `assert vehicleSpecification;` and `require transportRequirements;`
         * — bare-reference assertion form.  The referent is a known
         * requirement; no `constraint`/`requirement` keyword between.
         * Build a requirement usage that just specializes the named ref.   */
        isReq = true;
        Node* u = astMakeNode(NODE_USAGE, kwLine);
        u->as.usage.defKind    = DEF_REQUIREMENT;
        u->as.usage.assertKind = kw;
        astListAppend(&u->as.usage.specializes, dottedReference());
        consume(TOKEN_SEMICOLON, "Expected ';' after bare-reference assertion.");
        return u;
    }
    else {
        errorAtCurrent("Expected 'constraint' or 'requirement' after "
                       "assertion keyword.");
        advance();
        return NULL;
    }
    /* `assume` only makes sense on a constraint.  `require` is fine on
     * either (require constraint, require requirement).                 */
    if (kw == ASSERT_ASSUME && isReq) {
        error("'assume' is only valid before 'constraint'.");
    }

    Node* u = astMakeNode(NODE_USAGE, kwLine);
    u->as.usage.defKind    = isReq ? DEF_REQUIREMENT : DEF_CONSTRAINT;
    u->as.usage.assertKind = kw;

    /* Anonymous inline form: `assert constraint { expression }`. */
    if (match(TOKEN_LEFT_BRACE)) {
        if (isReq) {
            error("Inline body form is only valid for constraints.");
        }
        u->as.usage.body = expression();
        consume(TOKEN_RIGHT_BRACE, "Expected '}' to close inline constraint body.");
        return u;
    }

    /* Named or typed form.  Optional name, then optional `: TypeRef`. */
    if (check(TOKEN_IDENTIFIER)) {
        advance();
        u->as.usage.name = parser.previous;
    }
    if (match(TOKEN_COLON)) {
        appendQualifiedNameList(&u->as.usage.types);
    }
    /* Optional inline override body after `: T`: rare, but allowed. */
    if (match(TOKEN_LEFT_BRACE)) {
        if (isReq) {
            error("Inline body is not valid on a requirement usage.");
        }
        u->as.usage.body = expression();
        consume(TOKEN_RIGHT_BRACE, "Expected '}' to close inline constraint body.");
        return u;
    }

    consume(TOKEN_SEMICOLON, "Expected ';' after asserted constraint/requirement.");
    return u;
}

/*  subject ::= "subject" IDENTIFIER (":" qualifiedName)? ";"
 *
 *  Caller has consumed TOKEN_SUBJECT.  Models the subject as a USAGE
 *  with DEF_SUBJECT kind so it round-trips and validates uniformly
 *  with other named features.                                         */
static Node* subjectStatement(void) {
    int line = parser.previous.line;
    Node* u = astMakeNode(NODE_USAGE, line);
    u->as.usage.defKind = DEF_SUBJECT;

    /* Optional name (or anonymous if next is `=`/`:`/etc).
     *
     * Real SysML files use these forms:
     *   subject vehicle;
     *   subject vehicle : Vehicle;
     *   subject vehicle :> baseVehicle;
     *   subject vehicle [2] :> alternatives;
     *   subject = expr;                       — anonymous, initializer
     *   subject vehicle default expr;         — name + initializer
     *   subject vehicle { ... }               — name + body
     */
    if (check(TOKEN_IDENTIFIER)) {
        advance();
        u->as.usage.name = parser.previous;
    }

    /* Optional feature-relationship tail: `: T`, `:> X`, `[n]`, etc. */
    FeatureRels rels = parseFeatureRelationships();
    u->as.usage.types        = rels.types;
    u->as.usage.specializes  = rels.specializes;
    u->as.usage.redefines    = rels.redefines;
    u->as.usage.multiplicity = rels.multiplicity;

    /* Optional initializer (`= expr` or `default expr`).            */
    if (match(TOKEN_EQUAL) || match(TOKEN_DEFAULT)) {
        u->as.usage.defaultValue = expression();
    }

    /* Optional inline body — parse-and-skipped (no AST slot for
     * subject members yet).                                         */
    if (match(TOKEN_LEFT_BRACE)) {
        skipBracedBlock();
    } else {
        consume(TOKEN_SEMICOLON, "Expected ';' after subject.");
    }
    return u;
}

/* Parse a single succession endpoint — either `start`, `done`, or a
 * regular qualified-name reference.  The keyword forms produce a
 * one-segment qname pre-resolved to the matching builtin so the
 * resolver doesn't try to look them up in scope.                    */
static Node* parseActionRef(void) {
    if (check(TOKEN_START) || check(TOKEN_DONE)) {
        bool isStart = check(TOKEN_START);
        int line = parser.current.line;
        advance();                              /* eat the keyword     */
        /* Build a synthetic qname; the lexeme is the keyword text
         * itself, which is fine for the printer/JSON.                */
        Node* q = astMakeNode(NODE_QUALIFIED_NAME, line);
        astAppendQualifiedPart(q, parser.previous);
        q->as.qualifiedName.resolved = isStart ? builtinStart() : builtinDone();
        return q;
    }
    /* Action references in succession/transition contexts may use
     * either `::` (namespace) or `.` (member access through parts).
     * Use the dottedReference path so `vehicle.doorClosed` parses.  */
    return dottedReference();
}

/* Parse the target of a `then` clause.  Either a reference (qname or
 * start/done) or an inline action declaration `action name { ... }`.
 * For an inline action, we synthesize a USAGE with DEF_ACTION kind and
 * give it a brace-delimited body.                                    */
static Node* parseThenTarget(void) {
    if (match(TOKEN_ACTION)) {
        /* Inline `then action a [: T] [accept/send/parallel <stuff>] [{ … }];` form. */
        Node* u = astMakeNode(NODE_USAGE, parser.previous.line);
        u->as.usage.defKind = DEF_ACTION;
        if (check(TOKEN_IDENTIFIER)) {
            advance();
            u->as.usage.name = parser.previous;
        }
        if (match(TOKEN_COLON)) {
            appendQualifiedNameList(&u->as.usage.types);
        }
        /* Accept/send/parallel tail clauses on the inline action.    */
        bool isSend = check(TOKEN_IDENTIFIER) && parser.current.length == 4
                   && memcmp(parser.current.start, "send", 4) == 0;
        bool isParallel = check(TOKEN_IDENTIFIER) && parser.current.length == 8
                       && memcmp(parser.current.start, "parallel", 8) == 0;
        if (check(TOKEN_ACCEPT) || isSend || isParallel) {
            advance();
            while (!check(TOKEN_SEMICOLON) && !check(TOKEN_LEFT_BRACE)
                   && !check(TOKEN_THEN) && !check(TOKEN_EOF)) {
                advance();
            }
        }
        if (match(TOKEN_LEFT_BRACE)) {
            while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
                Node* m = declaration();
                if (m) astAppendUsageMember(u, m);
                if (parser.panicMode) synchronize();
            }
            consume(TOKEN_RIGHT_BRACE, "Expected '}' to close inline action body.");
        }
        return u;
    }
    /* `then event <ref>;` — succession target is an event reference.
     * Eat the `event` keyword and read the rest as a dotted ref.    */
    if (match(TOKEN_EVENT)) {
        /* Optional `occurrence` keyword between `event` and the ref. */
        (void)match(TOKEN_OCCURRENCE);
        return dottedReference();
    }
    /* `then fork <name>;` and `then join <name>;` — fork/join are not
     * SysML keywords in our lexer but appear as bare identifiers
     * followed by a name in succession contexts.  Recognize the
     * pattern by matching the literal text and producing a synthetic
     * action usage that wraps the fork/join name.                    */
    if (check(TOKEN_IDENTIFIER)
        && ((parser.current.length == 4
             && memcmp(parser.current.start, "fork", 4) == 0)
         || (parser.current.length == 4
             && memcmp(parser.current.start, "join", 4) == 0))) {
        Token forkOrJoin = parser.current;
        /* Lookahead: `fork <name>` is a fork node; otherwise it's just
         * an identifier reference.  Peek at the next token after the
         * keyword by tentatively advancing.                           */
        advance();
        if (check(TOKEN_IDENTIFIER)) {
            advance();
            Node* u = astMakeNode(NODE_USAGE, forkOrJoin.line);
            u->as.usage.defKind = DEF_ACTION;
            u->as.usage.name    = parser.previous;
            return u;
        }
        /* No following identifier — treat the keyword itself as the
         * action ref name.                                            */
        Node* q = astMakeNode(NODE_QUALIFIED_NAME, forkOrJoin.line);
        astAppendQualifiedPart(q, forkOrJoin);
        return q;
    }
    return parseActionRef();
}

/*  succession ::= "succession" [IDENTIFIER] [ "first" ref ] "then" target (";" | "then" target ...)
 *               | "first" ref ";"
 *               | "then" target ";"
 *
 *  The standalone `succession` form uses the named relationship; the
 *  bare `first`/`then` forms are inline statements inside an action
 *  def body that build a chain implicitly across consecutive lines.
 *
 *  Caller has consumed the leading keyword (TOKEN_SUCCESSION /
 *  TOKEN_FIRST / TOKEN_THEN); `kw` indicates which.                   */
static Node* parseSuccessionStmt(TokenType kw) {
    int line = parser.previous.line;
    Node* s = astMakeNode(NODE_SUCCESSION, line);

    if (kw == TOKEN_SUCCESSION) {
        /* Optional name. */
        if (check(TOKEN_IDENTIFIER)) {
            advance();
            s->as.succession.name = parser.previous;
        }
        /* Optional `first ref`. */
        if (match(TOKEN_FIRST)) {
            s->as.succession.first = parseActionRef();
        }
        /* Required `then target`. */
        consume(TOKEN_THEN, "Expected 'then' in succession.");
        astListAppend(&s->as.succession.targets, parseThenTarget());
        /* Additional `then` chain. */
        while (match(TOKEN_THEN)) {
            astListAppend(&s->as.succession.targets, parseThenTarget());
        }
    } else if (kw == TOKEN_FIRST) {
        /* `first ref [ then target ]+ ;` — record the first reference,
         * then accept any `then` chain that follows.                  */
        s->as.succession.first = parseActionRef();
        while (match(TOKEN_THEN)) {
            astListAppend(&s->as.succession.targets, parseThenTarget());
        }
    } else {
        /* TOKEN_THEN — bare continuation.  No `first`; just one or
         * more targets (rare, but the spec allows `then a then b;`). */
        astListAppend(&s->as.succession.targets, parseThenTarget());
        while (match(TOKEN_THEN)) {
            astListAppend(&s->as.succession.targets, parseThenTarget());
        }
    }

    /* Trailing `;` is optional when the last `then` target was an
     * inline action with a body — the closing `}` is a natural
     * terminator and real SysML files routinely omit the `;`.        */
    Node* lastTarget = s->as.succession.targets.count > 0
        ? s->as.succession.targets.items[s->as.succession.targets.count - 1]
        : NULL;
    bool lastWasInlineWithBody = lastTarget
        && lastTarget->kind == NODE_USAGE
        && lastTarget->as.usage.memberCount > 0;
    if (lastWasInlineWithBody) {
        (void)match(TOKEN_SEMICOLON);
    } else {
        consume(TOKEN_SEMICOLON, "Expected ';' after succession.");
    }
    return s;
}

/*  lifecycleAction ::= ("entry" | "do" | "exit") qualifiedName ";"
 *
 *  Caller has consumed the lifecycle keyword.  The single argument is
 *  a reference to an action (resolved later).  Inline declaration
 *  forms (`entry action initial;`) are flagged as unsupported here —
 *  the parser eats the keyword and reports a clear error.            */
static Node* parseLifecycleAction(LifecycleKind kind) {
    int line = parser.previous.line;
    Node* la = astMakeNode(NODE_LIFECYCLE_ACTION, line);
    la->as.lifecycleAction.kind = kind;

    /* `entry action <name>;` and similar inline forms — the `action`
     * keyword is optional sugar that affirms the lifecycle target is
     * an action.  We don't yet synthesize a separate action decl so
     * the keyword is silently consumed and we treat the rest as a
     * regular action ref.                                            */
    (void)match(TOKEN_ACTION);
    /* Accept dotted paths too: `entry vehicle.driveAction;`.         */
    la->as.lifecycleAction.action = dottedReference();
    /* Optional inline body — `do senseTemperature { ... }`.  When a
     * body is present, the `}` terminates the lifecycle statement and
     * no trailing `;` is required.  Without a body, `;` is required.   */
    if (match(TOKEN_LEFT_BRACE)) {
        skipBracedBlock();
        (void)match(TOKEN_SEMICOLON);   /* tolerate `};` if author wrote one */
    } else {
        consume(TOKEN_SEMICOLON, "Expected ';' after lifecycle action.");
    }
    return la;
}

/*  transition ::= "transition" [IDENTIFIER]
 *                   [ "first"  qualifiedName ]
 *                   [ "accept" qualifiedName ]
 *                   [ "if"     expression    ]
 *                   [ "do"     qualifiedName ]
 *                 "then" qualifiedName ";"
 *
 *  Caller has consumed TOKEN_TRANSITION.  Surface forms vary widely;
 *  the simple form `transition initial then off;` has only first and
 *  target.  Unsupported sub-forms (accept-via, accept-at, accept-when,
 *  send-effect, multi-target) are flagged with clear errors.        */
static Node* parseTransitionStmt(void) {
    int line = parser.previous.line;
    Node* t = astMakeNode(NODE_TRANSITION, line);

    /* Optional name vs implicit-first disambiguation:
     *   `transition initial then off;`        — first=initial, no name
     *   `transition off_to_on first off …;`   — name=off_to_on, first=off
     * If we see `<id> then`, the identifier is a `first` reference;
     * otherwise it's the transition's name.                          */
    if (check(TOKEN_IDENTIFIER)) {
        Token candidate = parser.current;
        advance();              /* tentatively consume                */
        if (check(TOKEN_THEN)) {
            /* No name; identifier is the implicit `first` ref.       */
            Node* q = astMakeNode(NODE_QUALIFIED_NAME, candidate.line);
            astAppendQualifiedPart(q, candidate);
            t->as.transition.first = q;
        } else {
            t->as.transition.name = candidate;
        }
    }
    /* Optional explicit `first <qname>`. */
    if (match(TOKEN_FIRST)) {
        t->as.transition.first = qualifiedName();
    }
    /* Optional `accept <event>` and its sub-forms.
     *
     *   accept e               : simple event ref
     *   accept e:T             : typed event ref
     *   accept e via port      : with via clause
     *   accept at <expr>       : time-based — `accept at maintenanceTime`
     *   accept when <expr>     : guard-based — `accept when temp > Tmax`
     *
     * The simple/typed/via forms produce a qname accept; the at/when
     * forms are parse-and-skipped (they need typed AST to model
     * properly).                                                      */
    if (match(TOKEN_ACCEPT)) {
        /* Detect the `at`/`when` shapes by literal text — neither is
         * a true keyword in our lexer, so we have to peek manually.   */
        bool atSubform =
            check(TOKEN_IDENTIFIER)
            && ((parser.current.length == 2
                 && memcmp(parser.current.start, "at", 2) == 0)
             || (parser.current.length == 4
                 && memcmp(parser.current.start, "when", 4) == 0));
        if (atSubform) {
            /* Eat tokens until we hit a transition continuation kw.    */
            while (!check(TOKEN_IF) && !check(TOKEN_DO) && !check(TOKEN_THEN)
                   && !check(TOKEN_SEMICOLON) && !check(TOKEN_EOF)) {
                advance();
            }
        } else if (check(TOKEN_IDENTIFIER) || check(TOKEN_START) || check(TOKEN_DONE)) {
            t->as.transition.accept = qualifiedName();
            if (match(TOKEN_COLON)) {
                qualifiedName();
            }
            if (check(TOKEN_IDENTIFIER) && parser.current.length == 3
                && memcmp(parser.current.start, "via", 3) == 0) {
                advance();
                qualifiedName();
            }
        } else {
            /* Other unfamiliar forms — eat to the next continuation.   */
            while (!check(TOKEN_IF) && !check(TOKEN_DO) && !check(TOKEN_THEN)
                   && !check(TOKEN_SEMICOLON) && !check(TOKEN_EOF)) {
                advance();
            }
        }
    }
    /* Optional `if <expression>`. */
    if (match(TOKEN_IF)) {
        t->as.transition.guard = expression();
    }
    /* Optional `do <action>`.  Simple form is `do <qname>`.  The
     * `do send new T() to P` effect form is parse-and-skipped.       */
    if (match(TOKEN_DO)) {
        if (check(TOKEN_IDENTIFIER) && parser.current.length == 4
            && memcmp(parser.current.start, "send", 4) == 0) {
            /* Eat tokens up to `then` / `;`.                            */
            while (!check(TOKEN_THEN) && !check(TOKEN_SEMICOLON) && !check(TOKEN_EOF)) {
                advance();
            }
        } else {
            t->as.transition.effect = qualifiedName();
        }
    }
    /* Required `then <target>`. */
    consume(TOKEN_THEN, "Expected 'then' clause in transition.");
    t->as.transition.target = qualifiedName();

    consume(TOKEN_SEMICOLON, "Expected ';' after transition.");
    return t;
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

    /* Two surface forms:
     *   dependency [<name>] from <srcs> to <tgts>;
     *   dependency <src> to <tgts>;            — no `from`, no name
     *
     * For the no-`from` form the IDENTIFIER (or qname) right after
     * `dependency` is the single source.                            */
    Token name = {0};
    NodeList sources = {0};
    NodeList targets = {0};

    if (check(TOKEN_IDENTIFIER)) {
        advance();
        name = parser.previous;
    }

    if (match(TOKEN_FROM)) {
        appendQualifiedNameList(&sources);
        consume(TOKEN_TO, "Expected 'to' in dependency.");
        appendQualifiedNameList(&targets);
    } else if (match(TOKEN_TO)) {
        /* No `from` — the name we just consumed is the source.  Wrap
         * it as a single-segment qname and clear the name slot.     */
        if (name.length == 0) {
            errorAtCurrent("Expected source name before 'to' in dependency.");
        } else {
            Node* q = astMakeNode(NODE_QUALIFIED_NAME, line);
            astAppendQualifiedPart(q, name);
            astListAppend(&sources, q);
            name = (Token){0};
        }
        appendQualifiedNameList(&targets);
    } else {
        errorAtCurrent("Expected 'from' or 'to' in dependency.");
    }

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
/* Skip over a brace-delimited block — counts braces so it tolerates
 * nesting.  Used by skipMetadataPrefix() when consuming `@X { ... }`
 * annotations.  Caller has just consumed the opening '{'.            */
static void skipBracedBlock(void) {
    int depth = 1;
    while (depth > 0 && !check(TOKEN_EOF)) {
        if      (match(TOKEN_LEFT_BRACE))  depth++;
        else if (match(TOKEN_RIGHT_BRACE)) depth--;
        else advance();
    }
}

/* Recognize and silently consume any leading metadata annotations:
 *
 *   #<identifier>                     short-form metadata application
 *   @<identifier> [ { ... } ]         full metadata application with body
 *   @<identifier> about <qname-list>  not yet supported, silently skipped
 *
 * These commonly precede a real declaration: `#mop attribute mass = …;`
 * or `#logical part vehicleLogical : Vehicle { … }`.  We don't yet
 * model metadata in the AST; for the parser to advance, we just consume
 * the syntax.  Multiple metadata applications can chain, so we loop.   */
/* Return true if the metadata block we just ate was a complete
 * statement (`@X { … };` or `@X about Y;` or just `@X;`).  In that
 * case the caller should treat the metadata as a standalone item
 * and not look for a follow-on declaration — common form is
 *     part bumper { @Safety { isMandatory = true; } }
 * where `@Safety { … }` is the only member of the body.            */
static bool skipMetadataPrefixIsStatement(void) {
    bool wasStatement = false;
    for (;;) {
        if (match(TOKEN_HASH)) {
            if (check(TOKEN_IDENTIFIER)) advance();
            while (match(TOKEN_COLON_COLON)) {
                if (check(TOKEN_IDENTIFIER)) advance();
            }
            continue;
        }
        if (match(TOKEN_AT)) {
            if (check(TOKEN_IDENTIFIER)) advance();
            while (match(TOKEN_COLON_COLON)) {
                if (check(TOKEN_IDENTIFIER)) advance();
            }
            if (match(TOKEN_ABOUT)) {
                while (!check(TOKEN_LEFT_BRACE) && !check(TOKEN_SEMICOLON)
                       && !check(TOKEN_EOF)) {
                    advance();
                }
                wasStatement = true;
            }
            if (match(TOKEN_LEFT_BRACE)) {
                skipBracedBlock();
                wasStatement = true;     /* `@X { … }` is a complete stmt */
            }
            if (match(TOKEN_SEMICOLON)) wasStatement = true;
            continue;
        }
        break;
    }
    return wasStatement;
}

static void skipMetadataPrefix(void) {
    (void)skipMetadataPrefixIsStatement();
}


Node* declaration(void) {
    /* Metadata annotations may precede any declaration; if they form
     * a complete statement on their own (e.g. `@Safety { … }`), we
     * return immediately so callers don't keep looking for a kind
     * keyword.                                                       */
    bool metaWasStatement = skipMetadataPrefixIsStatement();
    if (metaWasStatement
        && (check(TOKEN_RIGHT_BRACE) || check(TOKEN_EOF))) {
        return NULL;
    }

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
    } else if (check(TOKEN_IDENTIFIER)
               && (parser.current.length == 4
                   && (memcmp(parser.current.start, "fork", 4) == 0
                    || memcmp(parser.current.start, "join", 4) == 0))) {
        /* `fork fork1;` and `join join1;` — bare-keyword fork/join
         * declarations that introduce a control-flow node.  Synthesize
         * an action usage so the rest of the pipeline doesn't choke. */
        Token forkOrJoin = parser.current;
        advance();      /* eat fork/join */
        Node* u = astMakeNode(NODE_USAGE, forkOrJoin.line);
        u->as.usage.defKind = DEF_ACTION;
        if (check(TOKEN_IDENTIFIER)) {
            advance();
            u->as.usage.name = parser.previous;
        }
        consume(TOKEN_SEMICOLON, "Expected ';' after fork/join declaration.");
        result = u;
    } else if (check(TOKEN_COLON_GREATER) || check(TOKEN_COLON_GREATER_GREATER)
            || check(TOKEN_REDEFINES)     || check(TOKEN_SUBSETS)) {
        /* Bare-prefix anonymous attribute redef: `:>> X::Y = expr;`,
         * `:> X = expr;`, `redefines X = expr;`.  Common inside snapshot
         * bodies where the user declares concrete values for inherited
         * features without re-declaring their kind.  attributeDecl()
         * already handles unnamed attributes that start with one of
         * these tokens.                                                */
        if (dir != DIR_NONE) error("Direction modifier is not valid on anonymous redef.");
        result = attributeDecl();
        if (result && result->kind == NODE_ATTRIBUTE) {
            result->as.attribute.isDerived           = mods.isDerived;
            result->as.attribute.isAbstract          = mods.isAbstract;
            result->as.attribute.isConstant          = mods.isConstant;
            result->as.attribute.isReference         = mods.isReference;
            result->as.attribute.isReferenceExplicit = mods.isReference;
        }
    } else if (match(TOKEN_PART))       result = definitionOrUsage(&kPart,       dir, mods);
    else if (match(TOKEN_PORT))         result = definitionOrUsage(&kPort,       dir, mods);
    else if (match(TOKEN_INTERFACE))    result = definitionOrUsage(&kInterface,  dir, mods);
    else if (match(TOKEN_ITEM))         result = definitionOrUsage(&kItem,       dir, mods);
    else if (match(TOKEN_CONNECTION))   result = definitionOrUsage(&kConnection, dir, mods);
    else if (match(TOKEN_FLOW))         result = definitionOrUsage(&kFlow,       dir, mods);
    else if (match(TOKEN_END))          result = definitionOrUsage(&kEnd,        dir, mods);
    else if (match(TOKEN_ENUM))         result = definitionOrUsage(&kEnum,       dir, mods);
    else if (match(TOKEN_CONSTRAINT))   result = definitionOrUsage(&kConstraint, dir, mods);
    else if (match(TOKEN_REQUIREMENT))  result = definitionOrUsage(&kRequirement,dir, mods);
    else if (match(TOKEN_ACTION))       result = definitionOrUsage(&kAction,     dir, mods);
    else if (match(TOKEN_STATE))        result = definitionOrUsage(&kState,      dir, mods);

    /* Tier 2 keywords — same KindInfo path as the structural kinds.
     * `event occurrence x;` is a special case: the `event` keyword
     * sets the event flag and the rest of the parse continues as an
     * occurrence usage.  We model that by recognizing TOKEN_EVENT,
     * setting the flag on the result, and recursing.                  */
    else if (match(TOKEN_OCCURRENCE))   result = definitionOrUsage(&kOccurrence, dir, mods);
    else if (match(TOKEN_EVENT)) {
        /* `event occurrence x;` — full form.  Eat `occurrence`, parse
         * as occurrence usage, set defKind to DEF_EVENT.
         * `event <ref>.<port>;` — short form for declaring an event
         * usage that references an existing occurrence.  Parse as a
         * synthetic event usage with the qname stored in `types`.    */
        if (dir != DIR_NONE) error("Direction modifier is not valid on event.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on event.");
        if (match(TOKEN_OCCURRENCE)) {
            result = definitionOrUsage(&kOccurrence, dir, mods);
            if (result && result->kind == NODE_USAGE) {
                result->as.usage.defKind = DEF_EVENT;
            }
        } else {
            /* Short form: just an event-ref statement.  Eat the
             * dotted reference and trailing `;`.                       */
            int line = parser.previous.line;
            Node* u = astMakeNode(NODE_USAGE, line);
            u->as.usage.defKind = DEF_EVENT;
            astListAppend(&u->as.usage.types, dottedReference());
            consume(TOKEN_SEMICOLON, "Expected ';' after event reference.");
            result = u;
        }
    }
    else if (match(TOKEN_INDIVIDUAL))   result = definitionOrUsage(&kIndividual, dir, mods);
    else if (match(TOKEN_SNAPSHOT))     result = definitionOrUsage(&kSnapshot,   dir, mods);
    else if (match(TOKEN_TIMESLICE))    result = definitionOrUsage(&kTimeslice,  dir, mods);
    else if (match(TOKEN_ALLOCATION))   result = definitionOrUsage(&kAllocation, dir, mods);
    else if (match(TOKEN_VIEW))         result = definitionOrUsage(&kView,       dir, mods);
    else if (match(TOKEN_VIEWPOINT))    result = definitionOrUsage(&kViewpoint,  dir, mods);
    else if (match(TOKEN_RENDERING))    result = definitionOrUsage(&kRendering,  dir, mods);
    else if (match(TOKEN_CONCERN))      result = definitionOrUsage(&kConcern,    dir, mods);
    else if (match(TOKEN_VARIANT)) {
        /* `variant <kind> name : T;` — variant prefix on another kind.
         * Or just `variant name : T;` for an anonymous variant.       */
        if (check(TOKEN_PART) || check(TOKEN_PORT) || check(TOKEN_ITEM)
         || check(TOKEN_ATTRIBUTE) || check(TOKEN_ACTION) || check(TOKEN_STATE)) {
            /* Re-dispatch on the inner kind keyword with the variant
             * marker discarded; the AST has no variant flag yet so we
             * fall back to the inner kind's normal handling.           */
            return declaration();   /* recurse — current token is the inner kind */
        }
        result = definitionOrUsage(&kVariant,    dir, mods);
    }
    else if (match(TOKEN_VARIATION)) {
        /* `variation part def X { ... }` — variation prefix on def.   */
        if (check(TOKEN_PART) || check(TOKEN_PORT) || check(TOKEN_ITEM)
         || check(TOKEN_ATTRIBUTE) || check(TOKEN_ACTION) || check(TOKEN_STATE)) {
            return declaration();
        }
        result = definitionOrUsage(&kVariation,  dir, mods);
    }
    else if (match(TOKEN_ACTOR))        result = definitionOrUsage(&kActor,      dir, mods);
    else if (match(TOKEN_INCLUDE)) {
        /* `include use case <name> :> <ref>;` — common form for
         * including a use case usage in another use case body.  We
         * eat the `use case` introducer if present and treat it
         * uniformly with bare `include <name>`.                       */
        if (match(TOKEN_USE)) {
            if (check(TOKEN_IDENTIFIER) && parser.current.length == 4
                && memcmp(parser.current.start, "case", 4) == 0) {
                advance();      /* eat 'case' */
            }
        }
        result = definitionOrUsage(&kInclude, dir, mods);
    }
    else if (match(TOKEN_MESSAGE))      result = definitionOrUsage(&kMessage,    dir, mods);
    else if (match(TOKEN_METADATA))     result = definitionOrUsage(&kMetadata,   dir, mods);
    else if (match(TOKEN_VERIFICATION)) result = definitionOrUsage(&kVerification, dir, mods);
    else if (match(TOKEN_OBJECTIVE))    result = definitionOrUsage(&kObjective,  dir, mods);
    else if (match(TOKEN_ANALYSIS))     result = definitionOrUsage(&kAnalysis,   dir, mods);
    else if (match(TOKEN_VERIFY))       result = definitionOrUsage(&kVerify,     dir, mods);
    else if (match(TOKEN_STAKEHOLDER))  result = definitionOrUsage(&kStakeholder,dir, mods);
    else if (match(TOKEN_RENDER))       result = definitionOrUsage(&kRender,     dir, mods);
    else if (match(TOKEN_FRAME)) {
        /* `frame concern X:T;` — frame prefix on a concern usage.    */
        if (dir != DIR_NONE) error("Direction modifier is not valid on frame.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on frame.");
        if (check(TOKEN_CONCERN)) {
            advance();      /* eat 'concern' */
            result = definitionOrUsage(&kConcern, DIR_NONE, (FeatureModifiers){0});
        } else {
            /* Bare frame.  Treat the rest as a concern usage anyway. */
            result = definitionOrUsage(&kConcern, DIR_NONE, (FeatureModifiers){0});
        }
        if (result && result->kind == NODE_USAGE) {
            result->as.usage.defKind = DEF_FRAME;
        }
    }
    else if (match(TOKEN_EXPOSE)) {
        /* `expose <qname>;` — view-body statement.  Like filter,
         * just parse-and-skip until ';'.                              */
        if (dir != DIR_NONE) error("Direction modifier is not valid on expose.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on expose.");
        while (!check(TOKEN_SEMICOLON) && !check(TOKEN_EOF)) advance();
        match(TOKEN_SEMICOLON);
        result = NULL;
    }
    else if (match(TOKEN_SATISFY)) {
        /* `satisfy [requirement] [<name>:]<qname> [by <qname>] [{ body }];`
         * The `requirement` introducer is optional sugar (mirrors the
         * `assert constraint`/`assert requirement` shape).  An optional
         * `<name>:` may also precede the requirement reference for
         * named satisfies.                                            */
        if (dir != DIR_NONE) error("Direction modifier is not valid on satisfy.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on satisfy.");
        int line = parser.previous.line;
        Node* u = astMakeNode(NODE_USAGE, line);
        u->as.usage.defKind = DEF_SATISFY;
        (void)match(TOKEN_REQUIREMENT);
        /* Lookahead: `<name>:<qname>` form has IDENTIFIER then COLON.
         * If both are present, eat them and use the qname after.      */
        if (check(TOKEN_IDENTIFIER)) {
            Token cand = parser.current;
            advance();
            if (match(TOKEN_COLON)) {
                u->as.usage.name = cand;
                astListAppend(&u->as.usage.types, dottedReference());
            } else {
                /* It WAS the qname (single segment).                  */
                Node* q = astMakeNode(NODE_QUALIFIED_NAME, cand.line);
                astAppendQualifiedPart(q, cand);
                while (match(TOKEN_DOT) || match(TOKEN_COLON_COLON)) {
                    consume(TOKEN_IDENTIFIER, "Expected name after '.' or '::'.");
                    astAppendQualifiedPart(q, parser.previous);
                }
                astListAppend(&u->as.usage.types, q);
            }
        } else {
            astListAppend(&u->as.usage.types, dottedReference());
        }
        if (check(TOKEN_IDENTIFIER) && parser.current.length == 2
            && memcmp(parser.current.start, "by", 2) == 0) {
            advance();      /* eat 'by' */
            astListAppend(&u->as.usage.ends, dottedReference());
        }
        if (match(TOKEN_LEFT_BRACE)) {
            skipBracedBlock();
        } else {
            consume(TOKEN_SEMICOLON, "Expected ';' or '{' after satisfy.");
        }
        result = u;
    }
    else if (match(TOKEN_FILTER)) {
        /* `filter <expr>;` — view-body statement.  Parse-and-skip the
         * filter expression until ';'.  Common forms: `filter @Safety;`,
         * `filter @A or @B;`, `filter @SysML::PartUsage;`.            */
        if (dir != DIR_NONE) error("Direction modifier is not valid on filter.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on filter.");
        while (!check(TOKEN_SEMICOLON) && !check(TOKEN_EOF)) advance();
        match(TOKEN_SEMICOLON);
        result = NULL;
    }
    else if (match(TOKEN_USE)) {
        /* `use case [def] Name [: T] [{...}|;]` — the only valid two-
         * token construct that starts with `use`.  We require the next
         * token be the bare identifier `case` (we never made it a
         * proper keyword since it appears nowhere else).               */
        if (dir != DIR_NONE) error("Direction modifier is not valid on use case.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on use case.");
        if (!check(TOKEN_IDENTIFIER) || parser.current.length != 4
            || memcmp(parser.current.start, "case", 4) != 0) {
            errorAtCurrent("Expected 'case' after 'use'.");
            result = NULL;
        } else {
            advance();      /* eat 'case' */
            result = definitionOrUsage(&kUseCase, dir, mods);
        }
    }
    else if (match(TOKEN_EXHIBIT)) {
        /* `exhibit state s [: T] [{ … }]` — state-flavoured usage.
         * The `state` keyword is required to follow.                  */
        if (dir != DIR_NONE) error("Direction modifier is not valid on exhibit.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on exhibit.");
        if (!match(TOKEN_STATE)) {
            errorAtCurrent("Expected 'state' after 'exhibit'.");
            result = NULL;
        } else {
            result = definitionOrUsage(&kState, dir, mods);
            /* If the user wrote `parallel` between the name and the body,
             * reject it (composite/parallel states aren't supported yet).
             * We can't easily peek for it here because dispatch already
             * consumed the body — the parallel keyword would have been
             * a parse error inside usage().  We rely on that.          */
        }
    }
    else if (match(TOKEN_TRANSITION)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on transition.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on transition.");
        result = parseTransitionStmt();
    }
    else if (match(TOKEN_ENTRY)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on entry.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on entry.");
        result = parseLifecycleAction(LIFECYCLE_ENTRY);
    }
    else if (match(TOKEN_EXIT)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on exit.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on exit.");
        result = parseLifecycleAction(LIFECYCLE_EXIT);
    }
    else if (match(TOKEN_DO)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on do.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on do.");
        result = parseLifecycleAction(LIFECYCLE_DO);
    }
    else if (match(TOKEN_PERFORM)) {
        /* `perform [action] <qname> [: Type] [redefines …];`
         *
         * This declares an action usage owned by the enclosing element.
         * The `action` keyword is optional — the modeler can write either
         * `perform action providePower;` or `perform providePower;`.
         * In either case we synthesize an action usage where the qname
         * after `perform` is treated as a specialization (`:>`) so the
         * resolver can chase it back to the original action.            */
        if (dir != DIR_NONE) error("Direction modifier is not valid on perform.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on perform.");

        bool hadActionKw = match(TOKEN_ACTION);
        (void)hadActionKw;     /* tolerated either way */

        int line = parser.previous.line;
        Node* u = astMakeNode(NODE_USAGE, line);
        u->as.usage.defKind   = DEF_ACTION;
        u->as.usage.isPerform = true;

        /* The first qname is either a name or a reference-into-elsewhere.
         * Common SysML pattern: `perform providePower redefines providePower;`
         * — a part inherits an action from its definition and references
         * the same action it would have inherited.  Treat the first qname
         * as the usage's name when it's a single segment; fall through
         * to the regular usage parser otherwise.                         */
        if (check(TOKEN_IDENTIFIER)) {
            /* `perform <X>` has three possible shapes:
             *
             *   1. declaration:    perform action foo;
             *                      perform foo : T;
             *                      perform foo redefines bar;
             *      → candidate IS the usage's name.  Recognized by
             *        the explicit `action` keyword OR by a following
             *        clause that only makes sense on a fresh decl.
             *
             *   2. bare reference: perform foo;
             *      → candidate is a 1-segment qname stored in
             *        specializes; the usage stays anonymous so it
             *        can't shadow the outer `foo` it points at.
             *
             *   3. dotted reference: perform A::B::foo;
             *      → multi-segment qname stored in specializes.
             *
             * Cases 2 and 3 share the same handler — case 2 is just
             * case 3 where the trailing while-loop runs zero times. */
            Token candidate = parser.current;
            advance();
            bool isDottedRef = check(TOKEN_COLON_COLON) || check(TOKEN_DOT);
            bool isDeclaration = !isDottedRef
                && (hadActionKw || check(TOKEN_REDEFINES) || check(TOKEN_COLON));
            if (isDeclaration) {
                u->as.usage.name = candidate;
            } else {
                Node* q = astMakeNode(NODE_QUALIFIED_NAME, line);
                astAppendQualifiedPart(q, candidate);
                while (match(TOKEN_COLON_COLON) || match(TOKEN_DOT)) {
                    consume(TOKEN_IDENTIFIER, "Expected identifier in qualified name.");
                    astAppendQualifiedPart(q, parser.previous);
                }
                astListAppend(&u->as.usage.specializes, q);
            }
        }
        /* Optional `: Type`. */
        if (match(TOKEN_COLON)) {
            appendQualifiedNameList(&u->as.usage.types);
        }
        /* Optional specializes/redefines (any combination of keyword and
         * shortcut forms): `:> X`, `:>> Y`, `specializes X`, `redefines Y`,
         * `subsets Z`.  We just feed everything into parseFeatureRelationships
         * and merge into the usage we already built.                      */
        FeatureRels rels = parseFeatureRelationships();
        for (int i = 0; i < rels.types.count; i++) {
            astListAppend(&u->as.usage.types, rels.types.items[i]);
        }
        for (int i = 0; i < rels.specializes.count; i++) {
            astListAppend(&u->as.usage.specializes, rels.specializes.items[i]);
        }
        for (int i = 0; i < rels.redefines.count; i++) {
            astListAppend(&u->as.usage.redefines, rels.redefines.items[i]);
        }
        if (rels.multiplicity) u->as.usage.multiplicity = rels.multiplicity;
        /* Optional inline body — `perform action a { ... }`.  We
         * parse it as a normal usage body so nested actions inside
         * the perform are members of the resulting action usage.    */
        if (match(TOKEN_LEFT_BRACE)) {
            while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
                Node* m = declaration();
                if (m) astAppendUsageMember(u, m);
                if (parser.panicMode) synchronize();
            }
            consume(TOKEN_RIGHT_BRACE, "Expected '}' to close perform body.");
        } else {
            consume(TOKEN_SEMICOLON, "Expected ';' after perform statement.");
        }
        result = u;
    }
    else if (match(TOKEN_RETURN)) {
        /* `return [IDENTIFIER] (`:` qname (`:>` qnameList)? )? (`=` expression)? `;`
         *
         *  Forms found in the wild:
         *      return f_a : Real = bsfc * sgg * pwr;
         *      return :> distancePerVolume = 1/f;
         *      return distance :> length;
         *      return part selectedVehicle :> vehicle_b;
         *      return : Real;
         *
         *  We accept all of these and produce a NODE_RETURN with whatever
         *  fields are present.  Callers (resolver/typechecker) ignore
         *  unknown holes for now; meaningful checks come once we have a
         *  typed AST.                                                   */
        if (dir != DIR_NONE) error("Direction modifier is not valid on return.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on return.");

        int line = parser.previous.line;
        Node* r = astMakeNode(NODE_RETURN, line);
        /* Leading `attribute` / `part` keyword is sometimes used to tell
         * SysML which feature kind the return introduces.  We parse the
         * keyword (if any) but don't yet propagate it — return-with-kind
         * is rare and we don't have a slot for it.                      */
        if (check(TOKEN_ATTRIBUTE) || check(TOKEN_PART)
         || check(TOKEN_ITEM)      || check(TOKEN_PORT)
         || check(TOKEN_REF)) {
            advance();      /* swallow the optional kind keyword */
        }
        if (check(TOKEN_IDENTIFIER)) {
            advance();
            r->as.ret.name = parser.previous;
        }
        if (match(TOKEN_COLON)) {
            appendQualifiedNameList(&r->as.ret.types);
        }
        if (match(TOKEN_COLON_GREATER) || match(TOKEN_SPECIALIZES)) {
            appendQualifiedNameList(&r->as.ret.specializes);
        }
        /* The grammar also allows `:>>` here in calc def contexts; tolerate.  */
        if (match(TOKEN_COLON_GREATER_GREATER) || match(TOKEN_REDEFINES)) {
            /* No dedicated slot yet; reuse specializes as a safe approximation
             * — both denote that the return feature inherits from the named one. */
            appendQualifiedNameList(&r->as.ret.specializes);
        }
        if (match(TOKEN_EQUAL)) {
            r->as.ret.defaultValue = expression();
        }
        consume(TOKEN_SEMICOLON, "Expected ';' after return.");
        result = r;
    }
    else if (match(TOKEN_CALC)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on calc.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on calc.");
        result = definitionOrUsage(&kCalc, dir, mods);
    }
    else if (match(TOKEN_SUCCESSION)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on succession.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on succession.");
        result = parseSuccessionStmt(TOKEN_SUCCESSION);
    }
    else if (match(TOKEN_FIRST)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on first.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on first.");
        result = parseSuccessionStmt(TOKEN_FIRST);
    }
    else if (match(TOKEN_THEN)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on then.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on then.");
        result = parseSuccessionStmt(TOKEN_THEN);
    }
    else if (check(TOKEN_ASSERT) || check(TOKEN_ASSUME) || check(TOKEN_REQUIRE)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid before an assertion.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid before an assertion.");
        AssertKind kw;
        if      (check(TOKEN_ASSERT))  kw = ASSERT_ASSERT;
        else if (check(TOKEN_ASSUME))  kw = ASSERT_ASSUME;
        else                           kw = ASSERT_REQUIRE;
        int kwLine = parser.current.line;
        advance();                      /* consume the assertion keyword */
        result = assertedConstraintOrRequirement(kw, kwLine);
    }
    else if (match(TOKEN_SUBJECT)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on subject.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on subject.");
        result = subjectStatement();
    }
    else if (match(TOKEN_CONNECT)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on connect.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on connect.");
        result = connectStatement();
    }
    else if (match(TOKEN_BIND)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on bind.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on bind.");
        result = bindStatement();
    }
    else if (match(TOKEN_ALLOCATE)) {
        if (dir != DIR_NONE) error("Direction modifier is not valid on allocate.");
        if (hasAnyModifier(mods)) error("Feature modifiers are not valid on allocate.");
        result = allocateStatement();
    }
    else if (match(TOKEN_ATTRIBUTE)) {
        /* `in attribute scenario: T;` — calc/action def parameters
         * routinely carry a direction modifier.  Real SysML accepts
         * direction here; the AST has a direction slot already, so we
         * just need to stop rejecting it.                            */
        /* `attribute def Name [:> Parent];`  — full definition shape.
         * Route through definitionOrUsage so the def/usage dispatch
         * works correctly.  For the usage path, use attributeDecl()
         * which has the special handling for redefines/specializes
         * shortcuts.                                                  */
        if (check(TOKEN_DEF)) {
            if (dir != DIR_NONE) {
                error("Direction modifier is not valid on a definition.");
            }
            result = definitionOrUsage(&kAttributeDef, dir, mods);
        } else {
            result = attributeDecl();
            /* Attributes accept all four modifiers (they're usages). */
            if (result && result->kind == NODE_ATTRIBUTE) {
                result->as.attribute.direction           = dir;
                result->as.attribute.isDerived           = mods.isDerived;
                result->as.attribute.isAbstract          = mods.isAbstract;
                result->as.attribute.isConstant          = mods.isConstant;
                result->as.attribute.isReference         = mods.isReference;
                result->as.attribute.isReferenceExplicit = mods.isReference;
            }
        }
    } else {
        /* Bare-form usage (§8.3.6.3 ReferenceUsage): no kind keyword,
         * just an optional ref/direction prefix and a name.
         *
         * Three flavours reach here:
         *   - `ref name : T = expr;`        — explicit ReferenceUsage
         *   - `in name : T;` / `out …;`     — directional parameter
         *     used in constraint, calc, action def bodies
         *   - `name :> X = expr;`           — anonymous feature with no
         *     kind keyword at all (rare; appears in derived-quantity
         *     packages like `distancePerVolume :> scalarQuantities = …;`)
         *
         * For (1) and (2) the modifier or direction makes the intent
         * unambiguous — anything starting with `ref name` or `in name`
         * is a bare ref/parameter usage.
         *
         * For (3) we have to commit to the IDENTIFIER and check the
         * next token; if it's a feature-relationship token, we're
         * good — usage() will then re-lex from `previous` (which is
         * the identifier we just consumed).  Since usage() expects
         * the kind keyword as `previous`, we synthesize the bare
         * form by NOT consuming the identifier here and letting
         * usage() do it.                                              */
        bool bareFormByModifier = check(TOKEN_IDENTIFIER)
                              && (mods.isReference || dir != DIR_NONE);
        if (bareFormByModifier) {
            Node* u = usage(&kReference, dir);
            if (u && u->kind == NODE_USAGE) {
                u->as.usage.isDerived           = mods.isDerived;
                u->as.usage.isAbstract          = mods.isAbstract;
                u->as.usage.isConstant          = mods.isConstant;
                u->as.usage.isReference         = mods.isReference;
                u->as.usage.isReferenceExplicit = mods.isReference;
            }
            astSetVisibility(u, vis);
            return u;
        }

        /* Bare-form (3): `name :> X = expr;` with no kind keyword and
         * no modifier/direction.  Commit to the IDENTIFIER and check
         * what follows; if it's a feature-relationship token, build
         * the usage manually.  Otherwise it's a real error.           */
        if (check(TOKEN_IDENTIFIER)) {
            Token saveCurrent = parser.current;
            advance();                  /* commit: eat the identifier */
            bool looksFeature =
                   check(TOKEN_COLON)
                || check(TOKEN_COLON_GREATER)
                || check(TOKEN_COLON_GREATER_GREATER)
                || check(TOKEN_REDEFINES) || check(TOKEN_SPECIALIZES)
                || check(TOKEN_SUBSETS)
                || check(TOKEN_LEFT_BRACKET)
                || check(TOKEN_EQUAL)
                || check(TOKEN_DEFAULT);
            if (looksFeature) {
                int line = saveCurrent.line;
                Node* u = astMakeNode(NODE_USAGE, line);
                u->as.usage.defKind = DEF_REFERENCE;
                u->as.usage.name    = saveCurrent;
                FeatureRels rels = parseFeatureRelationships();
                u->as.usage.types        = rels.types;
                u->as.usage.specializes  = rels.specializes;
                u->as.usage.redefines    = rels.redefines;
                u->as.usage.multiplicity = rels.multiplicity;
                if (match(TOKEN_EQUAL) || match(TOKEN_DEFAULT)) {
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
                    consume(TOKEN_SEMICOLON, "Expected ';' after bare feature.");
                }
                astSetVisibility(u, vis);
                return u;
            }
            /* Not a feature — error reporting needs to point at the
             * still-unconsumed current token, but we already moved
             * past the identifier.  Just emit the generic message.   */
            errorAtCurrent("Expected declaration.");
            return NULL;
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
