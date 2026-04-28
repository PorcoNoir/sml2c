package BadSuccessionForm {
    private import ScalarValues::*;

    action def A {
        action a;
        action b;
        // Missing `then` after `first` — parser should reject.
        succession bad first a;
    }
}
