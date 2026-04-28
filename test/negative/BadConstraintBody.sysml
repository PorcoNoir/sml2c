package BadConstraintBody {
    private import ScalarValues::*;

    // Body is a numeric expression, not a Boolean — should be rejected.
    constraint def Wrong {
        in x : Real;
        x + 1
    }
}
