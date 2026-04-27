package DependencyDemo {
    private import ScalarValues::*;

    part def Engine { }
    part def Transmission { }
    part def Wheel { }
    part def Brake { }

    // Anonymous dependency: single source to single target.
    dependency from Engine to Transmission;

    // Named dependency, multiple sources to multiple targets.
    dependency drivetrain from Engine, Transmission to Wheel, Brake;

    // Visibility modifier on a dependency.
    private dependency internalLink from Engine to Transmission;
}
