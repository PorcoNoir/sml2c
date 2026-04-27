package BareRefDemo {
    private import ScalarValues::*;

    part def Engine {
        attribute torque : Real;
    }

    // Bare ReferenceUsage — `ref name : T;` with no kind keyword.
    // The spec calls these "ReferenceUsages" (§8.3.6.3).
    part def Vehicle {
        // Simple bare-ref with a type.
        ref engine : Engine;

        // Bare-ref with an initializer.
        ref nominalTorque : Real = 100.0;

        // Bare-ref with multiplicity and specialization.
        ref backupEngines : Engine [0..2];
    }

    // Bare-ref at package scope — automatically referential per §8.3.6.4
    // (no enclosing type), so the explicit `ref` is redundant but legal.
    ref globalEngine : Engine;
}
