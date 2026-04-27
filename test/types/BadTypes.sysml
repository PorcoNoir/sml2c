package BadTypes {
    private import ScalarValues::*;

    part def X {
        // String literal can't fit an Integer attribute
        attribute count : Integer = "five";

        // Real default doesn't specialize Integer
        attribute steps : Integer = 3.14;

        // Boolean default doesn't fit String
        attribute label : String = true;

        // Adding a String and an Integer
        attribute mash : Integer = "hi" + 1;

        // Comparing a Boolean with <
        attribute weirdCmp : Boolean = true < false;

        // Equality on unrelated types
        attribute mismatch : Boolean = "hi" == 7;

        // Bang on a non-Boolean
        attribute nonBoolBang : Boolean = !"hello";

        // Unary minus on a String
        attribute negStr : Integer = -"x";
    }
}
