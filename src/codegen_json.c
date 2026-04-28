/* sysmlc — codegen_json.c
 *
 * Pretty-printed JSON dump of the AST.  See codegen_json.h.
 *
 * Design notes:
 *   - One emit function per NodeKind.  Each writes a JSON object
 *     whose first field is "kind" and whose remaining fields depend
 *     on the variant.
 *   - Field separation is handled by a tiny "first" flag — toggled
 *     by writeFieldSep before each field after the first.  Cleaner
 *     than tracking commas at every emit site.
 *   - Multi-character escapes are explicit; bytes below 0x20 are
 *     emitted as \uXXXX so the output stays valid JSON regardless
 *     of source contents.
 *   - Resolutions are emitted as plain name strings, not full paths.
 *     Consumers that need the path can correlate by walking the
 *     emitted member tree.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "codegen_json.h"

typedef struct {
    FILE* out;
    int   indent;
} J;

/* ---- low-level emitters ---------------------------------------- */

static void emitIndent(J* j) {
    for (int i = 0; i < j->indent; i++) fputs("  ", j->out);
}

static void newline(J* j) { fputc('\n', j->out); emitIndent(j); }

/* Toggle a "first field" boolean and emit a comma + newline if not
 * the first.  Caller writes the field after.                      */
static void sep(J* j, bool* first) {
    if (!*first) { fputc(',', j->out); newline(j); }
    *first = false;
}

static void emitJsonString(J* j, const char* s, int len) {
    fputc('"', j->out);
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  fputs("\\\"", j->out); break;
        case '\\': fputs("\\\\", j->out); break;
        case '\n': fputs("\\n",  j->out); break;
        case '\r': fputs("\\r",  j->out); break;
        case '\t': fputs("\\t",  j->out); break;
        case '\b': fputs("\\b",  j->out); break;
        case '\f': fputs("\\f",  j->out); break;
        default:
            if (c < 0x20) fprintf(j->out, "\\u%04x", c);
            else          fputc((char)c, j->out);
        }
    }
    fputc('"', j->out);
}

static void emitToken(J* j, Token t) {
    emitJsonString(j, t.start, t.length);
}

static void emitKey(J* j, const char* k) {
    fprintf(j->out, "\"%s\": ", k);
}

static void emitFieldStr(J* j, const char* k, const char* v) {
    emitKey(j, k);
    emitJsonString(j, v, (int)strlen(v));
}

static void emitFieldBool(J* j, const char* k, bool v) {
    fprintf(j->out, "\"%s\": %s", k, v ? "true" : "false");
}

static void emitFieldInt(J* j, const char* k, long v) {
    fprintf(j->out, "\"%s\": %ld", k, v);
}

/* ---- enum-to-string maps --------------------------------------- */

static const char* defKindStr(DefKind k) {
    switch (k) {
    case DEF_PART:       return "PartDef";
    case DEF_PORT:       return "PortDef";
    case DEF_INTERFACE:  return "InterfaceDef";
    case DEF_ITEM:       return "ItemDef";
    case DEF_CONNECTION: return "ConnectionDef";
    case DEF_FLOW:       return "FlowDef";
    case DEF_END:        return "End";
    case DEF_DATATYPE:   return "DataTypeDef";
    case DEF_ENUM:       return "EnumDef";
    case DEF_REFERENCE:  return "ReferenceUsage";
    case DEF_CONSTRAINT: return "ConstraintDef";
    case DEF_REQUIREMENT:return "RequirementDef";
    case DEF_SUBJECT:    return "Subject";
    case DEF_ACTION:     return "ActionDef";
    case DEF_STATE:      return "StateDef";
    }
    return "?";
}

static const char* assertKindStr(AssertKind a) {
    switch (a) {
    case ASSERT_NONE:    return "none";
    case ASSERT_ASSERT:  return "assert";
    case ASSERT_ASSUME:  return "assume";
    case ASSERT_REQUIRE: return "require";
    }
    return "?";
}

static const char* visStr(Visibility v) {
    switch (v) {
    case VIS_DEFAULT:   return "default";
    case VIS_PUBLIC:    return "public";
    case VIS_PRIVATE:   return "private";
    case VIS_PROTECTED: return "protected";
    }
    return "?";
}

static const char* dirStr(Direction d) {
    switch (d) {
    case DIR_NONE:  return "none";
    case DIR_IN:    return "in";
    case DIR_OUT:   return "out";
    case DIR_INOUT: return "inout";
    }
    return "?";
}

static const char* litKindStr(LiteralKind k) {
    switch (k) {
    case LIT_INT:    return "Integer";
    case LIT_REAL:   return "Real";
    case LIT_STRING: return "String";
    case LIT_BOOL:   return "Boolean";
    }
    return "?";
}

static const char* opSym(TokenType t) {
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

/* ---- forward-declared emitters --------------------------------- */

static void emitNode(J* j, const Node* n);

/* ---- list helpers ---------------------------------------------- */

static void emitNodeArray(J* j, Node** items, int count) {
    if (count == 0) { fputs("[]", j->out); return; }
    fputc('[', j->out);
    j->indent++;
    for (int i = 0; i < count; i++) {
        newline(j);
        emitNode(j, items[i]);
        if (i + 1 < count) fputc(',', j->out);
    }
    j->indent--;
    newline(j);
    fputc(']', j->out);
}

static void emitNodeList(J* j, const NodeList* list) {
    emitNodeArray(j, list->items, list->count);
}

/* Pull the simple name out of a declaration node, for "resolvedTo"
 * fields.  Returns NULL when there's no meaningful name. */
static Token nodeNameForResolution(const Node* n) {
    Token empty = {0};
    if (!n) return empty;
    switch (n->kind) {
    case NODE_PACKAGE:
    case NODE_DEFINITION: return n->as.scope.name;
    case NODE_USAGE:      return n->as.usage.name;
    case NODE_ATTRIBUTE:  return n->as.attribute.name;
    default:              return empty;
    }
}

/* ---- per-kind emitters ----------------------------------------- */

static void emitMultiplicityNode(J* j, const Node* m) {
    if (!m || m->kind != NODE_MULTIPLICITY) { fputs("null", j->out); return; }
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr (j, "kind", "Multiplicity");
    sep(j, &first);
    if (m->as.multiplicity.lowerWildcard) {
        emitFieldStr(j, "lower", "*");
    } else {
        emitFieldInt(j, "lower", m->as.multiplicity.lower);
    }
    sep(j, &first); emitFieldBool(j, "isRange", m->as.multiplicity.isRange);
    if (m->as.multiplicity.isRange) {
        sep(j, &first);
        if (m->as.multiplicity.upperWildcard) {
            emitFieldStr(j, "upper", "*");
        } else {
            emitFieldInt(j, "upper", m->as.multiplicity.upper);
        }
    }
    j->indent--; newline(j); fputc('}', j->out);
}

static void emitQualifiedName(J* j, const Node* q) {
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr(j, "kind", "QualifiedName");
    sep(j, &first); emitFieldBool(j, "isConjugated", q->as.qualifiedName.isConjugated);
    sep(j, &first); emitKey(j, "parts"); fputc('[', j->out);
    for (int i = 0; i < q->as.qualifiedName.partCount; i++) {
        if (i > 0) fputs(", ", j->out);
        emitToken(j, q->as.qualifiedName.parts[i]);
    }
    fputc(']', j->out);
    /* Emit the resolution target if known, as a plain name.  NULL
     * means the resolver tentatively accepted the reference.       */
    sep(j, &first); emitKey(j, "resolvedTo");
    const Node* r = q->as.qualifiedName.resolved;
    if (!r) {
        fputs("null", j->out);
    } else {
        Token rn = nodeNameForResolution(r);
        if (rn.length == 0) {
            emitJsonString(j, "<anonymous>", 11);
        } else {
            emitToken(j, rn);
        }
    }
    j->indent--; newline(j); fputc('}', j->out);
}

static void emitLiteral(J* j, const Node* n) {
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr(j, "kind", "Literal");
    sep(j, &first); emitFieldStr(j, "litKind", litKindStr(n->as.literal.litKind));
    sep(j, &first); emitKey(j, "value");
    switch (n->as.literal.litKind) {
    case LIT_INT:
        fprintf(j->out, "%lld", n->as.literal.value.iv);
        break;
    case LIT_REAL:
        fprintf(j->out, "%g", n->as.literal.value.rv);
        break;
    case LIT_BOOL:
        fputs(n->as.literal.value.bv ? "true" : "false", j->out);
        break;
    case LIT_STRING:
        /* The token's lexeme includes the surrounding quotes; strip
         * them for the JSON value. */
        if (n->as.literal.token.length >= 2) {
            emitJsonString(j, n->as.literal.token.start + 1,
                           n->as.literal.token.length - 2);
        } else {
            emitJsonString(j, "", 0);
        }
        break;
    }
    j->indent--; newline(j); fputc('}', j->out);
}

static void emitBinary(J* j, const Node* n) {
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr(j, "kind", "Binary");
    sep(j, &first); emitFieldStr(j, "op", opSym(n->as.binary.op.type));
    sep(j, &first); emitKey(j, "left");  emitNode(j, n->as.binary.left);
    sep(j, &first); emitKey(j, "right"); emitNode(j, n->as.binary.right);
    j->indent--; newline(j); fputc('}', j->out);
}

static void emitUnary(J* j, const Node* n) {
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr(j, "kind", "Unary");
    sep(j, &first); emitFieldStr(j, "op", opSym(n->as.unary.op.type));
    sep(j, &first); emitKey(j, "operand"); emitNode(j, n->as.unary.operand);
    j->indent--; newline(j); fputc('}', j->out);
}

static void emitDoc(J* j, const Node* n) {
    /* Trim leading whitespace from the body for cleaner output. */
    const char* s = n->as.doc.body.start;
    int len = n->as.doc.body.length;
    while (len > 0 && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) {
        s++; len--;
    }
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr(j, "kind", "Doc");
    sep(j, &first); emitKey(j, "body"); emitJsonString(j, s, len);
    j->indent--; newline(j); fputc('}', j->out);
}

static void emitImport(J* j, const Node* n) {
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr (j, "kind", "Import");
    sep(j, &first); emitFieldStr (j, "visibility", visStr(n->as.import.visibility));
    sep(j, &first); emitFieldBool(j, "wildcard", n->as.import.wildcard);
    sep(j, &first); emitKey(j, "target"); emitNode(j, n->as.import.target);
    j->indent--; newline(j); fputc('}', j->out);
}

static void emitAttribute(J* j, const Node* n) {
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr(j, "kind", "Attribute");
    sep(j, &first); emitKey(j, "name"); emitToken(j, n->as.attribute.name);
    sep(j, &first); emitFieldStr(j, "visibility", visStr(n->as.attribute.visibility));
    sep(j, &first); emitFieldBool(j, "isDerived",   n->as.attribute.isDerived);
    sep(j, &first); emitFieldBool(j, "isAbstract",  n->as.attribute.isAbstract);
    sep(j, &first); emitFieldBool(j, "isConstant",  n->as.attribute.isConstant);
    sep(j, &first); emitFieldBool(j, "isReference", n->as.attribute.isReference);
    sep(j, &first); emitKey(j, "types");        emitNodeList(j, &n->as.attribute.types);
    sep(j, &first); emitKey(j, "specializes");  emitNodeList(j, &n->as.attribute.specializes);
    sep(j, &first); emitKey(j, "redefines");    emitNodeList(j, &n->as.attribute.redefines);
    sep(j, &first); emitKey(j, "multiplicity"); emitMultiplicityNode(j, n->as.attribute.multiplicity);
    sep(j, &first); emitKey(j, "default");
    if (n->as.attribute.defaultValue) emitNode(j, n->as.attribute.defaultValue);
    else                              fputs("null", j->out);
    j->indent--; newline(j); fputc('}', j->out);
}

static void emitDefinition(J* j, const Node* n) {
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr(j, "kind", "Definition");
    sep(j, &first); emitFieldStr(j, "defKind", defKindStr(n->as.scope.defKind));
    sep(j, &first); emitKey(j, "name"); emitToken(j, n->as.scope.name);
    sep(j, &first); emitFieldStr (j, "visibility", visStr(n->as.scope.visibility));
    sep(j, &first); emitFieldBool(j, "isAbstract", n->as.scope.isAbstract);
    sep(j, &first); emitKey(j, "specializes"); emitNodeList(j, &n->as.scope.specializes);
    sep(j, &first); emitKey(j, "redefines");   emitNodeList(j, &n->as.scope.redefines);
    sep(j, &first); emitKey(j, "body");
    if (n->as.scope.body) emitNode(j, n->as.scope.body);
    else                  fputs("null", j->out);
    sep(j, &first); emitKey(j, "members");
    emitNodeArray(j, n->as.scope.members, n->as.scope.memberCount);
    j->indent--; newline(j); fputc('}', j->out);
}

static void emitUsage(J* j, const Node* n) {
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr(j, "kind", "Usage");
    sep(j, &first); emitFieldStr(j, "defKind", defKindStr(n->as.usage.defKind));
    sep(j, &first); emitKey(j, "name");
    if (n->as.usage.name.length > 0) emitToken(j, n->as.usage.name);
    else                             fputs("null", j->out);
    sep(j, &first); emitFieldStr (j, "visibility", visStr(n->as.usage.visibility));
    sep(j, &first); emitFieldStr (j, "direction",  dirStr(n->as.usage.direction));
    sep(j, &first); emitFieldStr (j, "assertKind", assertKindStr(n->as.usage.assertKind));
    sep(j, &first); emitFieldBool(j, "isDerived",   n->as.usage.isDerived);
    sep(j, &first); emitFieldBool(j, "isAbstract",  n->as.usage.isAbstract);
    sep(j, &first); emitFieldBool(j, "isConstant",  n->as.usage.isConstant);
    sep(j, &first); emitFieldBool(j, "isReference", n->as.usage.isReference);
    sep(j, &first); emitFieldBool(j, "isPerform",   n->as.usage.isPerform);
    sep(j, &first); emitKey(j, "types");        emitNodeList(j, &n->as.usage.types);
    sep(j, &first); emitKey(j, "specializes");  emitNodeList(j, &n->as.usage.specializes);
    sep(j, &first); emitKey(j, "redefines");    emitNodeList(j, &n->as.usage.redefines);
    sep(j, &first); emitKey(j, "multiplicity"); emitMultiplicityNode(j, n->as.usage.multiplicity);
    sep(j, &first); emitKey(j, "ends");         emitNodeList(j, &n->as.usage.ends);
    sep(j, &first); emitKey(j, "default");
    if (n->as.usage.defaultValue) emitNode(j, n->as.usage.defaultValue);
    else                          fputs("null", j->out);
    sep(j, &first); emitKey(j, "body");
    if (n->as.usage.body) emitNode(j, n->as.usage.body);
    else                  fputs("null", j->out);
    sep(j, &first); emitKey(j, "members");
    emitNodeArray(j, n->as.usage.members, n->as.usage.memberCount);
    j->indent--; newline(j); fputc('}', j->out);
}

static void emitPackage(J* j, const Node* n) {
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr(j, "kind", "Package");
    sep(j, &first); emitKey(j, "name"); emitToken(j, n->as.scope.name);
    sep(j, &first); emitFieldStr(j, "visibility", visStr(n->as.scope.visibility));
    sep(j, &first); emitKey(j, "members");
    emitNodeArray(j, n->as.scope.members, n->as.scope.memberCount);
    j->indent--; newline(j); fputc('}', j->out);
}

static void emitProgram(J* j, const Node* n) {
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr(j, "kind", "Program");
    sep(j, &first); emitKey(j, "members");
    emitNodeArray(j, n->as.scope.members, n->as.scope.memberCount);
    j->indent--; newline(j); fputc('}', j->out);
}

static void emitAlias(J* j, const Node* n) {
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr(j, "kind", "Alias");
    sep(j, &first); emitKey(j, "name"); emitToken(j, n->as.alias.name);
    sep(j, &first); emitKey(j, "target");
    if (n->as.alias.target) emitNode(j, n->as.alias.target);
    else                    fputs("null", j->out);
    j->indent--; newline(j); fputc('}', j->out);
}

static void emitComment(J* j, const Node* n) {
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr(j, "kind", "Comment");
    sep(j, &first); emitKey(j, "name");
    if (n->as.comment.name.length > 0) emitToken(j, n->as.comment.name);
    else                                fputs("null", j->out);
    sep(j, &first); emitKey(j, "about"); emitNodeList(j, &n->as.comment.about);
    /* Body: trim leading whitespace for cleaner output. */
    {
        const char* s = n->as.comment.body.start;
        int len = n->as.comment.body.length;
        while (len > 0 && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) {
            s++; len--;
        }
        sep(j, &first); emitKey(j, "body"); emitJsonString(j, s, len);
    }
    j->indent--; newline(j); fputc('}', j->out);
}

static void emitDependency(J* j, const Node* n) {
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr(j, "kind", "Dependency");
    sep(j, &first); emitKey(j, "name");
    if (n->as.dependency.name.length > 0) emitToken(j, n->as.dependency.name);
    else                                   fputs("null", j->out);
    sep(j, &first); emitFieldStr(j, "visibility", visStr(n->as.dependency.visibility));
    sep(j, &first); emitKey(j, "sources"); emitNodeList(j, &n->as.dependency.sources);
    sep(j, &first); emitKey(j, "targets"); emitNodeList(j, &n->as.dependency.targets);
    j->indent--; newline(j); fputc('}', j->out);
}

static void emitSuccession(J* j, const Node* n) {
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr(j, "kind", "Succession");
    sep(j, &first); emitKey(j, "name");
    if (n->as.succession.name.length > 0) emitToken(j, n->as.succession.name);
    else                                   fputs("null", j->out);
    sep(j, &first); emitFieldStr(j, "visibility", visStr(n->as.succession.visibility));
    sep(j, &first); emitKey(j, "first");
    if (n->as.succession.first) emitNode(j, n->as.succession.first);
    else                        fputs("null", j->out);
    sep(j, &first); emitKey(j, "targets"); emitNodeList(j, &n->as.succession.targets);
    j->indent--; newline(j); fputc('}', j->out);
}

static const char* lifecycleStr(LifecycleKind k) {
    switch (k) {
    case LIFECYCLE_ENTRY: return "entry";
    case LIFECYCLE_DO:    return "do";
    case LIFECYCLE_EXIT:  return "exit";
    }
    return "?";
}

static void emitTransition(J* j, const Node* n) {
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr(j, "kind", "Transition");
    sep(j, &first); emitKey(j, "name");
    if (n->as.transition.name.length > 0) emitToken(j, n->as.transition.name);
    else                                   fputs("null", j->out);
    sep(j, &first); emitFieldStr(j, "visibility", visStr(n->as.transition.visibility));
    sep(j, &first); emitKey(j, "first");
    if (n->as.transition.first) emitNode(j, n->as.transition.first);
    else                        fputs("null", j->out);
    sep(j, &first); emitKey(j, "accept");
    if (n->as.transition.accept) emitNode(j, n->as.transition.accept);
    else                         fputs("null", j->out);
    sep(j, &first); emitKey(j, "guard");
    if (n->as.transition.guard) emitNode(j, n->as.transition.guard);
    else                        fputs("null", j->out);
    sep(j, &first); emitKey(j, "effect");
    if (n->as.transition.effect) emitNode(j, n->as.transition.effect);
    else                         fputs("null", j->out);
    sep(j, &first); emitKey(j, "target");
    if (n->as.transition.target) emitNode(j, n->as.transition.target);
    else                         fputs("null", j->out);
    j->indent--; newline(j); fputc('}', j->out);
}

static void emitLifecycleAction(J* j, const Node* n) {
    bool first = true;
    fputc('{', j->out); j->indent++; newline(j);
    sep(j, &first); emitFieldStr(j, "kind", "LifecycleAction");
    sep(j, &first); emitFieldStr(j, "lifecycle", lifecycleStr(n->as.lifecycleAction.kind));
    sep(j, &first); emitKey(j, "action");
    if (n->as.lifecycleAction.action) emitNode(j, n->as.lifecycleAction.action);
    else                              fputs("null", j->out);
    j->indent--; newline(j); fputc('}', j->out);
}

/* Dispatcher. */
static void emitNode(J* j, const Node* n) {
    if (!n) { fputs("null", j->out); return; }
    switch (n->kind) {
    case NODE_PROGRAM:        emitProgram      (j, n); break;
    case NODE_PACKAGE:        emitPackage      (j, n); break;
    case NODE_DEFINITION:     emitDefinition   (j, n); break;
    case NODE_USAGE:          emitUsage        (j, n); break;
    case NODE_ATTRIBUTE:      emitAttribute    (j, n); break;
    case NODE_IMPORT:         emitImport       (j, n); break;
    case NODE_QUALIFIED_NAME: emitQualifiedName(j, n); break;
    case NODE_MULTIPLICITY:   emitMultiplicityNode(j, n); break;
    case NODE_DOC:            emitDoc          (j, n); break;
    case NODE_LITERAL:        emitLiteral      (j, n); break;
    case NODE_BINARY:         emitBinary       (j, n); break;
    case NODE_UNARY:          emitUnary        (j, n); break;
    case NODE_ALIAS:          emitAlias        (j, n); break;
    case NODE_COMMENT:        emitComment      (j, n); break;
    case NODE_DEPENDENCY:     emitDependency   (j, n); break;
    case NODE_SUCCESSION:     emitSuccession   (j, n); break;
    case NODE_TRANSITION:     emitTransition   (j, n); break;
    case NODE_LIFECYCLE_ACTION: emitLifecycleAction(j, n); break;
    }
}

/* ---- public entry ---------------------------------------------- */

void emitJson(FILE* out, const Node* program) {
    J j = { .out = out, .indent = 0 };
    emitNode(&j, program);
    fputc('\n', out);
}
