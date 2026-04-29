/* Test driver for test/Thermostat.sysml.  Exercises whatever lowers
 * in the current version and is intended to grow as more constructs
 * become emittable.
 *
 * v0.23.1 — calls ResponsePower with three (error, gain) pairs.
 * v0.24+   — will add Controller_init + Controller_check exercises.
 * v0.25+   — will add Mode_step exercises. */

#include <stdio.h>
#include "sml2c-runtime.h"

Real ResponsePower(Real error, Real gain);

int main(void) {
    printf("ResponsePower(2, 100) = %g\n", ResponsePower(2.0, 100.0));
    printf("ResponsePower(-1, 50) = %g\n", ResponsePower(-1.0, 50.0));
    printf("ResponsePower(0, 100) = %g\n", ResponsePower(0.0, 100.0));
    return 0;
}
