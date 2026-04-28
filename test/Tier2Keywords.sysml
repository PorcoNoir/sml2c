// v0.7 — tier 2 keywords (occurrence / event / individual / snapshot /
// allocation / view / actor / variant / message / attribute def).
package Tier2KeywordsDemo {
    private import ScalarValues::*;

    // attribute def — first-class attribute definition.
    attribute def Mass :> Real;
    attribute def Velocity;
    attribute def Acceleration :> Real;

    // occurrence usage.
    part def Vehicle {
        occurrence drive;
        // event flag on occurrence.
        event occurrence engineStarted;
        event occurrence brakeApplied;
    }

    // individual: instance-level inheritance.
    individual def Vehicle_1 :> Vehicle;
    individual def Vehicle_2 :> Vehicle;

    // snapshot: time-instant feature value.
    part def Scenario {
        snapshot t0 : Vehicle_1;
        snapshot t1 : Vehicle_1;
    }

    // allocation def.
    allocation def LogicalToPhysical;

    // view, viewpoint, concern, rendering.
    view def StakeholderView;
    viewpoint def CostViewpoint;
    rendering def DefaultRendering;
    concern def CostConcern;

    // variant / variation.
    variation def Configuration;

    // actor / use case / include.
    actor def Driver;
    actor def Passenger;
    use case def StartVehicle;
    use case def DriveVehicle;

    // message def — event-flow definition.
    item def IgnitionCmd;
    part def Driver_part {
        out port input : IgnitionCmd;
    }
    part def Vehicle_part {
        in port input : IgnitionCmd;
    }
    part def Scenario_msg {
        part driver : Driver_part;
        part vehicle : Vehicle_part;
        message of IgnitionCmd from driver.input to vehicle.input;
    }
}
