#!/usr/bin/env python3
"""render_drawio.py — produce a drawio file from sml2c JSON via graphsml2.

This script is the "wire it all together" piece.  It expects:

    pip install drawpyo graphsml2

(graphsml2 itself depends on `sml2.domain.port_def` for its parser
side, but the renderer side only needs drawpyo.  If your install
chain doesn't include sml2, the parser features won't be available
but the renderer will still work because it just calls
Classifier.render() on our wrapper objects.)

Usage:
    sml2c --emit-json input.sysml | python render_drawio.py - > out.drawio
    python render_drawio.py input.json out.drawio

The output is a drawio XML file you can open in app.diagrams.net or
draw.io desktop.

Layout: top-level elements are placed in a vertical stack.  The
adapter doesn't position children — graphsml2's renderers handle
their own internal layout.  A more polished version would do
typed-graph layout (Sugiyama, etc.); this script is intentionally
minimal so the render path is the main demonstration.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

from sml2c_to_graphsml import load_program, from_json


def render_to_drawio(program, out_path: str) -> None:
    """Walk the program and render each top-level child onto a page."""
    # Imports deferred so the script imports cleanly without graphsml2.
    import drawpyo
    from graphsml2 import Classifier, GSML2Object

    # A drawpyo File with a single Page.  Real applications would split
    # by package or by viewpoint — this is the simplest layout.
    drawio_file = drawpyo.File()
    drawio_file.file_path = str(Path(out_path).parent)
    drawio_file.file_name = Path(out_path).name
    page = drawpyo.Page(file=drawio_file)
    page.title = "sml2c output"

    classifier = Classifier()

    # Vertical stack of top-level elements.  graphsml2's renderers
    # don't currently handle inter-element layout; spacing here is
    # large enough that big classifier blocks don't collide.
    y = 40
    for elem in program.children:
        try:
            shape = classifier.render(elem, page, x=40, y=y)
        except Exception as exc:
            # Some element kinds (alias, comment, dependency, doc, import)
            # don't have a graphsml2 renderer — those produce wrappers
            # whose sysml_type isn't dispatched in classifier.render().
            # Skip silently rather than failing the whole emit.
            print(f"  skip {elem.sysml_type} '{elem.name}': {exc}",
                  file=sys.stderr)
            continue
        # Bumps based on graphsml2's known shape sizes.  Conservative.
        y += 320

    drawio_file.write()


def main(argv: list[str]) -> int:
    if len(argv) < 2 or argv[1] in {"-h", "--help"}:
        print(__doc__, file=sys.stderr)
        return 0

    if argv[1] == "-":
        data = json.load(sys.stdin)
        program = from_json(data)
        out = argv[2] if len(argv) > 2 else "out.drawio"
    else:
        program = load_program(argv[1])
        out = argv[2] if len(argv) > 2 else str(Path(argv[1]).with_suffix(".drawio"))

    render_to_drawio(program, out)
    print(f"wrote {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
