/* Hand-written test driver for test/CalcEmit.sysml.  Linked against
 * the C output of `sml2c --emit-c test/CalcEmit.sysml`.  Uses the
 * SysML-named typedefs from sml2c-runtime.h (Real, Integer, …) so the
 * driver's signatures match the generated function prototypes
 * exactly — the C compiler would accept `double` here too (Real *is*
 * double, after typedef resolution), but using Real makes the
 * interop with generated code self-documenting. */

#include <stdio.h>
#include "sml2c-runtime.h"

Real Square(Real x);
Real Scale(Real mass, Real factor);
Real SumOfSquares(Real a, Real b);

int main(void) {
    printf("Square(3) = %g\n", Square(3.0));
    printf("Square(5) = %g\n", Square(5.0));
    printf("Scale(10, 2.5) = %g\n", Scale(10.0, 2.5));
    printf("SumOfSquares(3, 4) = %g\n", SumOfSquares(3.0, 4.0));
    return 0;
}
