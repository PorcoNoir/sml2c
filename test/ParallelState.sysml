package ParallelState {
    private import ScalarValues::*;

    part def Engine {
        // v0.10: `parallel` on exhibit state usages is now silently
        // accepted (real SysML uses it for orthogonal state regions).
        exhibit state engineStates parallel {
            state off;
            state on;
        }
    }
}
