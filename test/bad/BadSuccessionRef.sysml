package BadSuccessionRef {
    private import ScalarValues::*;

    part def Engine {
        attribute torque : Real;
    }

    // Succession references must point at an action usage; Engine is a
    // part def, so this should be rejected.
    action def Process {
        first start;
        then Engine;
        then done;
    }
}
