package BadAssertion {
    private import ScalarValues::*;

    constraint def C {
        in x : Real;
        x > 0
    }

    part def Engine {
        attribute t : Real;
        // Missing assert/assume/require prefix — parser should reject.
        constraint c : C;
    }
}
