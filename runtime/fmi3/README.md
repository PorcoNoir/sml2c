# Vendored FMI 3.0.2 headers and schemas

This directory contains the FMI 3.0.2 C headers and XSD schemas, copied
verbatim from <https://github.com/modelica/fmi-standard> tag `v3.0.2`.
They're vendored (not fetched at build time) so `--emit-fmu-c` output
builds offline and reproducibly.

`headers/` is what the generated `src/fmu.c` includes.  `schema/` is
used by the optional `make test-fmu-c` gate to validate generated XML
against the official schemas.

To upgrade: `git clone --depth 1 --branch <new-tag>
https://github.com/modelica/fmi-standard.git` and copy `headers/` and
`schema/` into here, then run `make sweep` to confirm nothing
regressed.

Source: https://github.com/modelica/fmi-standard
License: BSD 2-Clause (see LICENSE.txt)
