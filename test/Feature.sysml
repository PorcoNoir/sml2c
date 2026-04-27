/**
 * Feature.sysml  — Library Package
 *
 * Product Feature defined as a capability the product must perform.
 *
 * Changelog:
 *   [FIX-1] Removed duplicate `private import StateMachine::*`.
 *   [FIX-2] Added `private import OperationStateMachine::*` so that
 *            OperationSTD, OperationInterface, and OperationTerminator
 *            resolve without an implicit dependency on an unimported package.
 *   [FIX-3] `BasicFeatureOperationSTD` promoted to `abstract state def`
 *            because its members (usedInterfaces, participatingTerminators)
 *            remain abstract; a concrete state def may not have abstract members.
 *   [ADD-1] `Feature` now carries `dfdNodes` and `dfdFlows` attribute arrays so
 *            that tooling can traverse the DFD hierarchy from the part tree.
 *   [ADD-2] `FunctionalReq.operationMode` retyped from String to OperationMode
 *            so it aligns with the OperationStateMachine enum and enables
 *            model-level consistency checks.
 *
 * Package dependencies (import order):
 *   SysML stdlib  →  OperationStateMachine  →  Feature
 *   Feature is consumed by Feature-usage packages (Vehicle, User, …).
 */

package Feature {
    doc
    /*
     * Product Feature defined as a capability the product must perform.
     *
     * Architecture note:
     *   This library contains DEFINITIONS only (part def, interface def,
     *   requirement def, state def).  Feature USAGES (concrete part usages
     *   typed by these defs) live in separate Feature-usage packages
     *   (e.g., Vehicle, User) per SysML v2 Definition/Usage separation.
     * Rules
     * https://docs.sensmetry.com/about/rules/
     * TODO: Split into FeatureDefinition and FeatureUsage packages to
     *       fully align with SysML v2 §5.3 Definition/Usage semantics.
     */

    // ─────────────────────────────────────────────────────────────────────────
    // Standard library imports
    // ─────────────────────────────────────────────────────────────────────────
    private import ScalarValues::*;
    private import Requirements::*;
    private import Parts::Part;
    private import Actions::Action;
    private import StateMachine::*;                  // SysML stdlib state machine

    // ─────────────────────────────────────────────────────────────────────────
    // OperationStateMachine import  [FIX-2]
    // Provides: OperationSTD, OperationInterface, OperationTerminator,
    //           ControlSignal hierarchy, FaultSeverity, OperationMode,
    //           ControlFlowInterface, ModeFeedbackInterface, DiagnosticInterface,
    //           DFDNode, DFDFlow
    // ─────────────────────────────────────────────────────────────────────────
    private import OperationStateMachine::*;

    // =========================================================================
    // FEATURE DEFINITION
    // =========================================================================

    abstract part def Feature :> Part {
        doc
        /*
         * Feature is the most general class of engineering features representing
         * end-to-end customer-visible functionality.
         *
         * A Feature corresponds to a single process bubble at DFD0 (context
         * diagram level). Its internal decomposition is captured in child Features
         * and DFDNode metadata, forming the DFD1..DFDn hierarchy.
         *
         * Binding contract for concrete specializations:
         *   - externalInterfaces SHALL be bound to ≥ 1 FunctionalInterface usage.
         *   - featureTerminators SHALL be bound to ≥ 1 Terminator usage.
         *   - featureOperation SHALL be bound to exactly one OperationSTD usage.
         */

        // ── Identity ─────────────────────────────────────────────────────────
        attribute featureId   : String;
        attribute featureName : String;

        // ── Requirements allocated to this feature (FTS "shall"s) ─────────────
        abstract requirement allocatedRequirements        : FeatureRequirement[0..*];
        abstract requirement performanceRequirements      : FeaturePerformanceReq[0..*]
            :> allocatedRequirements;
        abstract requirement functionalRequirements       : FunctionalReq[0..*]
            :> allocatedRequirements;
        abstract requirement internalInterfaceRequirements: InternalInterfaceReq[0..*]
            :> allocatedRequirements;
        abstract requirement nonFunctionalRequirements    : NonFunctionalReq[0..*]
            :> allocatedRequirements;

        // ── External interfaces (DFD0 context-level data flows) ───────────────
        abstract interface externalInterfaces : FunctionalInterface[1..*];

        // ── Internal interfaces (DFD1+ function-to-function data flows) ───────
        abstract interface internalInterfaces : FunctionalInterface[0..*];

        // ── Terminators at feature boundary (DFD0 context rectangles) ─────────
        abstract part featureTerminators : Terminator[1..*];

        // ── Feature operation state machine (one STD per feature) ─────────────
        //    The OperationSTD is the Ward-Mellor CSPEC for this feature.
        //    Concrete specializations SHALL bind this to a BasicFeatureOperationSTD
        //    subtype (or a more specific OperationSTD) with all abstract members
        //    resolved.
        abstract state featureOperation : OperationSTD[1..1];

        // ── DFD hierarchy metadata  [ADD-1] ───────────────────────────────────
        //    dfdRootLevel: label of the context-diagram node (always "DFD0").
        //    dfdMaxDepth:  maximum decomposition depth reached in DFDn hierarchy.
        //    dfdNodes:     ordered descriptor array for every DFD process node
        //                  owned by this feature (tooling traversal anchor).
        //    dfdFlows:     descriptor array for every data/control flow arc
        //                  within this feature's DFD scope.
        attribute dfdRootLevel : String[0..1];        // expected: "DFD0"
        attribute dfdMaxDepth  : Integer[0..1];
        abstract attribute dfdNodes : DFDNode[0..*];  // populated by usage packages
        abstract attribute dfdFlows : DFDFlow[0..*];  // populated by usage packages
    }

    // ── Root feature property (analogous to Parts::parts) ────────────────────
    abstract part features : Feature[0..*] nonunique :> Parts::parts {
        doc
        /*
         * `features` is the base feature-usage property of all feature
         * sub-properties, exactly as `Parts::parts` is the base property for
         * all part usages in SysML v2.
         *
         * Usage-packages reference this property when listing child features
         * inside a Feature specialization body.
         */
    }

    // =========================================================================
    // PORT DEFINITIONS
    // =========================================================================

    abstract port def TerminatorInterface {
        doc
        /*
         * Port end at a Terminator.  Provides the typed connection point through
         * which FunctionalInterface flows and ControlFlowInterface signals cross
         * the feature boundary.
         *
         * Concrete specializations SHALL define conjugate ports for each
         * FunctionalInterface direction (IN / OUT / INOUT) used by the terminator.
         */
    }

    // =========================================================================
    // INTERFACE DEFINITIONS — Data-flow layer (DFD flows)
    //
    // These types carry payload data on the DFD.  They are orthogonal to
    // OperationInterface / ControlFlowInterface (control-flow layer).
    // The join type BasicFeatureOperationInterface bridges both layers.
    // =========================================================================
    abstract item def SignalItem {
        doc
        /*
         * SignalFlow is the most general type of flow on the DFD.  It is the
         * base type for all FlowTransfers and FlowUsages.
         *
         * A FlowTransfer is a flow that has a source event and target event that
         * define its temporal relationship to the OperationSTD execution flow.
         * A FlowUsage is a flow that identifies its source output and target input
         * at the DFD0 context level.
         */
        attribute name        : String;
        attribute description : String;

        // Optional timing / signal-quality attributes
        attribute refreshRate : ScalarValue[0..1];
        attribute latency     : ScalarValue[0..1];
        attribute units       : String[0..1];
    }

    abstract interface def FunctionalInterface {
        doc
        /*
         * A named functional flow exchanged between features and terminators.
         * Corresponds to a labeled data-flow arc in the DFD.
         *
         * Rules (MBSE / Ward-Mellor):
         *   1. Provide a name and description at minimum.
         *   2. Omit optional attributes if not required by allocated requirements.
         *   3. Interface names shared across features MUST be coordinated and
         *      registered in the project interface dictionary.
         *   4. FunctionalInterface SHALL NOT carry ControlSignal values
         *      (Ward-Mellor CSPEC/PSPEC separation — Rule [R3]).
         */
        attribute name        : String;
        attribute description : String;
    }

    abstract item def DiscreteItem :> SignalItem {
        doc
        /*
         * Discrete interface — mode, state, status, or command flows whose
         * values are selected from a finite named set.
         * Typical use: operation mode requests, on/off commands, fault status.
         */
        attribute states       : String[0..*];
        attribute defaultState : Integer[0..1] || String[0..1]; // can be integer or enumerated string
    }

    abstract item def ProportionalItem :> SignalItem {
        doc
        /*
         * Proportional interface — bounded numeric flows that vary in proportion
         * to a physical quantity or command.
         * Typical use: torque request, pedal position, steering angle.
         */
        attribute accuracy     : ScalarValue[0..1];
        attribute hysteresis   : ScalarValue[0..1];
        attribute minimumValue : ScalarValue[0..1];
        attribute maximumValue : ScalarValue[0..1];
        attribute resolution   : ScalarValue[0..1];
        attribute undershoot   : ScalarValue[0..1];
        attribute settlingTime : ScalarValue[0..1];
        attribute initialValue : ScalarValue[0..1];
        attribute defaultValue : ScalarValue[0..1];
    }

    abstract item def ContinuousItem :> SignalItem {
        doc
        /*
         * Continuous interface — continuously varying numeric flows where
         * discrete state enumeration is not meaningful.
         * Typical use: vehicle speed, yaw rate, battery voltage.
         */
        attribute accuracy     : ScalarValue[0..1];
        attribute hysteresis   : ScalarValue[0..1];
        attribute minimumValue : ScalarValue[0..1];
        attribute maximumValue : ScalarValue[0..1];
        attribute resolution   : ScalarValue[0..1];
        attribute initialValue : ScalarValue[0..1];
        attribute defaultValue : ScalarValue[0..1];
    }

    // =========================================================================
    // REQUIREMENT DEFINITIONS
    // =========================================================================

    abstract requirement def FeatureRequirement {
        doc
        /*
         * Base requirement type.  All FTS "shall" statements specialize this.
         *
         * Attributes follow NASA best-practices grammar:
         *   "<Subject> shall <Verb> <Object> [<Constraint>] [<Condition>]"
         * Reference: NASA.gov/seh/appendix-a-best-practices-for-writing-requirements
         */
        attribute requirementId    : String[0..1];
        attribute fReqSubject      : String;
        attribute fReqVerb         : String;
        attribute fReqObject       : String;
        attribute fReqConstraint   : String;    // negotiated / verified value
        attribute fReqCondition    : String;
        attribute isSafetyRelevant : Boolean[0..1];
    }

    abstract requirement def FeaturePerformanceReq :> FeatureRequirement {
        doc
        /*
         * End-to-end, black-box, measurable performance requirement.
         * Observable and verifiable with non-intrusive instrumentation.
         */
    }

    abstract requirement def FunctionalReq :> FeatureRequirement {
        doc
        /*
         * Functional requirement — defines the transformation of input flows
         * into output flows by DFD processes (DFD0, DFD1, DFD1.1, …).
         *
         * Captures behavior for: normal operation, degraded operation,
         * failed operation, and (for safety-critical features) safe-state operation.
         *
         * operationMode  [FIX — was String, now OperationMode enum]:
         *   Links this requirement to a specific STD state so that DFD process
         *   nodes can filter requirements by active mode.  Uses OperationMode
         *   from OperationStateMachine to ensure model consistency.
         */
        attribute functionId      : String[0..1];
        attribute dfdLevel        : String[0..1];          // e.g., "DFD1.2"
        attribute operationMode   : OperationMode[0..1];   // NORMAL | DEGRADED | FAILSAFE …
        attribute safetyGoalRef   : String[0..1];
        attribute hazardMetricRef : String[0..1];
        attribute fttiConstraint  : String[0..1];
    }

    abstract requirement def InternalInterfaceReq :> FeatureRequirement {
        doc
        /*
         * Internal interface requirement — constraints on function-to-function
         * interfaces within the feature boundary (not context-level).
         */
        attribute sourceFunctionId      : String[0..1];
        attribute destinationFunctionId : String[0..1];
        attribute interfaceId           : String[0..1];
    }

    abstract requirement def NonFunctionalReq :> FeatureRequirement {
        doc
        /*
         * Non-functional requirement — feature-wide quality constraints
         * (appearance, availability, functional reliability) that apply
         * across all DFD processes.
         */
        attribute appliesToWholeFeature : Boolean[0..1];
    }

    abstract requirement def FunctionalReliabilityReq :> FeatureRequirement {
        doc
        /*
         * Functional reliability requirement — ability of a feature or
         * component to perform its function over a defined operational life.
         * May be classified under FunctionalReq (4.1) or NonFunctionalReq (4.3)
         * depending on scope.
         */
        attribute reliabilityMeasure : String[0..1];
        attribute missionProfile     : String[0..1];
    }

    // =========================================================================
    // TERMINATOR DEFINITIONS
    // =========================================================================

    abstract part def Terminator {
        doc
        /*
         * External entity at the DFD0 context boundary.
         * Terminators are the rectangles on the DFD context diagram — they
         * exist outside the feature boundary and exchange FunctionalInterface
         * flows and ControlSignals with the feature.
         */
    }

    abstract part def FeatureTerminator :> Terminator {
        doc
        /*
         * Terminator that is another feature in the system feature tree.
         * FeatureTerminator represents allowable feature-to-feature interactions
         * at the DFD0 boundary (e.g., torque request from AWD to Propel,
         * traction control active status to Cruise Control).
         *
         * Modeling rule: FeatureTerminator usages SHALL reference an existing
         * Feature part def in the feature registry.  The interaction interface
         * (FunctionalInterface subtype) must be coordinated between both features.
         */
    }

    // =========================================================================
    // FEATURE OPERATION — STD ↔ DFD bridge types
    //
    // These definitions live in Feature (not OperationStateMachine) because they
    // join Feature-domain types (FunctionalInterface, FeatureTerminator) with
    // OperationStateMachine-domain types (OperationInterface, OperationTerminator).
    // The join must happen in a package that imports both libraries.
    // =========================================================================

    abstract interface def BasicFeatureOperationInterface
            :> OperationInterface, FunctionalInterface {
        doc
        /*
         * Join type: carries both a control-plane OperationInterface role and
         * a data-plane FunctionalInterface role across the feature boundary.
         *
         * Use this when a single interface arc on DFD0 simultaneously represents
         * a discrete control flow (mode command / status) AND a named data flow
         * (discrete state value).  For pure control flows use ControlFlowInterface;
         * for pure data flows use a FunctionalInterface subtype directly.
         */
    }

    abstract part def BasicFeatureOperationTerminator
            :> OperationTerminator, FeatureTerminator {
        doc
        /*
         * Join type: an external entity that participates in the OperationSTD
         * (sends/receives ControlSignals) AND is a FeatureTerminator in the
         * DFD0 context diagram.
         *
         * Most Vehicle and User terminator usages will specialize this type.
         */
    }

    abstract state def BasicFeatureOperationSTD :> OperationSTD {
        doc
        /*
         * Minimal concrete-ready STD for a feature.  [FIX-3: now abstract]
         *
         * Remains abstract because usedInterfaces and participatingTerminators
         * are abstract — concrete feature usage packages MUST bind them to
         * typed interface and terminator usages before instantiation.
         *
         * Inheritance: specializes OperationSTD (which provides the seven
         * canonical states OFF / INITIALIZING / STANDBY / ACTIVE / DEGRADED /
         * FAILSAFE / SHUTDOWN and all standard transitions).
         *
         * Concrete specialization pattern (in a usage package):
         *
         *   state def MyFeatureSTD :> BasicFeatureOperationSTD {
         *       interface usedInterfaces : MyFeatureOperationInterface[1..*];
         *       part participatingTerminators : MyFeatureTerminator[1..*];
         *       // Override entry/exit actions for each of the seven states
         *   }
         */

        // ── Minimum interface requirement (≥ 1 operation interface) ──────────
        abstract interface usedInterfaces : BasicFeatureOperationInterface[1..*];

        // ── Minimum terminator requirement (≥ 1 terminator) ──────────────────
        abstract part participatingTerminators : BasicFeatureOperationTerminator[1..*];
    }

}
