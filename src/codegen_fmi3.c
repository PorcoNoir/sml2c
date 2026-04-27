/* sml2c — codegen_fmi3.c
 *
 * Pretty-printed XML dump of the AST.  See codegen_fmi3.h.
 *
 * Design notes:
 *   - One emit function per NodeKind.  Each writes a JSON object
 *     whose first field is "kind" and whose remaining fields depend
 *     on the variant.
 *   - The files generated are the minimum components for an FMI3 compliant 
 *     model
 *.  - At minimum, the modelDescription.xml will be generated from which an 
 *     empty FMU can be generated.
 *.  - Need to link the AST for an FMU component with the AST of a SML2 comp
 */