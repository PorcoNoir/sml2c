/* Test driver for test/InitEmit.sysml.  Calls the generated init
 * functions (Engine_init, __sml2c_init) and prints the resulting
 * field values + the L2 global to verify v0.23's lowering. */

#include <stdio.h>
#include "sml2c-runtime.h"

typedef struct {
    Real mass;
    Real torque;
    Real redline;
    Real massPlusTorque;
    Real headroom;
} Engine;

void Engine_init(Engine* self);
void __sml2c_init(void);
extern Real referenceSquare;

int main(void) {
    /* L2 path — top-level globals get initialized via __sml2c_init. */
    __sml2c_init();
    printf("referenceSquare = %g\n", referenceSquare);

    /* T_init path — populate Engine from declared defaults. */
    Engine e;
    Engine_init(&e);
    printf("Engine.mass = %g\n", e.mass);
    printf("Engine.torque = %g\n", e.torque);
    printf("Engine.redline = %g\n", e.redline);
    printf("Engine.massPlusTorque = %g\n", e.massPlusTorque);
    printf("Engine.headroom = %g\n", e.headroom);
    return 0;
}
