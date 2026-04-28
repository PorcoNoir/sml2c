package MemberAccessDemo {
    private import ScalarValues::*;

    part def Engine {
        attribute mass : Real;
        attribute cost : Real;
    }

    part def Wheel {
        attribute diameter : Real;
    }

    part def Vehicle {
        part engine : Engine;
        part wheels : Wheel;

        // Single-step member access.
        attribute engineMass : Real = engine.mass;
        attribute engineCost : Real = engine.cost;
        attribute wheelSize : Real = wheels.diameter;

        // Member access in arithmetic.
        attribute totalCost : Real = engine.cost + 1000;
        attribute combined : Real = engine.mass + wheels.diameter * 4;

        // Member access nested in unary.
        attribute negEngineMass : Real = -engine.mass;
    }

    // Multi-level chains require typed AST to fully resolve.
    // For now verify they parse and round-trip.
    part def Outer {
        part inner : Vehicle;
        attribute deepCost : Real = inner.engine.cost;
    }
}
