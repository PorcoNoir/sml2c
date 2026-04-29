/* Hand-written test driver for test/CalcEmit.sysml.  Linked against
 * the C output of `sml2c --emit-c test/CalcEmit.sysml`.  Calls each
 * generated calc function with known inputs and prints results in a
 * stable format that test-c-run diffs against test/expected/CalcEmit.expect. */

#include <stdio.h>

double Square(double x);
double Scale(double mass, double factor);
double SumOfSquares(double a, double b);

int main(void) {
    printf("Square(3) = %g\n", Square(3.0));
    printf("Square(5) = %g\n", Square(5.0));
    printf("Scale(10, 2.5) = %g\n", Scale(10.0, 2.5));
    printf("SumOfSquares(3, 4) = %g\n", SumOfSquares(3.0, 4.0));
    return 0;
}
