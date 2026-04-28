package RequirementDemo {
    private import ScalarValues::*;

    constraint def TorqueLimit {
        in torque : Real;
        in maxT : Real;
        torque <= maxT
    }

    constraint def WeightLimit {
        in mass : Real;
        mass <= 5000
    }

    part def Engine {
        attribute torque : Real;
        attribute maxT : Real;
    }

    part def Vehicle {
        part engine : Engine;
        attribute mass : Real;
    }

    // Requirement def carrying a subject plus assume + require constraints.
    requirement def MaxTorque {
        subject e : Engine;
        assume constraint baseline : TorqueLimit;
        require constraint mainLimit : TorqueLimit;
    }

    // Requirement def with multiple require clauses.
    requirement def VehicleSpec {
        subject v : Vehicle;
        require constraint torqueOk : TorqueLimit;
        require constraint weightOk : WeightLimit;
    }

    // Bare requirement def with no constraints (allowed) — just a subject.
    requirement def NamedSubject {
        subject e : Engine;
    }

    // Top-level requirement usage on a part.
    part def Sedan :> Vehicle {
        require requirement r1 : VehicleSpec;
    }
}
