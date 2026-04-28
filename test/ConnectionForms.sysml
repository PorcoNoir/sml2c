// v0.7 — connection-form variants and tier-2 keywords.
package ConnectionFormsDemo {
    private import ScalarValues::*;

    part def Port;
    part def Engine {
        out port lugNutPort : Port;
        in port shankPort : Port;
    }
    part def Wheel {
        out port lugNutPort : Port;
        in port shankPort : Port;
    }
    interface def WheelFastenerInterface;

    item def Fuel;
    item def IgnitionCmd;

    part def Vehicle {
        port lugNutPort : Port;
        port shankPort : Port;
        part engine : Engine;
        part wheels : Wheel;

        // Multiplicity-prefixed connect with dot-paths on both ends.
        connect [1] wheels.lugNutPort to [1] engine.shankPort;

        // Plain dot-path connect.
        connect engine.lugNutPort to wheels.shankPort;

        // Interface usage with inline connect clause.
        interface fastener : WheelFastenerInterface [5]
            connect lugNutPort to shankPort;

        // Bind: equality between two feature references.
        bind lugNutPort = engine.lugNutPort;

        // Flow with `of` typed clause.
        flow of Fuel from engine.lugNutPort to wheels.shankPort;

        // Named flow.
        flow myFlow from engine.lugNutPort to wheels.shankPort;

        // Anonymous flow.
        flow from engine.lugNutPort to wheels.shankPort;
    }

    part def Logical;
    part def Physical;

    part def Allocation {
        part logical : Logical;
        part physical : Physical;

        // Allocate without body.
        allocate logical to physical;
    }
}
