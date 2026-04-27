/* sysmlc — referentialchecker.h
 *
 * Implements six rules from the SysML v2 abstract syntax (clause 8.3):
 *
 *   1. validateUsageIsReferential (8.3.6.4) — a Usage that is directed,
 *      is an end feature, or has no enclosing type must have
 *      isReference = true.
 *
 *   2. AttributeUsage::isReference (8.3.7.3) — every AttributeUsage
 *      has isReference = true by definition.
 *
 *   3. validatePortUsageIsReference (8.3.12.6) — a PortUsage whose
 *      owningType is not a PortDefinition or PortUsage must have
 *      isReference = true.
 *
 *   4. validatePortUsageNestedUsagesNotComposite (8.3.12.6) — the
 *      nestedUsages of a PortUsage that are not themselves PortUsages
 *      must have isReference = true.
 *
 *   5. validateAttributeDefinitionFeatures (8.3.7.2) — all features of
 *      an AttributeDefinition must have isReference = true.
 *
 *   6. validateFlowDefinitionFlowEnds (8.3.16.2) — a FlowDefinition
 *      may not have more than two flowEnds.
 *
 * Rules 1–5 are inference rules: where the spec says "must be
 * referential," we silently set isReference = true rather than
 * erroring on missing `ref` keyword.  This matches the Pilot
 * Implementation behavior, where `ref` is a way to make the flag
 * explicit but the flag itself is determined by context.
 *
 * Rule 6 is a hard error.
 */
#ifndef SYSMLC_REFERENTIAL_CHECKER_H
#define SYSMLC_REFERENTIAL_CHECKER_H

#include <stdbool.h>
#include "ast.h"

bool checkReferential(Node* program);

#endif /* SYSMLC_REFERENTIAL_CHECKER_H */
