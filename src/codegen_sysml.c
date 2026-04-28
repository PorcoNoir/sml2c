/* sysmlc — codegen_sysml.c
 *
 * Canonical SysML v2 emitter.  See codegen_sysml.h for design notes.
 *
 * Implementation shape:
 *
 *   - One emit function per NodeKind, plus per-kind helpers (relationship
 *     clauses, multiplicities, expressions, etc.).  This mirrors the
 *     JSON emitter's shape so they evolve together.
 *
 *   - Indentation is tracked in a small State struct holding the FILE*
 *     and current depth.  Every emitNode call decides its own newline
 *     and indent — we don't try to be clever about line packing.
 *
 *   - Expressions get conservative parenthesisation: every BINARY is
 *     wrapped in parens.  This produces output that's slightly noisy
 *     for nested expressions but guarantees the parser sees the same
 *     tree on round-trip without operator-precedence reasoning.
 *
 *   - Tokens are emitted lexeme-faithfully via fwrite of (start,length).
 *     For literals we use the same trick the parser uses — we have a
 *     parsed value but the lexeme is what the user wrote, and that's
 *     what the round-trip expects to see.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "codegen_sysml.h"

typedef struct {
    FILE* out;
    int   depth;
} S;

/* ---- low-level emitters ---------------------------------------- */

static void emitIndent(S* s) {
    for (int i = 0; i < s->depth; i++) fputs("    ", s->out);
}

/* Emit a token's lexeme verbatim — no quoting heuristic.  Used for
 * doc bodies and other free-form text payloads.  Names should go
 * through emitToken() which adds quotes when the lexeme isn't a
 * bare identifier.                                                  */
static void emitRawToken(S* s, Token t) {
    if (t.length > 0) fwrite(t.start, 1, (size_t)t.length, s->out);
}

/* Returns true if `t` is the (lexeme of a) plain identifier — i.e. a
 * letter or underscore followed by alphanumerics or underscores.  Any
 * other shape (hyphens, digits-only, embedded spaces) means the
 * original source quoted it with apostrophes, and we must do the
 * same on output to round-trip.                                       */
static bool isBareIdentifier(Token t) {
    if (t.length == 0) return false;
    char c = t.start[0];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) {
        return false;
    }
    for (int i = 1; i < t.length; i++) {
        char d = t.start[i];
        if (!((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z')
              || (d >= '0' && d <= '9') || d == '_')) {
            return false;
        }
    }
    return true;
}

static void emitToken(S* s, Token t) {
    if (t.length == 0) return;
    if (isBareIdentifier(t)) {
        fwrite(t.start, 1, (size_t)t.length, s->out);
    } else {
        fputc('\'', s->out);
        fwrite(t.start, 1, (size_t)t.length, s->out);
        fputc('\'', s->out);
    }
}

/* Forward declarations for the mutual recursion. */
static void emitNode(S* s, const Node* n, bool insideEnumDef);
static void emitExpression(S* s, const Node* expr);

/* Common helpers used by several kinds. */

static void emitVisibility(S* s, Visibility v) {
    switch (v) {
    case VIS_PUBLIC:    fputs("public ", s->out);    break;
    case VIS_PRIVATE:   fputs("private ", s->out);   break;
    case VIS_PROTECTED: fputs("protected ", s->out); break;
    case VIS_DEFAULT:   break;
    }
}

static void emitDirection(S* s, Direction d) {
    switch (d) {
    case DIR_IN:    fputs("in ",    s->out); break;
    case DIR_OUT:   fputs("out ",   s->out); break;
    case DIR_INOUT: fputs("inout ", s->out); break;
    case DIR_NONE:  break;
    }
}

static void emitAssertKind(S* s, AssertKind a) {
    switch (a) {
    case ASSERT_ASSERT:  fputs("assert ",  s->out); break;
    case ASSERT_ASSUME:  fputs("assume ",  s->out); break;
    case ASSERT_REQUIRE: fputs("require ", s->out); break;
    case ASSERT_NONE:    break;
    }
}

/* Emit the four feature modifiers in their canonical order. */
static void emitModifiers(S* s, bool d, bool a, bool c, bool r) {
    if (d) fputs("derived ",  s->out);
    if (a) fputs("abstract ", s->out);
    if (c) fputs("constant ", s->out);
    if (r) fputs("ref ",      s->out);
}

/* Emit a qualified name with `::` separators.  Honours the
 * conjugation flag.                                              */
static void emitQualifiedName(S* s, const Node* q) {
    if (!q) return;
    if (q->as.qualifiedName.isConjugated) fputc('~', s->out);
    for (int i = 0; i < q->as.qualifiedName.partCount; i++) {
        if (i > 0) fputs("::", s->out);
        emitToken(s, q->as.qualifiedName.parts[i]);
    }
}

/* Emit a comma-separated list of qualified names with a leading
 * keyword/operator prefix.  Does nothing if the list is empty.    */
static void emitNameList(S* s, const char* prefix, const NodeList* list) {
    if (list->count == 0) return;
    fputs(prefix, s->out);
    for (int i = 0; i < list->count; i++) {
        if (i > 0) fputs(", ", s->out);
        emitQualifiedName(s, list->items[i]);
    }
}

/* Emit a multiplicity in `[lo]`, `[lo..hi]`, `[*]`, `[lo..*]` form. */
static void emitMultiplicity(S* s, const Node* m) {
    if (!m) return;
    fputc(' ', s->out);
    fputc('[', s->out);
    if (m->as.multiplicity.lowerWildcard) fputc('*', s->out);
    else                                   fprintf(s->out, "%ld", m->as.multiplicity.lower);
    if (m->as.multiplicity.isRange) {
        fputs("..", s->out);
        if (m->as.multiplicity.upperWildcard) fputc('*', s->out);
        else                                  fprintf(s->out, "%ld", m->as.multiplicity.upper);
    }
    fputc(']', s->out);
}

/* Emit an endpoint clause (the `connect a to b` or `from a to b`
 * tail of a connection/flow usage).  Empty list emits nothing.    */
/* Emit an endpoint clause (the `connect a to b` or `from a to b`
 * tail of a connection/flow/message/interface usage).  Empty list
 * emits nothing.                                                    */
static void emitEnds(S* s, DefKind kind, const NodeList* ends) {
    if (ends->count == 0) return;
    if (kind == DEF_FLOW || kind == DEF_MESSAGE) fputs(" from ",    s->out);
    else                                          fputs(" connect ", s->out);   /* connection or interface */
    /* Parser produces exactly two ends; defensive code handles other
     * counts as a comma-list (won't actually trigger).             */
    if (ends->count >= 1) emitQualifiedName(s, ends->items[0]);
    if (ends->count >= 2) {
        fputs(" to ", s->out);
        emitQualifiedName(s, ends->items[1]);
    }
    for (int i = 2; i < ends->count; i++) {
        fputs(", ", s->out);
        emitQualifiedName(s, ends->items[i]);
    }
}

/* ---- expressions ---------------------------------------------- */

static const char* opSymbol(TokenType t) {
    switch (t) {
    case TOKEN_PLUS:           return "+";
    case TOKEN_MINUS:          return "-";
    case TOKEN_STAR:           return "*";
    case TOKEN_STAR_STAR:      return "**";
    case TOKEN_SLASH:          return "/";
    case TOKEN_BANG:           return "!";
    case TOKEN_EQUAL_EQUAL:    return "==";
    case TOKEN_BANG_EQUAL:     return "!=";
    case TOKEN_LESS:           return "<";
    case TOKEN_LESS_EQUAL:     return "<=";
    case TOKEN_GREATER:        return ">";
    case TOKEN_GREATER_EQUAL:  return ">=";
    case TOKEN_AND:            return "and";
    case TOKEN_OR:             return "or";
    default:                   return "?";
    }
}

/* Print an expression node — emits parens around every binary so
 * the parser sees the same tree on round-trip without us having to
 * compute precedences here.                                       */
static void emitExpression(S* s, const Node* expr) {
    if (!expr) return;
    switch (expr->kind) {
    case NODE_LITERAL:
        emitRawToken(s, expr->as.literal.token);
        break;
    case NODE_QUALIFIED_NAME:
        emitQualifiedName(s, expr);
        break;
    case NODE_BINARY:
        fputc('(', s->out);
        emitExpression(s, expr->as.binary.left);
        fprintf(s->out, " %s ", opSymbol(expr->as.binary.op.type));
        emitExpression(s, expr->as.binary.right);
        fputc(')', s->out);
        break;
    case NODE_UNARY:
        fprintf(s->out, "%s", opSymbol(expr->as.unary.op.type));
        emitExpression(s, expr->as.unary.operand);
        break;
    case NODE_CALL:
        emitExpression(s, expr->as.call.callee);
        fputc('(', s->out);
        for (int i = 0; i < expr->as.call.args.count; i++) {
            if (i > 0) fputs(", ", s->out);
            emitExpression(s, expr->as.call.args.items[i]);
        }
        fputc(')', s->out);
        break;
    case NODE_MEMBER_ACCESS:
        emitExpression(s, expr->as.memberAccess.target);
        fputc('.', s->out);
        emitToken(s, expr->as.memberAccess.member);
        break;
    default:
        /* Other node kinds shouldn't appear inside expressions. */
        break;
    }
}

/* ---- per-kind emitters --------------------------------------- */

/* Map a DefKind to the source keyword used to introduce it.  Used
 * by both definition and usage emitters.                          */
static const char* defKeyword(DefKind k) {
    switch (k) {
    case DEF_PART:       return "part";
    case DEF_PORT:       return "port";
    case DEF_INTERFACE:  return "interface";
    case DEF_ITEM:       return "item";
    case DEF_CONNECTION: return "connection";
    case DEF_FLOW:       return "flow";
    case DEF_END:        return "end";
    case DEF_ENUM:       return "enum";
    case DEF_DATATYPE:   return "datatype";    /* no source form, sentinel */
    case DEF_REFERENCE:  return "ref";          /* bare-ref usage */
    case DEF_CONSTRAINT: return "constraint";
    case DEF_REQUIREMENT:return "requirement";
    case DEF_SUBJECT:    return "subject";
    case DEF_ACTION:     return "action";
    case DEF_STATE:      return "state";
    case DEF_CALC:       return "calc";
    case DEF_ATTRIBUTE_DEF: return "attribute";
    case DEF_OCCURRENCE: return "occurrence";
    case DEF_EVENT:      return "event occurrence";
    case DEF_INDIVIDUAL: return "individual";
    case DEF_SNAPSHOT:   return "snapshot";
    case DEF_TIMESLICE:  return "timeslice";
    case DEF_ALLOCATION: return "allocation";
    case DEF_VIEW:       return "view";
    case DEF_VIEWPOINT:  return "viewpoint";
    case DEF_RENDERING:  return "rendering";
    case DEF_CONCERN:    return "concern";
    case DEF_VARIANT:    return "variant";
    case DEF_VARIATION:  return "variation";
    case DEF_ACTOR:      return "actor";
    case DEF_USE_CASE:   return "use case";
    case DEF_INCLUDE:    return "include";
    case DEF_MESSAGE:    return "message";
    }
    return "?";
}

static void emitDefinition(S* s, const Node* n) {
    emitIndent(s);
    emitVisibility(s, n->as.scope.visibility);
    if (n->as.scope.isAbstract) fputs("abstract ", s->out);
    fprintf(s->out, "%s def ", defKeyword(n->as.scope.defKind));
    emitToken(s, n->as.scope.name);
    emitNameList(s, " :> ",  &n->as.scope.specializes);
    emitNameList(s, " :>> ", &n->as.scope.redefines);

    /* A constraint def body may have only a trailing expression with no
     * other members, but it always needs the body printed inside braces. */
    bool hasBody = (n->as.scope.body != NULL);
    if (n->as.scope.memberCount == 0 && !hasBody) {
        fputs(" { }\n", s->out);
        return;
    }
    fputs(" {\n", s->out);
    s->depth++;
    bool childIsEnum = (n->as.scope.defKind == DEF_ENUM);
    for (int i = 0; i < n->as.scope.memberCount; i++) {
        emitNode(s, n->as.scope.members[i], childIsEnum);
    }
    if (hasBody) {
        emitIndent(s);
        emitExpression(s, n->as.scope.body);
        fputc('\n', s->out);
    }
    s->depth--;
    emitIndent(s);
    fputs("}\n", s->out);
}

/* Emit a usage value initializer for an enum value: just the bare
 * identifier with optional `= initializer`.  Used inside enum-def
 * bodies where the parser expects `name (= expr)? ;` rather than
 * the full usage syntax.                                          */
static bool isEnumValue(const Node* n) {
    /* Enum values are stamped as Attributes whose declared type
     * resolves to an enum def.  This is a heuristic but matches
     * the parser's construction.                                  */
    if (!n || n->kind != NODE_ATTRIBUTE) return false;
    if (n->as.attribute.types.count != 1) return false;
    const Node* tref = n->as.attribute.types.items[0];
    if (!tref || tref->kind != NODE_QUALIFIED_NAME) return false;
    const Node* td = tref->as.qualifiedName.resolved;
    return td && td->kind == NODE_DEFINITION
              && td->as.scope.defKind == DEF_ENUM;
}

static void emitEnumValue(S* s, const Node* n) {
    emitIndent(s);
    emitToken(s, n->as.attribute.name);
    if (n->as.attribute.defaultValue) {
        fputs(" = ", s->out);
        emitExpression(s, n->as.attribute.defaultValue);
    }
    fputs(";\n", s->out);
}

static void emitUsage(S* s, const Node* n) {
    /* `bind X = Y;` is a connection-usage variant with two ends but
     * a different surface form.  Short-circuit before the general
     * usage emission path so we don't print `connection connect`.   */
    if (n->as.usage.isBind) {
        emitIndent(s);
        fputs("bind ", s->out);
        if (n->as.usage.ends.count >= 1) {
            emitQualifiedName(s, n->as.usage.ends.items[0]);
        }
        fputs(" = ", s->out);
        if (n->as.usage.ends.count >= 2) {
            emitQualifiedName(s, n->as.usage.ends.items[1]);
        }
        fputs(";\n", s->out);
        return;
    }

    /* `allocate X to Y;` — same shape as bind, different keyword.  We
     * don't preserve the optional body since it's parse-and-skipped.  */
    if (n->as.usage.isAllocate) {
        emitIndent(s);
        fputs("allocate ", s->out);
        if (n->as.usage.ends.count >= 1) {
            emitQualifiedName(s, n->as.usage.ends.items[0]);
        }
        fputs(" to ", s->out);
        if (n->as.usage.ends.count >= 2) {
            emitQualifiedName(s, n->as.usage.ends.items[1]);
        }
        fputs(";\n", s->out);
        return;
    }

    emitIndent(s);
    emitVisibility(s, n->as.usage.visibility);
    emitAssertKind(s, n->as.usage.assertKind);
    emitDirection (s, n->as.usage.direction);
    /* Emit `ref` only when the user wrote it explicitly.  The
     * referentialchecker may set `isReference=true` on usages where
     * SysML semantics force referentiality; that flag is for downstream
     * passes, not for source emission.  Bare-ref usages (DEF_REFERENCE
     * with isReferenceExplicit=true) print `ref name`; direction-only
     * parameters (DEF_REFERENCE with isReferenceExplicit=false) print
     * just `in name`.                                                  */
    bool emitRef = n->as.usage.isReferenceExplicit;
    emitModifiers (s, n->as.usage.isDerived,
                       n->as.usage.isAbstract,
                       n->as.usage.isConstant,
                       emitRef);

    /* Bare-ref usages (DEF_REFERENCE) print as `ref name : T;` —
     * the `ref` keyword is already emitted by emitModifiers above
     * (isReference is always true for these).  Don't add an extra
     * keyword.  `perform` usages emit `perform action <name>` or
     * `perform <name>` depending on whether they have a name; the
     * round-trip stays a fixed point either way.                   */
    if (n->as.usage.isPerform) {
        fputs("perform ", s->out);
    }
    if (n->as.usage.defKind != DEF_REFERENCE) {
        fputs(defKeyword(n->as.usage.defKind), s->out);
        if (n->as.usage.name.length > 0) fputc(' ', s->out);
    }
    if (n->as.usage.name.length > 0) {
        emitToken(s, n->as.usage.name);
    }

    emitNameList   (s, " : ",   &n->as.usage.types);
    emitNameList   (s, " :> ",  &n->as.usage.specializes);
    emitNameList   (s, " :>> ", &n->as.usage.redefines);
    emitMultiplicity(s, n->as.usage.multiplicity);
    emitEnds       (s, n->as.usage.defKind, &n->as.usage.ends);

    if (n->as.usage.defaultValue) {
        fputs(" = ", s->out);
        emitExpression(s, n->as.usage.defaultValue);
    }

    /* Inline constraint body: `assert constraint { x > 0 }`.  Replaces
     * the `;` terminator with a brace-delimited expression form.       */
    if (n->as.usage.body) {
        fputs(" { ", s->out);
        emitExpression(s, n->as.usage.body);
        fputs(" }\n", s->out);
        return;
    }

    if (n->as.usage.memberCount == 0) {
        fputs(";\n", s->out);
        return;
    }
    fputs(" {\n", s->out);
    s->depth++;
    for (int i = 0; i < n->as.usage.memberCount; i++) {
        emitNode(s, n->as.usage.members[i], false);
    }
    s->depth--;
    emitIndent(s);
    fputs("}\n", s->out);
}

static void emitAttribute(S* s, const Node* n) {
    emitIndent(s);
    emitVisibility(s, n->as.attribute.visibility);
    /* Same rule as emitUsage: emit `ref` only when source had it
     * explicitly.  The referentialchecker forces isReference=true on
     * every attribute usage (rule #2), but that's a downstream marker
     * — the user didn't write `ref attribute` in source.              */
    emitModifiers (s, n->as.attribute.isDerived,
                       n->as.attribute.isAbstract,
                       n->as.attribute.isConstant,
                       n->as.attribute.isReferenceExplicit);
    fputs("attribute ", s->out);
    /* An attribute with no source-given name (`attribute redefines mass = 75 [kg];`,
     * `attribute :>> fuelMass;`) emits the keyword alone with no
     * trailing identifier.                                            */
    if (n->as.attribute.name.length > 0) {
        emitToken(s, n->as.attribute.name);
    }

    emitNameList   (s, " : ",   &n->as.attribute.types);
    emitNameList   (s, " :> ",  &n->as.attribute.specializes);
    emitNameList   (s, " :>> ", &n->as.attribute.redefines);
    emitMultiplicity(s, n->as.attribute.multiplicity);

    if (n->as.attribute.defaultValue) {
        fputs(" = ", s->out);
        emitExpression(s, n->as.attribute.defaultValue);
    }
    fputs(";\n", s->out);
}

static void emitImport(S* s, const Node* n) {
    emitIndent(s);
    emitVisibility(s, n->as.import.visibility);
    fputs("import ", s->out);
    emitQualifiedName(s, n->as.import.target);
    if (n->as.import.wildcard) fputs("::*", s->out);
    fputs(";\n", s->out);
}

static void emitDoc(S* s, const Node* n) {
    emitIndent(s);
    fputs("/**", s->out);
    emitRawToken(s, n->as.doc.body);
    fputs("*/\n", s->out);
}

static void emitAlias(S* s, const Node* n) {
    emitIndent(s);
    emitVisibility(s, n->as.alias.visibility);
    fputs("alias ", s->out);
    emitToken(s, n->as.alias.name);
    fputs(" for ", s->out);
    emitQualifiedName(s, n->as.alias.target);
    fputs(";\n", s->out);
}

static void emitComment(S* s, const Node* n) {
    emitIndent(s);
    fputs("comment", s->out);
    if (n->as.comment.name.length > 0) {
        fputc(' ', s->out);
        emitToken(s, n->as.comment.name);
    }
    if (n->as.comment.about.count > 0) {
        fputs(" about ", s->out);
        for (int i = 0; i < n->as.comment.about.count; i++) {
            if (i > 0) fputs(", ", s->out);
            emitQualifiedName(s, n->as.comment.about.items[i]);
        }
    }
    /* Body delimiters preserved so the body can be re-scanned.
     * Emit verbatim so round-trip is fixed-point — the body's
     * own leading/trailing whitespace is part of the AST. */
    fputs(" /*", s->out);
    emitRawToken(s, n->as.comment.body);
    fputs("*/\n", s->out);
}

static void emitDependency(S* s, const Node* n) {
    emitIndent(s);
    emitVisibility(s, n->as.dependency.visibility);
    fputs("dependency", s->out);
    if (n->as.dependency.name.length > 0) {
        fputc(' ', s->out);
        emitToken(s, n->as.dependency.name);
    }
    fputs(" from ", s->out);
    for (int i = 0; i < n->as.dependency.sources.count; i++) {
        if (i > 0) fputs(", ", s->out);
        emitQualifiedName(s, n->as.dependency.sources.items[i]);
    }
    fputs(" to ", s->out);
    for (int i = 0; i < n->as.dependency.targets.count; i++) {
        if (i > 0) fputs(", ", s->out);
        emitQualifiedName(s, n->as.dependency.targets.items[i]);
    }
    fputs(";\n", s->out);
}

/* Print one succession target — either a qname reference or an
 * inline action declaration `action [name] [: T] [{ ... }]`.        */
static void emitSuccessionTarget(S* s, const Node* t) {
    if (!t) return;
    if (t->kind == NODE_QUALIFIED_NAME) {
        emitQualifiedName(s, t);
        return;
    }
    /* Inline action declaration. */
    fputs("action", s->out);
    if (t->as.usage.name.length > 0) {
        fputc(' ', s->out);
        emitToken(s, t->as.usage.name);
    }
    if (t->as.usage.types.count > 0) {
        fputs(" : ", s->out);
        for (int i = 0; i < t->as.usage.types.count; i++) {
            if (i > 0) fputs(", ", s->out);
            emitQualifiedName(s, t->as.usage.types.items[i]);
        }
    }
    if (t->as.usage.memberCount > 0) {
        fputs(" {\n", s->out);
        s->depth++;
        for (int i = 0; i < t->as.usage.memberCount; i++) {
            emitNode(s, t->as.usage.members[i], false);
        }
        s->depth--;
        emitIndent(s);
        fputs("}", s->out);
    }
}

static void emitSuccession(S* s, const Node* n) {
    emitIndent(s);
    emitVisibility(s, n->as.succession.visibility);
    /* Three surface forms:
     *   `first F;`                    — first=F, targets=[]
     *   `then T (then T)*;`           — first=NULL, targets=[T,…], no name
     *   `succession [name] [first F] then T (then T)*;` — everything else
     */
    bool hasName    = (n->as.succession.name.length > 0);
    bool hasFirst   = (n->as.succession.first != NULL);
    bool hasTargets = (n->as.succession.targets.count > 0);

    if (hasFirst && !hasTargets && !hasName) {
        fputs("first ", s->out);
        emitQualifiedName(s, n->as.succession.first);
        fputs(";\n", s->out);
        return;
    }
    if (!hasFirst && hasTargets && !hasName) {
        for (int i = 0; i < n->as.succession.targets.count; i++) {
            if (i > 0) fputc(' ', s->out);
            fputs("then ", s->out);
            emitSuccessionTarget(s, n->as.succession.targets.items[i]);
        }
        fputs(";\n", s->out);
        return;
    }
    /* Full form. */
    fputs("succession", s->out);
    if (hasName) {
        fputc(' ', s->out);
        emitToken(s, n->as.succession.name);
    }
    if (hasFirst) {
        fputs(" first ", s->out);
        emitQualifiedName(s, n->as.succession.first);
    }
    for (int i = 0; i < n->as.succession.targets.count; i++) {
        fputs(" then ", s->out);
        emitSuccessionTarget(s, n->as.succession.targets.items[i]);
    }
    fputs(";\n", s->out);
}

static void emitTransition(S* s, const Node* n) {
    emitIndent(s);
    emitVisibility(s, n->as.transition.visibility);
    fputs("transition", s->out);
    if (n->as.transition.name.length > 0) {
        fputc(' ', s->out);
        emitToken(s, n->as.transition.name);
    }
    if (n->as.transition.first) {
        fputs(" first ", s->out);
        emitQualifiedName(s, n->as.transition.first);
    }
    if (n->as.transition.accept) {
        fputs(" accept ", s->out);
        emitQualifiedName(s, n->as.transition.accept);
    }
    if (n->as.transition.guard) {
        fputs(" if ", s->out);
        emitExpression(s, n->as.transition.guard);
    }
    if (n->as.transition.effect) {
        fputs(" do ", s->out);
        emitQualifiedName(s, n->as.transition.effect);
    }
    if (n->as.transition.target) {
        fputs(" then ", s->out);
        emitQualifiedName(s, n->as.transition.target);
    }
    fputs(";\n", s->out);
}

static void emitLifecycleAction(S* s, const Node* n) {
    emitIndent(s);
    switch (n->as.lifecycleAction.kind) {
    case LIFECYCLE_ENTRY: fputs("entry ", s->out); break;
    case LIFECYCLE_DO:    fputs("do ",    s->out); break;
    case LIFECYCLE_EXIT:  fputs("exit ",  s->out); break;
    }
    if (n->as.lifecycleAction.action) {
        emitQualifiedName(s, n->as.lifecycleAction.action);
    }
    fputs(";\n", s->out);
}

static void emitPackage(S* s, const Node* n) {
    emitIndent(s);
    emitVisibility(s, n->as.scope.visibility);
    fputs("package ", s->out);
    emitToken(s, n->as.scope.name);

    if (n->as.scope.memberCount == 0) {
        fputs(" { }\n", s->out);
        return;
    }
    fputs(" {\n", s->out);
    s->depth++;
    for (int i = 0; i < n->as.scope.memberCount; i++) {
        emitNode(s, n->as.scope.members[i], false);
    }
    s->depth--;
    emitIndent(s);
    fputs("}\n", s->out);
}

static void emitProgram(S* s, const Node* n) {
    /* Program is the synthetic root container; we don't print it
     * as a node.  Just walk its members. */
    for (int i = 0; i < n->as.scope.memberCount; i++) {
        emitNode(s, n->as.scope.members[i], false);
    }
}

/* Statement-level `return` emitter.  The forms we accept on parse:
 *   return name : T = expr;
 *   return :> S = expr;
 *   return name :> S;
 *   return : T;
 * are all rendered with whatever fields are present.                */
static void emitReturn(S* s, const Node* n) {
    emitIndent(s);
    fputs("return", s->out);
    if (n->as.ret.name.length > 0) {
        fputc(' ', s->out);
        emitToken(s, n->as.ret.name);
    }
    emitNameList(s, " : ",  &n->as.ret.types);
    emitNameList(s, " :> ", &n->as.ret.specializes);
    if (n->as.ret.defaultValue) {
        fputs(" = ", s->out);
        emitExpression(s, n->as.ret.defaultValue);
    }
    fputs(";\n", s->out);
}

/* Dispatcher. */
static void emitNode(S* s, const Node* n, bool insideEnumDef) {
    if (!n) return;

    /* Inside an enum def's body, attributes are enum values and use
     * a slimmer syntax (no `attribute` keyword, no type clause —
     * the type is implicit from the enclosing enum).  We rely on
     * the structural context (insideEnumDef) for this; the AST
     * shape alone isn't a reliable signal because user-written
     * attributes can also be enum-typed.                            */
    if (insideEnumDef && isEnumValue(n)) { emitEnumValue(s, n); return; }

    switch (n->kind) {
    case NODE_PROGRAM:        emitProgram   (s, n); break;
    case NODE_PACKAGE:        emitPackage   (s, n); break;
    case NODE_DEFINITION:     emitDefinition(s, n); break;
    case NODE_USAGE:          emitUsage     (s, n); break;
    case NODE_ATTRIBUTE:      emitAttribute (s, n); break;
    case NODE_IMPORT:         emitImport    (s, n); break;
    case NODE_DOC:            emitDoc       (s, n); break;
    case NODE_ALIAS:          emitAlias     (s, n); break;
    case NODE_COMMENT:        emitComment   (s, n); break;
    case NODE_DEPENDENCY:     emitDependency(s, n); break;
    case NODE_SUCCESSION:     emitSuccession(s, n); break;
    case NODE_TRANSITION:     emitTransition(s, n); break;
    case NODE_LIFECYCLE_ACTION: emitLifecycleAction(s, n); break;
    case NODE_RETURN:         emitReturn    (s, n); break;

    /* These shouldn't appear at statement positions; they are
     * embedded inside other emitters via emitExpression /
     * emitQualifiedName / emitMultiplicity.                       */
    case NODE_QUALIFIED_NAME:
    case NODE_MULTIPLICITY:
    case NODE_LITERAL:
    case NODE_BINARY:
    case NODE_UNARY:
    case NODE_CALL:
    case NODE_MEMBER_ACCESS:
        break;
    }
}

/* ---- public entry --------------------------------------------- */

void emitSysml(FILE* out, const Node* program) {
    S s = { .out = out, .depth = 0 };
    emitNode(&s, program, false);
}
