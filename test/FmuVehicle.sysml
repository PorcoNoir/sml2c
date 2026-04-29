package Vehicle {
  private import ScalarValues::*;

  part def Engine {
    port def powerPort {
      out attribute power : Real;
    }
  }
  part def Brakes;
  part def Steering;
  part def Tire;
  
  part def PlantChassis {
    part engine : Engine;
    part brakes : Brakes;
    part steering : Steering;
    part tire : Tire;

  }
}