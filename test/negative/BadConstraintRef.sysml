package BadConstraintRef {
    private import ScalarValues::*;

    // Engine is a part def, not a constraint def — referencing it from
    // `assert constraint` is a type error.
    part def Engine {
        attribute torque : Real;
    }

    part def User {
        assert constraint bad : Engine;
    }
}
