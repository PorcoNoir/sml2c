package Dup {
    private import ScalarValues::*;

    part def Engine {
        attribute torque : Real;
        attribute torque : Real;     // duplicate, same scope
    }

    // duplicate top-level definitions
    part def Reactor { }
    part def Reactor { }
}
