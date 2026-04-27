/* sysmlc — referentialchecker.c
 *
 * Single tree walk that tracks the enclosing context and applies the
 * referential rules from clause 8.3.  See header for the rule list.
 *
 * Design notes:
 *  - We walk the tree carrying an `enclosing` pointer (the immediate
 *    parent Definition or Usage, or NULL at the package/program top).
 *    This is enough to dispatch all five inference rules.
 *  - All inference rules SET isReference=true; none clear it.  The
 *    spec lets the `ref` keyword make the flag explicit but doesn't
 *    treat its absence as a forced "false."
 *  - The flow-end count is the only error case.  Errors go through
 *    a small variadic emitter; the pass returns false if any fired.
 */
#include <stdio.h>
#include <stdarg.h>
#include "referentialchecker.h"

static int errorCount = 0;

static void refError(int line, const char* fmt, ...) {
    fprintf(stderr, "[line %d] Referential error: ", line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    errorCount++;
}

/* ---- predicates ------------------------------------------------ */

/* True when `n` is a Usage of kind PORT (the "PortUsage" in spec
 * terms — note: a Usage, not a Definition).                       */
static bool isPortUsage(const Node* n) {
    return n && n->kind == NODE_USAGE && n->as.usage.defKind == DEF_PORT;
}

/* True when `n` is a Definition of kind PORT.                     */
static bool isPortDefinition(const Node* n) {
    return n && n->kind == NODE_DEFINITION
        && n->as.scope.defKind == DEF_PORT;
}

/* True when the enclosing context demands referentiality for any
 * Usage nested directly inside it.  This is the rule from
 * validatePortUsageNestedUsagesNotComposite (for non-port nesteds
 * of a PortUsage) and validateAttributeDefinitionFeatures.        */
static bool enclosingForcesReference(const Node* enclosing, const Node* member) {
    if (!enclosing) return false;
    /* Members of an AttributeDefinition are always referential. */
    if (enclosing->kind == NODE_DEFINITION
        && enclosing->as.scope.defKind == DEF_DATATYPE) return true;
    /* Attributes in our parser use NODE_ATTRIBUTE, but attribute defs
     * also use the same DEF_DATATYPE marker on the Definition path. */
    /* nestedUsages of a PortUsage that are NOT themselves PortUsages
     * must be referential.                                         */
    if (isPortUsage(enclosing) && !isPortUsage(member)) return true;
    return false;
}

/* Rule #1 + #3 condensed: should this Usage be inferred referential
 * given its own attributes plus its enclosing context?            */
static bool usageMustBeReferential(const Node* usage, const Node* enclosing) {
    if (!usage || usage->kind != NODE_USAGE) return false;

    /* (1) directed → ref */
    if (usage->as.usage.direction != DIR_NONE) return true;

    /* (1) end feature → ref */
    if (usage->as.usage.defKind == DEF_END) return true;

    /* (1) no enclosing type → ref (top-level usage at package level) */
    if (!enclosing
        || enclosing->kind == NODE_PACKAGE
        || enclosing->kind == NODE_PROGRAM) return true;

    /* (3) PortUsage whose owningType is not a Port → ref */
    if (isPortUsage(usage)
        && !isPortUsage(enclosing) && !isPortDefinition(enclosing)) return true;

    /* (4)/(5) enclosing context forces reference */
    if (enclosingForcesReference(enclosing, usage)) return true;

    return false;
}

/* ---- the walk -------------------------------------------------- */

/* Forward declaration. */
static void walk(Node* n, const Node* enclosing);

/* Apply rules to a Usage node before recursing into its body.    */
static void inferOnUsage(Node* u, const Node* enclosing) {
    /* Rule #2 special case in the parser: AttributeUsage in spec,
     * NODE_ATTRIBUTE in our AST.  Handled in inferOnAttribute().  */
    if (usageMustBeReferential(u, enclosing)) {
        u->as.usage.isReference = true;
    }
}

static void inferOnAttribute(Node* a, const Node* enclosing) {
    (void)enclosing;
    /* Rule #2: every AttributeUsage is referential. */
    a->as.attribute.isReference = true;
}

/* Rule #6: a FlowDefinition may not have more than two ends.  Ends
 * are member Usages with defKind = DEF_END.                      */
static void checkFlowDefinitionEnds(const Node* def) {
    if (def->kind != NODE_DEFINITION) return;
    if (def->as.scope.defKind != DEF_FLOW) return;
    int endCount = 0;
    for (int i = 0; i < def->as.scope.memberCount; i++) {
        const Node* m = def->as.scope.members[i];
        if (m && m->kind == NODE_USAGE && m->as.usage.defKind == DEF_END) {
            endCount++;
        }
    }
    if (endCount > 2) {
        refError(def->line,
                 "FlowDefinition '%.*s' has %d ends; at most 2 are allowed.",
                 def->as.scope.name.length, def->as.scope.name.start,
                 endCount);
    }
}

/* Generic recursion. */
static void walk(Node* n, const Node* enclosing) {
    if (!n) return;
    switch (n->kind) {
    case NODE_PROGRAM:
    case NODE_PACKAGE:
        for (int i = 0; i < n->as.scope.memberCount; i++) {
            walk(n->as.scope.members[i], n);
        }
        break;

    case NODE_DEFINITION:
        checkFlowDefinitionEnds(n);
        for (int i = 0; i < n->as.scope.memberCount; i++) {
            walk(n->as.scope.members[i], n);
        }
        break;

    case NODE_USAGE:
        inferOnUsage(n, enclosing);
        for (int i = 0; i < n->as.usage.memberCount; i++) {
            walk(n->as.usage.members[i], n);
        }
        break;

    case NODE_ATTRIBUTE:
        inferOnAttribute(n, enclosing);
        break;

    default:
        break;
    }
}

bool checkReferential(Node* program) {
    errorCount = 0;
    walk(program, NULL);
    if (errorCount > 0) {
        fprintf(stderr, "Referential checking failed with %d error%s.\n",
                errorCount, errorCount == 1 ? "" : "s");
        return false;
    }
    return true;
}
