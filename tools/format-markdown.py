#!/usr/bin/env python3

"""Conservatively wrap prose in CoW's Markdown sources."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys
import textwrap


WIDTH = 79
LIST_ITEM = re.compile(r"^(\s*(?:[-+*]|[0-9]+[.)])\s+)(.*)$")
METADATA = re.compile(r"(?:title|section|date|source|volume):\s")
ATOMIC_SPAN = re.compile(r"!?\[[^]\n]*\]\([^\n)]*\)|`+[^`\n]+`+")
EXCLUDED_DIRS = {".git", "build", "public", "subprojects"}


def is_structural(line: str, in_metadata: bool) -> bool:
    """Return true when reflowing the line could change Markdown semantics."""

    if not line or line[0].isspace():
        return True
    if in_metadata and METADATA.match(line):
        return True
    if line.startswith(("#", "|", ":", "<!--", "```", "~~~")):
        return True
    if line in ("---", "***", "___"):
        return True
    return False


def wrap_prefixed(prefix: str, text: str) -> list[str]:
    """Wrap text while retaining a Markdown marker as a hanging indent."""

    marker = "\ue000"
    protected = ATOMIC_SPAN.sub(
        lambda match: match.group(0).replace(" ", marker), text
    )
    protected = re.sub(r"[ \t]+", " ", protected).strip()
    wrapped = textwrap.wrap(
        protected,
        width=WIDTH,
        initial_indent=prefix,
        subsequent_indent=" " * len(prefix),
        break_long_words=False,
        break_on_hyphens=False,
    )
    return [line.replace(marker, " ") for line in wrapped]


def format_markdown(source: str) -> str:
    lines = source.splitlines()
    result: list[str] = []
    in_metadata = True
    in_fence = False
    frontmatter = lines[0] if lines and lines[0] in ("---", "+++") else None
    i = 0

    while i < len(lines):
        line = lines[i]

        if frontmatter is not None:
            result.append(line)
            i += 1
            if i > 1 and line == frontmatter:
                frontmatter = None
                in_metadata = False
            continue

        if line.startswith(("```", "~~~")):
            in_fence = not in_fence
            result.append(line)
            i += 1
            continue

        if not line:
            in_metadata = False
            result.append(line)
            i += 1
            continue

        if in_fence or is_structural(line, in_metadata):
            result.append(line)
            i += 1
            continue

        item = LIST_ITEM.match(line)
        if item is not None:
            prefix, text = item.groups()
            i += 1
            continuation = " " * len(prefix)
            while i < len(lines):
                line = lines[i]
                if (
                    not line.startswith(continuation)
                    or line.startswith(continuation + " ")
                    or LIST_ITEM.match(line)
                ):
                    break
                text += " " + line.strip()
                i += 1
            result.extend(wrap_prefixed(prefix, text))
            continue

        if line.startswith(": "):
            if len(line) > WIDTH:
                result.extend(wrap_prefixed(": ", line[2:]))
            else:
                result.append(line)
            i += 1
            continue

        paragraph = [line]
        i += 1
        while i < len(lines):
            line = lines[i]
            if is_structural(line, in_metadata):
                break
            paragraph.append(line)
            i += 1

        is_definition_term = (
            i + 1 < len(lines)
            and not lines[i]
            and lines[i + 1].startswith(": ")
        )
        if is_definition_term:
            result.extend(paragraph)
            continue

        # A trailing backslash requests a hard line break.  Preserve every
        # line in such a block rather than guessing where it may be moved.
        if any(part.endswith("\\") for part in paragraph):
            result.extend(paragraph)
            continue

        text = " ".join(part.strip() for part in paragraph)
        result.extend(wrap_prefixed("", text))

    return "\n".join(result) + "\n"


def default_paths() -> list[Path]:
    paths = set(Path(".").rglob("*.md"))
    paths.update(Path(".").rglob("*.md.in"))
    return sorted(
        path
        for path in paths
        if not EXCLUDED_DIRS.intersection(path.parts)
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="wrap prose in repository Markdown sources"
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="report files which need formatting without changing them",
    )
    parser.add_argument("files", nargs="*", type=Path)
    args = parser.parse_args()

    paths = args.files or default_paths()
    changed = []
    for path in paths:
        source = path.read_text(encoding="utf-8")
        formatted = format_markdown(source)
        if source == formatted:
            continue
        changed.append(path)
        if not args.check:
            path.write_text(formatted, encoding="utf-8")

    if args.check and changed:
        for path in changed:
            print(f"needs formatting: {path}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
