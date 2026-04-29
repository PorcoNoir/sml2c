/*
 * Smoke test for --emit-c (v0.15).
 *
 * Mixes top-level static-const attributes with a struct-emittable
 * part def and a deliberately unemittable part def, to verify that
 * codegen handles both shapes and skips cleanly when needed.
 */
package EmitC {
    private import ScalarValues::*;

    /* Top-level primitives — should become `static const`. */
    attribute pi      : Real    = 3.14;
    attribute count   : Integer = 7;
    attribute tag     : String  = "ok";
    attribute enabled : Boolean = true;

    /* Expression default — should round-trip through emit-c with
     * the right type, parens, and operator. */
    attribute area    : Real    = pi * 2.0 + 1.0;

    /* Struct-emittable: all members are primitive attributes. */
    part def Vehicle {
        attribute mass     : Real;
        attribute name     : String;
        attribute capacity : Integer;
        attribute electric : Boolean;
    }

    /* Deliberately unemittable: contains a non-attribute member.
     * Codegen should skip with a comment, not partial-emit. */
    part def Truck {
        attribute mass : Real;
        part engine : Vehicle;
    }
}
