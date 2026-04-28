package BadEnum {
    private import ScalarValues::*;

    enum def Color {
        red;
        green;
    }

    part def Lamp {
        // Error: assigning a Real literal to a Color-typed attribute.
        // (User-written; not inside the enum def, so the relaxation
        //  for value-tag literals doesn't apply.)
        attribute bad : Color = 3.14;
    }
}

