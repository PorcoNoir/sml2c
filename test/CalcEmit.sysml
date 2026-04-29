/*
 * CalcEmit — v0.22 regression test for calc-def → C function lowering.
 *
 * Three calc-def shapes:
 *   1. Trivial — only `in` parameters and a return expression.
 *   2. With intermediate `attribute` body — should lower to a local
 *      C variable initialized inline before the return.
 *   3. Calling another calc — lowers to a bare-name C function call.
 */
package CalcEmit {
    private import ScalarValues::*;

    /* Shape 1 — trivial.  Two `in` parameters, one return expression
     *           composed only of literals and parameter references. */
    calc def Square {
        in x : Real;
        return result : Real = x * x;
    }

    /* Shape 2 — intermediate attribute.  `scaled` becomes a local C
     *           variable in the lowered function body.  Type comes
     *           from the typechecker's inferredType (no `: Real` on
     *           the source line). */
    calc def Scale {
        in mass   : Real;
        in factor : Real;
        attribute scaled = mass * factor;
        return result : Real = scaled;
    }

    /* Shape 3 — calls another calc.  Lowers to bare-name C call.    */
    calc def SumOfSquares {
        in a : Real;
        in b : Real;
        return total : Real = Square(a) + Square(b);
    }
}
