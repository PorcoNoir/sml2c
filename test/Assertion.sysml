package Assertion {
    private import ScalarValues::*;

    constraint def C {
        in x : Real;
        x > 0
    }

    part def Engine {
        attribute t : Real;
        // v0.9: bare constraint usages without prefix are now accepted
        // (real SysML uses these constantly).
        constraint c : C;
    }
}
