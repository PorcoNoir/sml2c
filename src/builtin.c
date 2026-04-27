/* sysmlc — builtin.c
 *
 * Synthetic standard library.  See builtin.h for context.
 *
 * The trick is that we construct a regular AST — same Node struct,
 * same DefKind/Node fields the parser produces — but we do it in C
 * rather than via parsing.  Tokens point at static string literals,
 * which have program-lifetime, so they're safe to store.
 *
 * The specializes references are pre-resolved at construction time:
 * we set `qualifiedName.resolved` directly so the resolver doesn't
 * need to look them up again.  This also ensures the typechecker
 * can walk the supertype chain without going through the resolver.
 */
#include <stdlib.h>
#include <string.h>

#include "builtin.h"

/* ---- module-static singletons ---------------------------------- */

static Node* sNumber  = NULL;
static Node* sReal    = NULL;
static Node* sInteger = NULL;
static Node* sBoolean = NULL;
static Node* sString  = NULL;
static Node* sPackage = NULL;

/* ---- construction helpers -------------------------------------- */

static Token mkTok(const char* lexeme) {
    Token t = {0};
    t.type   = TOKEN_IDENTIFIER;
    t.start  = lexeme;
    t.length = (int)strlen(lexeme);
    t.line   = 0;          /* synthetic — doesn't appear in source  */
    return t;
}

static Node* mkDatatype(const char* name) {
    Node* n = astMakeNode(NODE_DEFINITION, 0);
    n->as.scope.name    = mkTok(name);
    n->as.scope.defKind = DEF_DATATYPE;
    return n;
}

/* Build a qualified-name reference whose `resolved` is filled in
 * up-front.  Used to wire the synthetic specializes lists.        */
static Node* mkResolvedRef(const char* name, const Node* resolved) {
    Node* q = astMakeNode(NODE_QUALIFIED_NAME, 0);
    astAppendQualifiedPart(q, mkTok(name));
    q->as.qualifiedName.resolved = resolved;
    return q;
}

static void addSpecializes(Node* def, const char* parentName, const Node* parent) {
    astListAppend(&def->as.scope.specializes, mkResolvedRef(parentName, parent));
}

/* ---- one-shot init --------------------------------------------- */

static void initIfNeeded(void) {
    if (sPackage) return;        /* already built                    */

    sNumber  = mkDatatype("Number");
    sReal    = mkDatatype("Real");
    sInteger = mkDatatype("Integer");
    sBoolean = mkDatatype("Boolean");
    sString  = mkDatatype("String");

    addSpecializes(sReal,    "Number", sNumber);
    addSpecializes(sInteger, "Real",   sReal);

    sPackage = astMakeNode(NODE_PACKAGE, 0);
    sPackage->as.scope.name = mkTok("ScalarValues");
    astAppendScopeMember(sPackage, sNumber);
    astAppendScopeMember(sPackage, sReal);
    astAppendScopeMember(sPackage, sInteger);
    astAppendScopeMember(sPackage, sBoolean);
    astAppendScopeMember(sPackage, sString);
}

/* ---- public accessors ------------------------------------------ */

const Node* builtinScalarValuesPackage(void) { initIfNeeded(); return sPackage; }
const Node* builtinNumber (void)              { initIfNeeded(); return sNumber;  }
const Node* builtinReal   (void)              { initIfNeeded(); return sReal;    }
const Node* builtinInteger(void)              { initIfNeeded(); return sInteger; }
const Node* builtinBoolean(void)              { initIfNeeded(); return sBoolean; }
const Node* builtinString (void)              { initIfNeeded(); return sString;  }
