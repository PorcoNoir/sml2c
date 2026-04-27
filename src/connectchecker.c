/* sysmlc — connectchecker.c
 *
 * Validates `connect a to b` and `from a to b` clauses.  Both forms
 * appear in the AST as a NODE_USAGE with a non-empty `ends` list of
 * qualified-name references.  Each reference must resolve to a
 * port-typed feature.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "connectchecker.h"

static int errorCount = 0;

static void connectionError(int line, const char* fmt, ...) {
    fprintf(stderr, "[line %d] Connection error: ", line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    errorCount++;
}

/* Pull the resolved declared type of a feature node, if any. */
static const Node* declaredType(const Node* n) {
    if (!n) return NULL;
    const NodeList* types = NULL;
    if (n->kind == NODE_USAGE)         types = &n->as.usage.types;
    else if (n->kind == NODE_ATTRIBUTE) types = &n->as.attribute.types;
    if (!types || types->count == 0) return NULL;
    const Node* ref = types->items[0];
    if (!ref || ref->kind != NODE_QUALIFIED_NAME) return NULL;
    return ref->as.qualifiedName.resolved;
}

/* True when `n` is itself a port usage, or a feature whose declared
 * type is a port definition.                                       */
static bool isPortLike(const Node* n) {
    if (!n) return false;
    if (n->kind == NODE_USAGE && n->as.usage.defKind == DEF_PORT) return true;
    const Node* t = declaredType(n);
    if (t && t->kind == NODE_DEFINITION && t->as.scope.defKind == DEF_PORT) return true;
    return false;
}

/* Brief human description of a node, for diagnostics. */
static const char* describeNode(const Node* n) {
    if (!n) return "<unresolved reference>";
    switch (n->kind) {
    case NODE_USAGE:
        switch (n->as.usage.defKind) {
        case DEF_PART:       return "part usage";
        case DEF_PORT:       return "port usage";       /* shouldn't fail */
        case DEF_INTERFACE:  return "interface usage";
        case DEF_ITEM:       return "item usage";
        case DEF_CONNECTION: return "connection usage";
        case DEF_FLOW:       return "flow usage";
        case DEF_END:        return "end declaration";
        case DEF_DATATYPE:   return "datatype usage";
        }
        return "usage";
    case NODE_ATTRIBUTE:  return "attribute";
    case NODE_DEFINITION: return "definition";
    case NODE_PACKAGE:    return "package";
    default:              return "non-feature node";
    }
}

/* Check one end reference. */
static void checkEnd(const Node* endRef) {
    if (!endRef || endRef->kind != NODE_QUALIFIED_NAME) return;
    const Node* resolved = endRef->as.qualifiedName.resolved;
    /* If the reference was tentatively accepted (resolved is NULL),
     * the resolver would have already flagged it if it could.  We
     * defer rather than double-reporting here.                    */
    if (!resolved) return;
    if (!isPortLike(resolved)) {
        connectionError(endRef->line,
                        "Connection end must reference a port-typed feature; got %s.",
                        describeNode(resolved));
    }
}

/* Walk the AST. */
static void walk(const Node* n) {
    if (!n) return;
    switch (n->kind) {
    case NODE_PROGRAM:
    case NODE_PACKAGE:
    case NODE_DEFINITION:
        for (int i = 0; i < n->as.scope.memberCount; i++) {
            walk(n->as.scope.members[i]);
        }
        break;

    case NODE_USAGE: {
        /* If this usage is a connection or flow with explicit endpoints,
         * verify each end resolves to a port-like feature. */
        const NodeList* ends = &n->as.usage.ends;
        if (ends->count > 0) {
            for (int i = 0; i < ends->count; i++) {
                checkEnd(ends->items[i]);
            }
        }
        /* Also recurse into the body (a connection def body might
         * contain nested usages with their own connect clauses). */
        for (int i = 0; i < n->as.usage.memberCount; i++) {
            walk(n->as.usage.members[i]);
        }
        break;
    }

    default:
        break;
    }
}

bool checkConnections(const Node* program) {
    errorCount = 0;
    walk(program);
    if (errorCount > 0) {
        fprintf(stderr, "Connection checking failed with %d error%s.\n",
                errorCount, errorCount == 1 ? "" : "s");
        return false;
    }
    return true;
}
