/* Test driver for test/ConstraintEmit.sysml.  Exercises T_check on
 * default-state structs (should pass) and on deliberately-violated
 * structs (should fail with stderr message + bool false return).
 *
 * `make test-c-run` runs us with `2>&1`, so the golden file captures
 * both stdout (our printf) and stderr (T_check's failure messages)
 * interleaved in source order — but only after stream flushing,
 * which `setvbuf(_IONBF)` forces line-by-line.  Without unbuffered
 * stdout, our printf might appear in the wrong order relative to
 * fprintf(stderr) in the merged stream.                             */

#include <stdio.h>
#include "sml2c-runtime.h"

typedef struct {
    Real mass;
    Real torque;
    Real redline;
} Engine;

typedef struct {
    Real voltage;
    Real soc;
} Battery;

void Engine_init(Engine* self);
void Battery_init(Battery* self);
Boolean Engine_check(const Engine* self);
Boolean Battery_check(const Battery* self);

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* (1) — defaults pass.  No stderr output expected. */
    Engine e;
    Engine_init(&e);
    printf("Engine defaults: %s\n", Engine_check(&e) ? "pass" : "fail");

    Battery b;
    Battery_init(&b);
    printf("Battery defaults: %s\n", Battery_check(&b) ? "pass" : "fail");

    /* (2) — sane_mass violation. */
    e.mass = -5.0;
    printf("Engine after mass=-5: %s\n", Engine_check(&e) ? "pass" : "fail");
    e.mass = 200.0;     /* restore */

    /* (3) — sane_torque violation. */
    e.torque = 0.0;
    printf("Engine after torque=0: %s\n", Engine_check(&e) ? "pass" : "fail");
    e.torque = 350.0;     /* restore */

    /* (4) — anonymous redline violation. */
    e.redline = 15000.0;
    printf("Engine after redline=15000: %s\n", Engine_check(&e) ? "pass" : "fail");
    e.redline = 7000.0;   /* restore */

    /* (5) — Battery violation. */
    b.voltage = 5.0;
    printf("Battery after voltage=5: %s\n", Battery_check(&b) ? "pass" : "fail");

    return 0;
}
