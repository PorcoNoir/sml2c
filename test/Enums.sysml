package EnumDemo {
    private import ScalarValues::*;

    // Simple enum definition.
    enum def Color {
        red;
        green;
        blue;
    }

    // Enum with explicit values via default-value expressions.
    enum def TrafficLightColor {
        stop = 0;
        caution = 1;
        go = 2;
    }

    // Enum used as the type of a part attribute.  The default value
    // references an enum value via qualified name; the resolver finds
    // it as a member of Color, and typechecking accepts since both
    // sides resolve to Color-typed nodes.
    part def Lamp {
        attribute hue : Color = Color::red;
        attribute brightness : Real;
    }

    // Enum specialization — narrowing the value set in a subtype.
    enum def WarmColor :> Color {
        red;
    }
}

