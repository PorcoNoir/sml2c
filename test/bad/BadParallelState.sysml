package BadParallelState {
    private import ScalarValues::*;

    part def Engine {
        // The `parallel` keyword on exhibit state is not yet supported.
        exhibit state engineStates parallel {
            state off;
            state on;
        }
    }
}
