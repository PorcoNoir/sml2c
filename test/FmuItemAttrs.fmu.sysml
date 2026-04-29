/*
 * FmuItemAttrs.fmu.sysml — v0.28 fixture for `--emit-fmu-c`.
 *
 * Exercises items with scalar attributes (F11 in the design doc).
 * Each port-member item attribute lowers to its own FMI variable
 * with structured name `<port>.<item>.<attr>`, all under the same
 * Terminal as TerminalMemberVariable rows.
 *
 * The fixture mixes an empty item (Ack -> Binary, the v0.27 path)
 * and a non-empty item (Reading -> two Float64 scalars, the v0.28
 * path) inside the same port def, to verify both lowering paths
 * coexist with a contiguous VR sequence.
 *
 * Expected variables (in source order, VR 1..3):
 *
 *   pin.reading.rpm          : Float64  causality=output  (item's `out` direction)
 *   pin.reading.oilPressure  : Float64  causality=output
 *   pin.ack                  : Binary   causality=input   (item's `in` direction)
 *
 * The generated test_fmu.c reads each via the type-appropriate
 * fmi3Get* call and prints the value, demonstrating that the VR
 * routing in the scalar getter and the Binary getter both index
 * into the same ModelData struct.
 */
package FmuItemAttrs {
    private import ScalarValues::*;

    /* Port def with one non-empty and one empty item.            */
    port def Mixed {
        out item reading : Reading;
        in  item ack     : Ack;
    }

    /* Non-empty item -> flattened into per-attribute scalar vars. */
    item def Reading {
        attribute rpm         : Real;
        attribute oilPressure : Real;
    }

    /* Empty item -> single Binary (presence-only signal).        */
    item def Ack;

    part def Engine {
        port pin : Mixed;
    }
}
