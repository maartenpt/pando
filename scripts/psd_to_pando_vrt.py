#!/usr/bin/env python3
"""
Convert Penn Treebank–style bracketed trees (PSD / S-expression) to Pando VRT.

Emits CWB-style sentence tags and optional nested <node type="…"> regions compatible with
CorpusBuilder::read_vertical.

**Preterminals** (e.g. (NN parsing)) can be encoded in two ways:

- **Default:** each preterminal is a region: <node type="NN"> around the token line.
  One column per token; POS appears only as node_type on that region.

- **--as-pos:** preterminals are not wrapped in <node>; the PTB tag is the second column
  (tab-separated: word, then tag). The VRT header declares word pos; pando-index stores
  the tag as xpos. Phrasal nodes (NP, VP, S, …) still use nested <node> regions.

A Kielipankki-style <!-- #vrt positional-attributes: … --> line is written at the top
unless --no-vrt-header is set.

Usage:
  python scripts/psd_to_pando_vrt.py [input.psd] [output.vrt]
  python scripts/psd_to_pando_vrt.py --as-pos trees.psd out.vrt
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, field
from typing import List, Tuple, Union


@dataclass
class Node:
    label: str
    children: List[Union["Node", str]] = field(default_factory=list)


def xml_escape_attr(s: str) -> str:
    return (
        s.replace("&", "&amp;")
        .replace('"', "&quot;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
    )


def tokenize_psd(text: str) -> List[str]:
    """Split PSD into '(', ')', and atoms (no whitespace inside atoms)."""
    out: List[str] = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
            continue
        if c in "()":
            out.append(c)
            i += 1
            continue
        j = i
        while j < n and not text[j].isspace() and text[j] not in "()":
            j += 1
        out.append(text[i:j])
        i = j
    return out


def parse_node(tokens: List[str], i: int) -> Tuple[Node, int]:
    if i >= len(tokens) or tokens[i] != "(":
        raise ValueError(f"expected '(' at token index {i}")
    i += 1
    if i >= len(tokens):
        raise ValueError("truncated tree: missing label after '('")
    label = tokens[i]
    i += 1
    if i >= len(tokens):
        raise ValueError("truncated tree: missing body")

    if tokens[i] == ")":
        return Node(label, []), i + 1

    if tokens[i] == "(":
        children: List[Union[Node, str]] = []
        while tokens[i] != ")":
            child, i = parse_node(tokens, i)
            children.append(child)
        return Node(label, children), i + 1

    word = tokens[i]
    i += 1
    if i >= len(tokens) or tokens[i] != ")":
        raise ValueError(f"expected ')' after leaf token {word!r}")
    return Node(label, [word]), i + 1


def parse_forest(text: str) -> List[Node]:
    """Parse one or more top-level bracketed trees; ignores junk between trees."""
    tokens = tokenize_psd(text)
    trees: List[Node] = []
    i = 0
    while i < len(tokens):
        if tokens[i] == "(":
            tree, i = parse_node(tokens, i)
            trees.append(tree)
        else:
            i += 1
    return trees


def is_preterminal(node: Node) -> bool:
    return len(node.children) == 1 and isinstance(node.children[0], str)


def vrt_positional_attributes_header(as_pos: bool) -> str:
    """Kielipankki line; must match token columns (pos → xpos in builder.cpp)."""
    names = "word pos" if as_pos else "word"
    return f"<!-- #vrt positional-attributes: {names} -->"


def emit_vrt(node: Node, lines: List[str], as_pos: bool) -> None:
    if is_preterminal(node):
        word = node.children[0]
        assert isinstance(word, str)
        form = word if word else "_"
        tag = node.label
        if as_pos:
            lines.append(f"{form}\t{tag}")
        else:
            lines.append(f'<node type="{xml_escape_attr(tag)}">')
            lines.append(form)
            lines.append("</node>")
        return

    if not node.children:
        return

    lines.append(f'<node type="{xml_escape_attr(node.label)}">')
    for ch in node.children:
        if isinstance(ch, str):
            raise ValueError("internal node with bare string child (malformed tree)")
        emit_vrt(ch, lines, as_pos)
    lines.append("</node>")


def tree_to_vrt(tree: Node, as_pos: bool) -> str:
    lines: List[str] = ["<s>"]
    emit_vrt(tree, lines, as_pos)
    lines.append("</s>")
    lines.append("")
    return "\n".join(lines)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "input",
        nargs="?",
        type=argparse.FileType("r", encoding="utf-8"),
        default=sys.stdin,
        help="PSD file (default: stdin)",
    )
    ap.add_argument(
        "output",
        nargs="?",
        type=argparse.FileType("w", encoding="utf-8"),
        default=sys.stdout,
        help="VRT file (default: stdout)",
    )
    ap.add_argument(
        "--as-pos",
        action="store_true",
        help="encode PTB preterminals as word+POS columns, not <node> regions",
    )
    ap.add_argument(
        "--no-vrt-header",
        action="store_true",
        help="omit the <!-- #vrt positional-attributes: ... --> line",
    )
    args = ap.parse_args()
    text = args.input.read()
    trees = parse_forest(text)
    if not trees:
        print("psd_to_pando_vrt: no bracketed trees found", file=sys.stderr)
        sys.exit(1)
    parts: List[str] = []
    if not args.no_vrt_header:
        parts.append(vrt_positional_attributes_header(args.as_pos))
        parts.append("")
    parts.extend(tree_to_vrt(t, args.as_pos) for t in trees)
    args.output.write("\n".join(parts))


if __name__ == "__main__":
    main()
