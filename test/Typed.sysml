package TypedDemo {
    private import ScalarValues::*;

    part def Engine {
        // Real attribute with Integer default — Integer specializes Real
        attribute torque : Real = 100;

        // Real attribute with Real default
        attribute idle : Real = 800.0;

        // Integer attribute with Integer default
        attribute redline : Integer = 7000;

        // String attribute with String default
        attribute name : String = "engine";

        // Boolean attribute with Boolean default
        attribute active : Boolean = true;

        // Arithmetic, mixed Integer + Real → Real, fits Real attribute
        attribute boost : Real = 100 + 1.5;

        // Comparison yields Boolean, fits Boolean attribute
        attribute hot : Boolean = torque > 50;

        // Unary negation, Integer → Integer
        attribute offset : Integer = -10;

        // Unary bang on Boolean → Boolean
        attribute idle_check : Boolean = !active;

        // Identifier reference: torque is Real, fits Real
        attribute alias : Real = torque;

        // Equality on compatible types → Boolean
        attribute matches : Boolean = redline == 7000;
    }
}
