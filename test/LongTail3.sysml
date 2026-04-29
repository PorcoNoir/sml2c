// v0.11 — final long-tail pass.  Each construct here was a parser
// hole on the PTC reference file before this version.
package LongTail3 {
    private import ScalarValues::*;

    // (1) Tier 4 keywords: verify, frame, stakeholder, render, expose,
    //     concern.
    concern def VehicleSafety {
        doc /* identify system safety features */
    }
    part def SafetyEngineer;

    viewpoint def SafetyViewpoint {
        frame concern vs : VehicleSafety;
        stakeholder se : SafetyEngineer;
    }

    rendering def TreeDiagram;
    view def TreeView {
        render asTreeDiagram : TreeDiagram;
    }
    view def NestedView;

    metadata def Safety { attribute isMandatory : Boolean; }
    part def PartsTree;
    view vehicleView : TreeView {
        expose PartsTree::**;
        filter @Safety;
    }

    // (2) `subject` form variants.
    requirement def Spec;
    part def Vehicle;
    requirement r1 : Spec {
        subject vehicle : Vehicle;
        subject = expr;
        subject vehicle1 default expr;
        subject vehicle2 [2] :> vehicle;
    }
    attribute expr;

    // (3) `dependency a to b;` (no `from`).
    part def A;
    part def B;
    dependency A to B;

    // (4) `assert <ref>;` / `require <ref>;` (no constraint/requirement keyword).
    requirement def MassReq;
    part def Subject2 {
        assert MassReq;
        require MassReq;
    }

    // (5) Attribute with direction modifier.
    action def Compute {
        in attribute scenario : Vehicle;
        out attribute result : Vehicle;
    }

    // (6) `then event <ref>;` succession target.  The target must be
    //     an action usage to satisfy the typechecker; the `event`
    //     keyword is parser sugar that we accept and discard.
    action def OccAction;
    action sendSensedSpeed : OccAction;
    action def Activity {
        first start;
        then event sendSensedSpeed;
    }

    // (7) `satisfy requirement <name>:<ref>;`.
    part def Subject3 {
        satisfy requirement sv : MassReq;
    }
}
