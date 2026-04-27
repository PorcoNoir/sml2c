package SpecializationDemo {
    import ScalarValues::*;

    /* Plain definition — baseline. */
    part def Engine {
        attribute torque : Real;
    }

    /* part def with symbolic ':>' specialization. */
    part def TurboEngine :> Engine {
        /* attribute that redefines an inherited one with ':>>' */
        attribute torque :>> torque;
    }

    /* Same idea using the keyword forms. */
    part def DieselEngine specializes Engine {
        attribute torque redefines torque;
    }

    /* Attribute carrying all three relationships in one declaration:
     *   typed by Real, specializes Mass, redefines Engine::torque. */
    part def InstrumentedEngine :> Engine {
        attribute torque : Real :> ScalarValues::Real :>> Engine::torque;
    }

    /* part usage that specializes a type. */
    part myEngine : Engine :> TurboEngine;

    /* part usage with all three: typed, specializes, redefines, and a body. */
    part diagEngine : Engine :> TurboEngine :>> myEngine {
        attribute reading : Real;
    }
}
