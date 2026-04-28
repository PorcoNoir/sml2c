package UnitsAndMetadata {
    private import ScalarValues::*;

    // Metadata applications (parse-and-skip in v0.5).
    #logical part def LogicalPart;
    #mop part def MOPPart;

    @Safety
    part def SafetyCriticalPart;

    @Safety { isMandatory = true; }
    part def MandatoryPart;

    part def Vehicle {
        // Unit suffixes on numeric initializers.
        attribute mass : Real = 75 [kg];
        attribute height : Real = 1.96 [m];
        attribute acceleration : Real = 1.96 [m/s**2];

        // Unit suffix on grouped expression.
        attribute total : Real = (1 + 2) [kg];

        // Metadata mid-body.
        #mop attribute weighted : Real;
    }
}
