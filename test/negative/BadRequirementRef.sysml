package BadRequirementRef {
    private import ScalarValues::*;

    part def Engine {
        attribute torque : Real;
    }

    part def Vehicle {
        // Engine is a part def, not a requirement def.
        require requirement bad : Engine;
    }
}
