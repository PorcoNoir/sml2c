#!/usr/bin/env python3
"""sml2c_to_graphsml.py — adapter from sml2c JSON to graphsml2 elements.

sml2c emits its AST as JSON via `sml2c --emit-json file.sysml`.  This
adapter reads that JSON and produces lightweight wrapper objects that
match the protocol `graphsml2.Classifier` expects on its inputs:

    obj.name                  → str, the element's local name
    obj.id                    → str, a stable unique identifier
    obj.children              → iterable of child wrappers (or absent)
    obj.usage_type_ref        → str, the resolved type name (for usages)
    obj.usage_kind            → str, the usage kind label
    obj.usage_direction       → str, "in" / "out" / "inout" / ""
    obj.direction             → str, alias for usage_direction
    obj.drawio_metadata()     → dict mapping graphsml2 keys to strings

The graphsml2 normalize_metadata() helper consults the methods/attrs
above and falls back to drawio_metadata() for everything else.  We
therefore produce metadata dicts with the keys it expects:

    sysml_type      "package", "part def", "port def", "attribute def",
                    "part", "port", "attribute", "connection", "flow", ...
    sysml_name      the local name
    sysml_id        a stable id (we use a path through the tree)
    sysml_abstract  "true" / "false"
    sysml_base      first specialization target name (or empty)

Usage example (with graphsml2 + drawpyo installed):

    from graphsml2 import Classifier, GSML2Object
    from sml2c_to_graphsml import load_program

    program = load_program("Connections.sysml.json")
    page = GSML2Object.create_page()
    classifier = Classifier()
    for top in program.children:
        classifier.render(top, page)

This module has zero external dependencies — graphsml2 itself is only
needed at render time, not at adapter time.  The adapter can also be
used standalone to inspect the converted tree.
"""
from __future__ import annotations

import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable


# --- mapping ---------------------------------------------------------------

# Map the JSON `defKind` field on definitions to graphsml2's sysml_type.
_DEF_TYPE = {
    "PartDef":         "part def",
    "PortDef":         "port def",
    "InterfaceDef":    "interface def",
    "ItemDef":         "item def",
    "ConnectionDef":   "connection def",
    "FlowDef":         "flow def",
    "EnumDef":         "enum def",
    "DataTypeDef":     "datatype def",
}

# Map JSON `defKind` on usages to graphsml2's sysml_type.  Note the
# spec talks about a single Usage class with a `kind` discriminator;
# graphsml2 uses the colloquial labels.
_USAGE_TYPE = {
    "PartDef":         "part",
    "PortDef":         "port",
    "InterfaceDef":    "interface",
    "ItemDef":         "item",
    "ConnectionDef":   "connection",
    "FlowDef":         "flow",
    "End":             "end",
    "EnumDef":         "enum value",
    "ReferenceUsage":  "reference",
    "DataTypeDef":     "datatype",
}


def _qname_string(qn: dict | None) -> str:
    """Render a JSON QualifiedName as `A::B::C`, with leading `~` if conjugated."""
    if not qn or not isinstance(qn, dict):
        return ""
    parts = qn.get("parts") or []
    prefix = "~" if qn.get("isConjugated") else ""
    return prefix + "::".join(parts)


def _qname_resolved(qn: dict | None) -> str:
    """Return the resolution target's plain name, or empty string."""
    if not qn or not isinstance(qn, dict):
        return ""
    return qn.get("resolvedTo") or ""


def _first_type_ref(types: list | None) -> dict | None:
    """The leading `:` reference, if any.  Lists are rare in our subset
    but we keep the multi-typed shape future-proof."""
    if not types: return None
    first = types[0]
    return first if isinstance(first, dict) else None


def _first_specialize(spec: list | None) -> str:
    """First `:>` target name, for the `sysml_base` slot."""
    if not spec: return ""
    return _qname_resolved(spec[0]) or _qname_string(spec[0])


# --- wrapper objects -------------------------------------------------------

@dataclass
class Element:
    """Base wrapper around a JSON node.  Matches the graphsml2 protocol."""
    name: str
    id: str
    sysml_type: str
    metadata: dict[str, str] = field(default_factory=dict)
    children: list["Element"] = field(default_factory=list)
    usage_type_ref: str = ""
    usage_kind: str = ""
    usage_direction: str = ""

    @property
    def direction(self) -> str:
        # graphsml2's normalize_metadata reads either name; we keep both.
        return self.usage_direction

    def drawio_metadata(self) -> dict[str, str]:
        # Always include the canonical fields graphsml2 looks for; user
        # extras (multiplicity etc.) live in self.metadata.
        m: dict[str, str] = {
            "sysml_name": self.name,
            "sysml_id":   self.id,
            "sysml_type": self.sysml_type,
        }
        if self.usage_type_ref:    m["sysml_usage_type_ref"]  = self.usage_type_ref
        if self.usage_kind:        m["sysml_usage_kind"]      = self.usage_kind
        if self.usage_direction:   m["sysml_usage_direction"] = self.usage_direction
        m.update(self.metadata)
        return m


@dataclass
class Program:
    """Top-level container — not itself a graphsml2 element, but holds
    the top-level packages and definitions you'd render."""
    children: list[Element] = field(default_factory=list)


# --- conversion ------------------------------------------------------------

def _path_id(prefix: str, name: str, idx: int) -> str:
    """Construct a stable id from the path to this node."""
    if name:
        return f"{prefix}::{name}" if prefix else name
    return f"{prefix}::#{idx}" if prefix else f"#{idx}"


def _convert_member(node: dict, parent_id: str, idx: int) -> Element | None:
    """Convert one JSON node into an Element wrapper.  Returns None for
    nodes that don't have a graphical representation (imports, doc,
    aliases, etc. — the user can still walk them via the Program if
    they want)."""
    kind = node.get("kind")

    if kind == "Package":
        elem = Element(
            name=node.get("name", ""),
            id=_path_id(parent_id, node.get("name", ""), idx),
            sysml_type="package",
        )
        for i, m in enumerate(node.get("members") or []):
            child = _convert_member(m, elem.id, i)
            if child: elem.children.append(child)
        return elem

    if kind == "Definition":
        sysml_type = _DEF_TYPE.get(node.get("defKind", ""), "definition")
        elem = Element(
            name=node.get("name", ""),
            id=_path_id(parent_id, node.get("name", ""), idx),
            sysml_type=sysml_type,
        )
        elem.metadata["sysml_abstract"] = "true" if node.get("isAbstract") else "false"
        base = _first_specialize(node.get("specializes"))
        if base: elem.metadata["sysml_base"] = base
        for i, m in enumerate(node.get("members") or []):
            child = _convert_member(m, elem.id, i)
            if child: elem.children.append(child)
        return elem

    if kind == "Usage":
        # Connection/flow usages without an explicit name are anonymous;
        # we synthesize an id so they still appear as graph elements.
        usage_kind = _USAGE_TYPE.get(node.get("defKind", ""), "usage")
        type_ref = _qname_resolved(_first_type_ref(node.get("types"))) or \
                   _qname_string(_first_type_ref(node.get("types")))
        elem = Element(
            name=node.get("name") or "",
            id=_path_id(parent_id, node.get("name") or "", idx),
            sysml_type=usage_kind,
            usage_type_ref=type_ref,
            usage_kind=usage_kind,
            usage_direction=node.get("direction") if node.get("direction") != "none" else "",
        )
        # Endpoint refs become metadata so a connector renderer can
        # find them; the actual edge drawing happens in the user's
        # render pass.
        ends = node.get("ends") or []
        if ends:
            for j, end in enumerate(ends):
                key = f"sysml_end_{j}"
                elem.metadata[key] = _qname_resolved(end) or _qname_string(end)
        for i, m in enumerate(node.get("members") or []):
            child = _convert_member(m, elem.id, i)
            if child: elem.children.append(child)
        return elem

    if kind == "Attribute":
        type_ref = _qname_resolved(_first_type_ref(node.get("types"))) or \
                   _qname_string(_first_type_ref(node.get("types")))
        elem = Element(
            name=node.get("name", ""),
            id=_path_id(parent_id, node.get("name", ""), idx),
            sysml_type="attribute def",
            usage_type_ref=type_ref,
            usage_kind="attribute",
        )
        # Attribute modifiers; relevant to graphsml2's row labels.
        for flag in ("isDerived", "isAbstract", "isConstant", "isReference"):
            if node.get(flag):
                elem.metadata[f"sysml_{flag.lower()}"] = "true"
        return elem

    # Aliases, comments, dependencies, imports, docs — pass through as
    # metadata-only Elements so they're discoverable but produce no
    # primary shape in graphsml2.  Most renderers filter on sysml_type
    # and skip these.
    if kind in {"Alias", "Comment", "Dependency", "Import", "Doc"}:
        name = node.get("name") or ""
        return Element(
            name=name,
            id=_path_id(parent_id, name, idx),
            sysml_type=kind.lower(),
        )

    # Unknown or expression-internal nodes — skip silently.
    return None


def from_json(data: dict) -> Program:
    """Convert a parsed sml2c --emit-json document into a Program tree."""
    if data.get("kind") != "Program":
        raise ValueError(f"Expected top-level kind 'Program', got {data.get('kind')!r}")
    program = Program()
    for i, m in enumerate(data.get("members") or []):
        child = _convert_member(m, "", i)
        if child: program.children.append(child)
    return program


def load_program(path: str | Path) -> Program:
    """Load an sml2c JSON file and convert it."""
    text = Path(path).read_text(encoding="utf-8")
    return from_json(json.loads(text))


# --- standalone CLI --------------------------------------------------------

def _walk_print(elem: Element, depth: int = 0) -> None:
    """Pretty-print the wrapper tree.  Useful for verifying the shape
    of the converted output without needing graphsml2 installed."""
    indent = "    " * depth
    bits = [f"<{elem.sysml_type}> '{elem.name}'"]
    if elem.usage_type_ref: bits.append(f": {elem.usage_type_ref}")
    if elem.usage_direction: bits.append(f"[{elem.usage_direction}]")
    if elem.metadata:
        keys = sorted(k for k in elem.metadata if k.startswith("sysml_"))
        if keys:
            extras = ", ".join(f"{k}={elem.metadata[k]}" for k in keys)
            bits.append(f"  /* {extras} */")
    print(indent + " ".join(bits))
    for c in elem.children:
        _walk_print(c, depth + 1)


def main(argv: list[str]) -> int:
    if len(argv) < 2 or argv[1] in {"-h", "--help"}:
        print(__doc__, file=sys.stderr)
        print("\nUsage: sml2c_to_graphsml.py <file.sysml.json>", file=sys.stderr)
        print("       sml2c_to_graphsml.py - < file.sysml.json", file=sys.stderr)
        return 0

    if argv[1] in {"-", "/dev/stdin"}:
        program = from_json(json.loads(sys.stdin.read()))
    else:
        program = load_program(argv[1])
    print(f"Program: {len(program.children)} top-level element(s)")
    for child in program.children:
        _walk_print(child, depth=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
