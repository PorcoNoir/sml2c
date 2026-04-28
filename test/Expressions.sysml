package ExprDemo {
    private import ScalarValues::*;

    part def Inner {
        attribute torque : Real;
    }

    part def Engine {
        // an inner part we can dot-access
        part engine : Inner;

        // simple integer literal
        attribute redline : Integer = 7000;

        // real literal
        attribute idleHours : Real = 3.5;

        // string literal default
        attribute name : String = "DefaultEngine";

        // bool literal
        attribute installed : Boolean = true;

        // term-vs-factor precedence: should print ((1) + ((2) * (3)))
        attribute computed : Integer = 1 + 2 * 3;

        // explicit grouping flips precedence: ((1 + 2) * 3)
        attribute grouped : Integer = (1 + 2) * 3;

        // left-associativity: ((10 - 4) - 1), not (10 - (4 - 1))
        attribute leftAssoc : Integer = 10 - 4 - 1;

        // unary minus and double negation
        attribute neg : Integer = -5;
        attribute doubleNeg : Integer = - -5;

        // unary applied to a parenthesized binary
        attribute negSum : Integer = -(1 + 2);

        // mixed comparison + arithmetic — comparison binds looser than +
        // expected: ((torque + 10) > redline)
        attribute overspeed : Boolean = torque + 10 > redline;

        // equality precedence: ((x + 1) == (y * 2))
        attribute equal : Boolean = x + 1 == y * 2;

        // bang on a comparison
        attribute notHot : Boolean = !(torque > 100);

        // identifier reference + dotted feature chain
        attribute downscaled : Real = engine.torque / 2;

        // typed attribute with default
        attribute torque : Real = 100;
        attribute x : Integer = 0;
        attribute y : Integer = 0;
    }
}
