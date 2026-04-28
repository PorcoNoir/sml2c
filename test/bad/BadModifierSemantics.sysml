package BadModifierSemantics {
    private import ScalarValues::*;

    // Error: 'constant' attribute must have a value.
    constant attribute pi : Real;

    // Error: 'derived' attribute must have a derivation expression.
    derived attribute area : Real;

    part def Vehicle {
        constant attribute speedLimit : Real = 100.0;
    }

    // Error: redefining a 'constant' attribute must also be 'constant'.
    part def Car :> Vehicle {
        attribute speedLimit :>> speedLimit : Real = 80.0;
    }
}
