#!/usr/bin/env python3
"""Lift a source file out of the originating repository into anvil.

Mechanical only. It renames the namespace, remaps include paths and doc/rule
citations, and strips finding numbers that would dangle. Everything it cannot do
safely it REPORTS rather than guesses at, so the reviewer sees exactly what still
needs a human decision.

The rule the task ledger states: a comment citing a finding number by itself is
worse than no citation, because the register it points into does not exist here.
Where the surrounding sentence already carries the reasoning — which is the usual
case — the bare marker is dropped and nothing is lost.
"""
import re
import sys
from pathlib import Path

MODULES = {
    "lib/core": "anvil/core",
    "lib/crypto": "anvil/crypto",
    "lib/i18n": "anvil/i18n",
    "lib/inputvalidation": "anvil/input",
    "lib/auth": "anvil/auth",
    "lib/config": "anvil/config",
    "lib/http": "anvil/http",
    "lib/filesystem": "anvil/fs",
    "lib/db": "anvil/db",
    "lib/redis": "anvil/redis",
    "lib/images": "anvil/images",
    "lib/accesscontrol": "anvil/accesscontrol",
    "lib/timer": "anvil/timer",
    "lib/sections": "anvil/sections",
    "lib/notifications": "anvil/notifications",
    "repositories/base.h": "anvil/db/repository.h",
    "repositories/versioned.h": "anvil/db/versioned.h",
    "controllers/route_declaration.h": "anvil/accesscontrol/route_declaration.h",
}

# yardclub doc -> anvil doc. Anything not here is product documentation that has
# no counterpart, and the citation is dropped.
DOCS = {
    "00-architecture": "00-architecture",
    "01-data-model": "09-mongodb",
    "02-i18n-utf8": "03-i18n-utf8",
    "03-access-control": "04-access-control",
    "04-auth-sessions": "05-auth-sessions",
    "06-sectionsdata": "12-sections-cms",
    "09-lib-inputvalidation": "06-input-validation",
    "10-lib-filesystem": "07-filesystem",
    "11-lib-images": "08-images",
    "12-lib-timer": "10-timer-jobs",
    "13-lib-resetsections": "12-sections-cms",
    "14-lib-dynamicforms": "13-dynamic-forms",
    "15-mongodb-drogon": "09-mongodb",
    "17-lib-notifications": "11-notifications",
}

# yardclub CLAUDE.md section -> anvil CLAUDE.md section.
RULES = {"1": "2", "2": "3", "3": "4", "4": "5", "5": "6", "6": "7", "7": "8"}


def lift(text: str) -> tuple[str, list[str]]:
    notes: list[str] = []

    text = text.replace("yardclub::", "anvil::")
    text = re.sub(r"\bnamespace yardclub\b", "namespace anvil", text)
    text = text.replace("// namespace yardclub", "// namespace anvil")
    text = re.sub(r"\bYARDCLUB_", "ANVIL_", text)
    # Bare identifiers and string literals naming the originating project: a
    # target name in a comment, a temp-path prefix. None of the three patterns
    # above catch these, and both kinds shipped before this line existed.
    text = re.sub(r"\byardclub_(foundation|core)\b", r"anvil_\1", text)
    text = re.sub(r"/tmp/yardclub-", "/tmp/anvil-", text)

    for old, new in sorted(MODULES.items(), key=lambda kv: -len(kv[0])):
        text = text.replace(f'#include "{old}', f'#include "{new}')
        text = text.replace(f"({old}", f"({new}")
        text = text.replace(f" {old}/", f" {new}/")

    def doc_ref(m: re.Match) -> str:
        name = m.group(1)
        mapped = DOCS.get(name)
        return f"docs/{mapped}.md" if mapped else "\x00DROPDOC\x00"

    text = re.sub(r"docs/([0-9]{2}-[a-z-]+)\.md", doc_ref, text)

    # The BARE form — "docs/12 §7" — which the pattern above misses entirely
    # because it has no name and no extension. It is the commoner spelling in
    # running prose, and it shipped unmapped before this line existed.
    BARE = {"00": "00-architecture", "01": "09-mongodb", "02": "03-i18n-utf8",
            "03": "04-access-control", "04": "05-auth-sessions",
            "06": "12-sections-cms", "09": "06-input-validation",
            "10": "07-filesystem", "11": "08-images", "12": "10-timer-jobs",
            "13": "12-sections-cms", "14": "13-dynamic-forms",
            "15": "09-mongodb", "17": "11-notifications"}
    for number in ("05", "07", "08", "16", "18", "19", "20", "21", "22", "23", "24", "25", "26"):
        text = re.sub(r"\s*\(docs/" + number + r"(?![0-9-])[^)]*\)", "", text)
        text = re.sub(r",?\s*docs/" + number + r"(?![0-9-])\s*§[0-9.]+[a-z]?", "", text)
    text = re.sub(r"docs/([0-9]{2})(?![0-9a-z-])",
                  lambda m: f"docs/{BARE[m.group(1)]}.md" if m.group(1) in BARE else m.group(0),
                  text)

    # A dropped doc citation takes its surrounding parenthetical or trailing
    # clause with it, rather than leaving a dangling "(  §3)".
    text = re.sub(r"\s*\(\x00DROPDOC\x00[^)]*\)", "", text)
    text = re.sub(r",?\s*(?:see\s+)?\x00DROPDOC\x00[^\s,.;)]*", "", text)
    text = text.replace("\x00DROPDOC\x00", "")

    text = re.sub(r"CLAUDE\.md §(\d)", lambda m: f"CLAUDE.md §{RULES.get(m.group(1), m.group(1))}", text)

    # A comment can wrap a citation across lines, which puts a `//` between the
    # digits and the closing paren. Join those before matching, or the citation
    # survives and dangles — which is how the first pass missed one.
    text = re.sub(r"\(((?:finding |findings )?F\d+(?:,\s*)?)\n(\s*//\s*)(F\d+(?:,\s*F\d+)*)\)",
                  lambda m: f"({m.group(1).rstrip().rstrip(',')}, {m.group(3)})", text)

    # Finding numbers. The register does not exist here, so a bare citation
    # dangles. Report every one so the reviewer can confirm the sentence still
    # carries its reasoning without it.
    for m in re.finditer(r"\(?\b(?:finding |findings )?(F\d+(?:, F\d+)*)\)?", text):
        notes.append(f"finding citation dropped: {m.group(1)}")
    text = re.sub(r"\s*\((?:finding |findings )?F\d+(?:,\s*F\d+)*\)", "", text)
    text = re.sub(r",?\s*(?:finding|findings) F\d+(?:,\s*F\d+)*", "", text)
    text = re.sub(r"\s*\(\*\*F\d+\*\*\)", "", text)
    text = re.sub(r"\s+\(§[^)]*\)", lambda m: m.group(0), text)

    # Anything still naming the originating project needs a human.
    for m in re.finditer(r".*\byardclub\b.*", text, re.IGNORECASE):
        notes.append(f"MANUAL: {m.group(0).strip()[:100]}")

    return text, notes


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: lift.py <src> <dst>", file=sys.stderr)
        return 2
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    text, notes = lift(src.read_text())
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(text)
    for note in notes:
        print(f"  {dst.name}: {note}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
