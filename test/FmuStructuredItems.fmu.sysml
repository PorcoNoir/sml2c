/*
 * FmuStructuredItems.fmu.sysml — v0.29 fixture for `--emit-fmu-c`.
 *
 * Exercises items whose attributes are themselves item-typed (the F12
 * design item).  Each leaf scalar reachable through any depth of
 * item-typed attribute chaining lowers to its own FMI variable named
 * `<port>.<item>[.<attr>]+`, all under the same Terminal.
 *
 * The fixture covers:
 *
 *   - Nesting depth 2 (`pin.posture.position.x`, `pin.posture.position.y`).
 *     Vec2 is reached through Posture's `position` attribute.
 *   - Sibling at depth 1 (`pin.posture.heading`) — a non-item-typed
 *     attribute on the same item def, demonstrating that the recursion
 *     interleaves with leaf scalars at every level.
 *   - Top-level part-def attribute (`serial` — String parameter) for
 *     VR-allocation interleaving with port-driven variables.
 *   - Empty-item port member (`pin.reset` Binary) — the v0.27 path
 *     coexisting with the v0.29 paths in the same Terminal.
 *
 * Expected variable layout (source order):
 *
 *   serial                       String   parameter   tunable
 *   pin.posture.position.x       Float64  output      discrete
 *   pin.posture.position.y       Float64  output      discrete
 *   pin.posture.heading          Float64  output      discrete
 *   pin.reset                    Binary   input
 *
 * The smoke test reads each via the type-appropriate fmi3Get* call,
 * confirming the VR routing in the scalar getter, the Binary getter,
 * and ModelData's interleaved struct fields all stay in lockstep.
 *
 * Cycle protection: this fixture intentionally does NOT include a
 * recursive item def (e.g. `item def Node { attribute next : Node; }`)
 * because that would emit zero variables for `next` and confuse the
 * smoke-test diff.  Cycle break is exercised manually in the
 * development log.  v0.30+ may add it as a separate negative-test
 * fixture once the smoke-test format supports XFAIL-style cases.
 */
package FmuStructuredItems {
    private import ScalarValues::*;

    /* Leaf item: two scalar attributes, no nesting.                */
    item def Vec2 {
        attribute x : Real;
        attribute y : Real;
    }

    /* Composite item: one item-typed attribute (nesting trigger)
     * plus one scalar at the same level.                           */
    item def Posture {
        attribute position : Vec2;
        attribute heading  : Real;
    }

    /* Empty item: presence-only signal -> Binary.                  */
    item def Reset;

    /* Port def with a non-empty composite item and an empty item. */
    port def Telem {
        out item posture : Posture;
        in  item reset   : Reset;
    }

    /* Outer part def: top-level String parameter alongside the port. */
    part def Engine {
        attribute serial : String = "ENG-0001";
        port pin : Telem;
    }
}
