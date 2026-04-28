// Regression test for v0.5: the referentialchecker pass infers
// isReference=true on attributes and on top-level usages, but the
// SysML emitter must not echo `ref` for those — only when the
// user explicitly wrote `ref` in source.
package InferenceFlags {
    private import ScalarValues::*;

    // Top-level usage without `ref` — referentialchecker infers
    // isReference=true (rule "no enclosing type → ref").
    requirement def MassReq;
    requirement vehicleMassReq : MassReq;

    // Top-level part with attributes — every attribute gets
    // isReference=true from rule #2.  Source has no `ref`.
    part def Vehicle {
        attribute mass : Real;
        attribute fuelMass : Real = 50;
    }

    // Action def parameters: direction-bearing usages get
    // isReference=true automatically.  Source has only `in`/`out`.
    action def Process {
        in input : Real;
        out output : Real;
    }

    // Explicit `ref` IS preserved.  This declaration roundtrips
    // with the keyword intact.
    part def Holder {
        ref attribute explicitlyRef : Real;
    }
}
