package BadEnumMember {
    private import ScalarValues::*;

    enum def Color {
        red;
        green;
    }

    part def Lamp {
        // Error: 'purple' isn't a member of Color.
        attribute hue : Color = Color::purple;
    }
}
