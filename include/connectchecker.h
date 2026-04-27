/* sysmlc — connectchecker.h
 *
 * Validates that every reference appearing in a `connect a to b` or
 * `from a to b` clause resolves to a port-typed feature.  A feature
 * is "port-typed" if it is itself a port usage (NODE_USAGE with
 * defKind == DEF_PORT) or if its declared type is a port definition.
 *
 * Errors:
 *   [line N] Connection error: Connection end must reference a
 *            port-typed feature; got <description>.
 *
 * Returns true if no errors were reported.
 */
#ifndef SYSMLC_CONNECTCHECKER_H
#define SYSMLC_CONNECTCHECKER_H

#include <stdbool.h>
#include "ast.h"

bool checkConnections(const Node* program);

#endif /* SYSMLC_CONNECTCHECKER_H */
