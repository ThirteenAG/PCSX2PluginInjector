"""Replace our generated download block, preserving existing release notes."""
import os
from pathlib import Path
import sys

begin, end = "<!-- plugin-upstream:begin -->", "<!-- plugin-upstream:end -->"
body = os.environ.get("EXISTING_RELEASE_BODY", "")
snippet = Path(sys.argv[1]).read_text(encoding="utf-8").strip()
if begin in body:
    if body.count(begin) != 1 or body.count(end) != 1:
        raise ValueError("Ambiguous release download block")
    before, rest = body.split(begin, 1)
    _, after = rest.split(end, 1)
    body = before + snippet + after
else:
    body = body.rstrip() + "\n\n" + snippet + "\n"
Path(sys.argv[2]).write_text(body, encoding="utf-8")
