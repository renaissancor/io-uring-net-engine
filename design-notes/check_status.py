#!/usr/bin/env python3
"""Lint the design-notes status front matter.

Every dated note (YYYY-MM-DD-*.md) must open with a front-matter block whose
`status` is one of proposed | accepted | superseded. A superseded note must
name at least one existing successor in `superseded_by`; `amended_by`
targets must exist too. The journal table in README.md must carry each
note's status word. Exit 1 on any violation, listing them all.

    python3 design-notes/check_status.py
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
STATUSES = {"proposed", "accepted", "superseded"}
LIST_KEYS = {"superseded_by", "amended_by"}


def front_matter(text: str):
    if not text.startswith("---\n"):
        return None
    end = text.find("\n---\n", 4)
    if end < 0:
        return None
    fm, key = {}, None
    for line in text[4:end].splitlines():
        if line.startswith("  - ") and key in LIST_KEYS:
            fm[key].append(line[4:].strip())
        elif ":" in line:
            key, _, val = line.partition(":")
            key, val = key.strip(), val.strip()
            fm[key] = [] if key in LIST_KEYS and not val else val
    return fm


def main() -> int:
    errors = []
    readme = (HERE / "README.md").read_text()
    for path in sorted(HERE.glob("2026-*.md")):
        text = path.read_text()
        fm = front_matter(text)
        if fm is None:
            errors.append(f"{path.name}: no front matter")
            continue
        status = fm.get("status")
        if status not in STATUSES:
            errors.append(f"{path.name}: status {status!r} not in {sorted(STATUSES)}")
        if status == "superseded" and not fm.get("superseded_by"):
            errors.append(f"{path.name}: superseded without superseded_by")
        for key in LIST_KEYS:
            for target in fm.get(key, []) or []:
                if not (HERE / target).exists():
                    errors.append(f"{path.name}: {key} -> {target} does not exist")
        row = re.search(rf"^\|.*\]\({re.escape(path.name)}\).*$", readme, re.M)
        if row is None:
            errors.append(f"{path.name}: not in README.md journal")
        elif status and status.capitalize() not in row.group(0):
            errors.append(f"{path.name}: README row lacks the word {status.capitalize()!r}")
    for e in errors:
        print(e)
    print(f"{len(errors)} problem(s)" if errors else "ok")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
