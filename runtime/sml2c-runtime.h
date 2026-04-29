/* sml2c-runtime.h — type definitions for sml2c-generated C code.
 *
 * The generated `.c` file from `sml2c --emit-c` includes this header
 * to pick up the C-level names for SysML's standard-library types.
 * Two layers:
 *
 *   1. Kernel Data Type Library — the language's core primitives
 *      (Real, Integer, Boolean, String, Number).  These are direct
 *      typedefs of C primitives.
 *
 *   2. SysML Domain-Specific Libraries — engineering quantities
 *      from ISQ (International System of Quantities), all currently
 *      aliasing Real.  The names preserve dimensional intent in the
 *      generated source even though the C type system can't enforce
 *      it (a Mass and a Force are both `double` to the C compiler).
 *
 * Tagged-struct lowering for full dimensional safety (Mass and Force
 * become distinct struct types so the C compiler rejects mixing
 * them) is a future option.  This header is the seam where that
 * change would land — the generator would not need to change.
 *
 * Also forward-declares the few standard library functions the
 * generator emits calls to (currently none; v0.25+ will add print
 * helpers and the `__sml2c_init` entry point).
 */
#ifndef SML2C_RUNTIME_H
#define SML2C_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

/* ---- Kernel Data Type Library ---- *
 *
 * The five primitive types every SysML program implicitly imports
 * via `ScalarValues::*`.  Mapping rationale:
 *
 *   Real     → double      (C's standard floating-point)
 *   Integer  → long long   (64-bit signed; matches SysML's spec
 *                           that Integer is unbounded in principle)
 *   Boolean  → bool        (from <stdbool.h>)
 *   String   → const char* (immutable string-literal pointer)
 *   Number   → double      (Real's superset; same C representation)
 */
typedef double      Real;
typedef long long   Integer;
typedef bool        Boolean;
typedef const char* String;
typedef double      Number;

/* ---- ISQ — International System of Quantities ---- *
 *
 * Standard SysML domain library for physics-derived measurements.
 * All alias Real for now.  Names match the SysML standard library
 * so source like `attribute mass : ISQ::MassValue;` lowers to a
 * field typed as `MassValue` in the emitted C.
 *
 * This list is intentionally not exhaustive — it covers the
 * quantities that appear in the project's reference models.  Adding
 * a new one is a single typedef line below.
 *
 * For the SysML aliases that engineering models commonly use
 * (e.g. `alias Torque for ISQ::TorqueValue;` in PTC), the alias
 * resolution at codegen time looks through to the underlying
 * MassValue / TorqueValue / etc., so we don't also need to typedef
 * the alias names here.  When the compiler does encounter a
 * user-defined alias whose target is in this header, the lowered C
 * uses the underlying name — a defensible choice that keeps the
 * header free of user-domain type pollution.
 */
typedef Real LengthValue;
typedef Real TimeValue;
typedef Real MassValue;
typedef Real ForceValue;
typedef Real EnergyValue;
typedef Real PowerValue;
typedef Real TorqueValue;
typedef Real SpeedValue;
typedef Real AccelerationValue;
typedef Real FrequencyValue;
typedef Real RotationalFrequencyValue;
typedef Real PressureValue;
typedef Real TemperatureValue;
typedef Real VoltageValue;
typedef Real CurrentValue;
typedef Real ResistanceValue;
typedef Real AreaValue;
typedef Real VolumeValue;
typedef Real DensityValue;
typedef Real AngleValue;

#endif /* SML2C_RUNTIME_H */
