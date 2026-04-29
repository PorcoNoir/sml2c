/*
 * FmuInterfaceMeta.fmu.sysml — v0.30 fixture for `--emit-fmu-c`.
 *
 * Exercises the F13 design item: `interface def` and `connection def`
 * declarations contribute <Annotations> metadata on the affected
 * Terminals.  Importers that recognize our annotation types can pair
 * Terminals across two FMUs that share a declared interface even
 * when both Terminals carry the generic conjugate matching rule.
 *
 * The fixture replicates the running Sleigh-Reindeer example from
 * design/fmu-c-codegen.md, with a single part def that participates
 * in a HarnessInterface (interface def) and is wired by a Harness
 * (connection def).  Both reference HarnessConnectionPoint as their
 * non-conjugate end, and ~HarnessConnectionPoint as their conjugate
 * end.
 *
 * Expected Terminal annotations:
 *
 *   <Annotations>
 *     <Annotation type="dev.sml2c.interface">
 *       <InterfaceRef defName="HarnessInterface" endName="eyelet" conjugated="false"/>
 *     </Annotation>
 *     <Annotation type="dev.sml2c.interface">
 *       <InterfaceRef defName="HarnessInterface" endName="harnessLoop" conjugated="true"/>
 *     </Annotation>
 *     <Annotation type="dev.sml2c.connection">
 *       <ConnectionRef defName="Harness" endName="eyelet" conjugated="false"/>
 *     </Annotation>
 *     <Annotation type="dev.sml2c.connection">
 *       <ConnectionRef defName="Harness" endName="harnessLoop" conjugated="true"/>
 *     </Annotation>
 *   </Annotations>
 *
 * The smoke test verifies the FMU still builds and reads back its
 * Binary variables — the F13 changes are pure metadata, so the
 * runtime contract is identical to v0.27's.  XSD validation against
 * fmi3TerminalsAndIcons.xsd confirms the annotation structure is
 * legal per the FMI 3.0 schema.
 */
package FmuInterfaceMeta {
    /* Port def — same as v0.27/Sleigh. */
    port def HarnessConnectionPoint {
        out item command  : Command;
        in  item feedback : Feedback;
    }

    item def Command;
    item def Feedback;

    /* Interface def — declares the abstract pairing of two ports.
     * Eyelet (non-conjugate) connects to harnessLoop (conjugate). */
    interface def HarnessInterface {
        end eyelet      : HarnessConnectionPoint;
        end harnessLoop : ~HarnessConnectionPoint;
    }

    /* Connection def — concrete realization of the interface.    */
    connection def Harness {
        end eyelet      : HarnessConnectionPoint;
        end harnessLoop : ~HarnessConnectionPoint;
    }

    /* Single part def — its `eyelet` port participates in both
     * HarnessInterface and Harness as the non-conjugate end.    */
    part def Sleigh {
        port eyelet : HarnessConnectionPoint;
    }
}
