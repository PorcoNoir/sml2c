/* sysmlc — connectchecker.c
 *
 * Validates `connect a to b` and `from a to b` clauses.  Both forms
 * appear in the AST as a NODE_USAGE with a non-empty `ends` list of
 * qualified-name references.  Each reference must resolve to a
 * port-typed feature.
 *
 * For flows specifically, we additionally check direction:
 *   `from a`  — a's port direction must be `out` or `inout`
 *   `to b`    — b's port direction must be `in` or `inout`
 *
 * Direction checks only fire when the resolved end is *directly* a
 * port usage; indirect cases (a feature whose type contains an
 * inner port) are skipped — they need richer member-walking that
 * we'll add when there's demand.
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
        case DEF_ENUM:       return "enum value";
        case DEF_REFERENCE:  return "reference usage";
        }
        return "usage";
    case NODE_ATTRIBUTE:  return "attribute";
    case NODE_DEFINITION: return "definition";
    case NODE_PACKAGE:    return "package";
    default:              return "non-feature node";
    }
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

/* Flip a direction under conjugation: in↔out, inout stays inout
 * (because both halves are intrinsic), none stays none.            */
static Direction flipDirection(Direction d) {
    switch (d) {
    case DIR_IN:  return DIR_OUT;
    case DIR_OUT: return DIR_IN;
    default:      return d;
    }
}

/* What role does this end play? */
typedef enum {
    END_CONNECT,        /* `connect a to b` — no direction constraint   */
    END_FLOW_FROM,      /* source side of `flow from a to b`            */
    END_FLOW_TO         /* target side of `flow from a to b`            */
} EndRole;

static void checkEnd(const Node* endRef, EndRole role) {
    if (!endRef || endRef->kind != NODE_QUALIFIED_NAME) return;
    const Node* resolved = endRef->as.qualifiedName.resolved;
    /* Tentatively-accepted references: the resolver would have flagged
     * if it could.  Don't double-report. */
    if (!resolved) return;

    if (!isPortLike(resolved)) {
        connectionError(endRef->line,
                        "Connection end must reference a port-typed feature; got %s.",
                        describeNode(resolved));
        return;     /* don't bother with direction check on a non-port */
    }

    /* Direction check fires only for flows AND only when the resolved
     * end is directly a port usage.  Features with port-typed members
     * are deferred. */
    if (role == END_CONNECT) return;
    if (resolved->kind != NODE_USAGE || resolved->as.usage.defKind != DEF_PORT) return;

    Direction d = resolved->as.usage.direction;
    /* If the qname's resolution path crossed an odd number of
     * conjugated type references (`~Sensor` somewhere in the chain),
     * flip the effective direction.  Set by the resolver in
     * qname.conjugationParity.                                     */
    if (endRef->as.qualifiedName.conjugationParity) d = flipDirection(d);

    if (role == END_FLOW_FROM && d != DIR_OUT && d != DIR_INOUT) {
        connectionError(endRef->line,
                        "Flow source port must be 'out' or 'inout'; got '%s'.",
                        dirStr(d));
    }
    if (role == END_FLOW_TO && d != DIR_IN && d != DIR_INOUT) {
        connectionError(endRef->line,
                        "Flow target port must be 'in' or 'inout'; got '%s'.",
                        dirStr(d));
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
        const NodeList* ends = &n->as.usage.ends;
        if (ends->count > 0) {
            bool isFlow = (n->as.usage.defKind == DEF_FLOW);
            /* Parser produces exactly 2 ends; defensive code handles
             * other counts by treating extras as connection-style. */
            for (int i = 0; i < ends->count; i++) {
                EndRole role;
                if (isFlow && i == 0)      role = END_FLOW_FROM;
                else if (isFlow && i == 1) role = END_FLOW_TO;
                else                       role = END_CONNECT;
                checkEnd(ends->items[i], role);
            }
        }
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
