package BadTransitionForm {
    private import ScalarValues::*;

    state def S {
        state a;
        state b;
        // Missing required `then` clause.
        transition first a;
    }
}
