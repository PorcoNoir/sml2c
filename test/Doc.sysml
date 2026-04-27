/** Top-level documentation for the package, written in the implicit form. */
package DocDemo {
    private import ScalarValues::*;

    doc /* Standalone doc using the explicit keyword form,
         spanning multiple lines so we can confirm newlines
         survive the scanner. */

    /** Inline doc-comment style for the part def. */
    part def Engine {
        /** Doc on an attribute, implicit form. */
        attribute torque : Real;

        doc /* Doc on the next attribute, explicit form. */
        attribute power : Real;

        // a regular line comment that should still be discarded
        /* a regular block comment, also discarded */
        attribute idleRpm : Integer;
    }

    /** Doc on a part usage, implicit form. */
    part myEngine : Engine [3];
}
