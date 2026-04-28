package CalcDemo {
    private import ScalarValues::*;

    // Simplest calc def: parameters in, return value out.
    calc def Multiply {
        in a : Real;
        in b : Real;
        return result : Real = a * b;
    }

    // Calc with intermediate attributes in the body.
    calc def Compute {
        in mass : Real;
        in factor : Real;
        attribute scaled = mass * factor;
        return result : Real = scaled + 1;
    }

    // Calc that calls another calc.
    calc def Combined {
        in x : Real;
        in y : Real;
        return total : Real = Multiply(x, y) + Multiply(x, x);
    }

    // Calls inside attribute initializers.
    part def Vehicle {
        attribute mass : Real;
        attribute factor : Real;
        attribute computed : Real = Compute(mass, factor);
        attribute combined : Real = Combined(mass, factor);
    }
}
