/*
 * FmuFoundation.fmu.sysml — v0.25 fixture for `--emit-fmu-c`.
 *
 * Exercises the foundational lowering: a single outer part def
 * with one attribute of each kernel scalar type.  Each becomes
 * an FMI 3.0 parameter variable in the emitted modelDescription.xml,
 * a struct field in include/model.h, and an initializer in
 * src/model.c.
 *
 * The test driver (test/test_fmu.c, generated alongside the FMU)
 * calls fmi3InstantiateCoSimulation, transitions through Init,
 * reads each parameter's start value back, and free-instances.
 * That path covers: instantiation memory layout, FMI3_FUNCTION_PREFIX
 * symbol export, Float64/Int64/Boolean/String getter routing,
 * and the cmake build of vendored fmi3*.h headers.
 *
 * Ports, calc defs, constraints, conjugate types, items, and
 * connections are all out of scope for v0.25 (see design/fmu-c-codegen.md).
 */
package FmuFoundation {
    private import ScalarValues::*;

    part def Foundation {
        attribute mass     : Real    = 200.0;
        attribute capacity : Integer = 7;
        attribute label    : String  = "santa";
        attribute enabled  : Boolean = true;
    }
}
