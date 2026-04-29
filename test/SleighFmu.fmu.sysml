/*
 * SleighFmu.fmu.sysml — v0.27 fixture for `--emit-fmu-c`.
 *
 * Exercises ports → Terminals lowering on the standard (non-conjugate)
 * side of the Sleigh-Reindeer running example:
 *
 *   - port eyelet : HarnessConnectionPoint  (no `~`, so causalities
 *                                            are taken as-written)
 *   - port def with two items, both empty   → fmi3Binary variables
 *   - out item command  → causality="output"
 *   - in  item feedback → causality="input"
 *   - one <Terminal name="eyelet"> grouping both binaries with the
 *     dev.sml2c.port.* terminalKind / matchingRule / variableKind
 *
 * The conjugate path is in ReindeerFmu.fmu.sysml.
 *
 * The generated test_fmu.c readbacks each binary VR via fmi3GetBinary
 * and prints "<port>.<member> size=0" — confirming the routing
 * through ModelData and the size_t/buffer pair layout.
 */
package SleighFmu {
    part def Sleigh {
        port eyelet : HarnessConnectionPoint;
    }

    /* Port definition with one out and one in item — both empty.
     * Their direction drives the FMI causality of the lowered Binary
     * variables.                                                    */
    port def HarnessConnectionPoint {
        out item command  : Command;
        in  item feedback : Feedback;
    }

    /* Empty item defs — presence-only signals.  No attributes
     * means each lowers to one fmi3Binary FMI variable.            */
    item def Command;
    item def Feedback;
}
