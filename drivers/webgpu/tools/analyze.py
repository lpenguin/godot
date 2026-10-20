from __future__ import annotations

import collections
import re
import sys
from typing import TypedDict


class FailureBlock(TypedDict):
    name: str
    first: str
    lines: list[str]


log = open(sys.argv[1], encoding="utf-8", errors="replace").read().split("\n")

# A failure block starts with "ERROR: WebGPU: Tint failed for 'NAME' (exit code N): <first line>"
# and the tool output (disassembly, errors) follows until the next "ERROR:"/"CONVERT" marker.
blocks: list[FailureBlock] = []
cur: FailureBlock | None = None
for line in log:
    m = re.match(r"ERROR: WebGPU: Tint failed for '([^']+)' \(exit code (\d+)\): (.*)", line)
    if m:
        cur = {"name": m.group(1), "first": m.group(3), "lines": []}
        blocks.append(cur)
        continue
    if line.startswith("ERROR: Condition") or line.startswith("CONVERT") or line.startswith("   at:"):
        cur = None
        continue
    if cur is not None:
        cur["lines"].append(line)


def norm(s: str) -> str:
    s = re.sub(r"%[0-9A-Za-z_]+", "%X", s)
    s = re.sub(r"'[^']*'", "'..'", s)
    s = re.sub(r"\b\d+\b", "N", s)
    return s.strip()[:170]


cats: collections.Counter[str] = collections.Counter()
examples: dict[str, str] = {}
fams: collections.defaultdict[str, collections.Counter[str]] = collections.defaultdict(collections.Counter)
for b in blocks:
    msgs = []
    for message in b["lines"]:
        if re.search(r"\berror\b", message) and "OpName" not in message and "Op" != message.strip()[:2]:
            if message.strip().startswith(("error:", "spirv:", "<", "[")) or re.match(r"^\S", message):
                msgs.append(message.strip())
    key = norm(msgs[0]) if msgs else norm(b["first"])
    cats[key] += 1
    examples.setdefault(key, b["name"])
    fams[key][re.sub(r"_?\d*__[0-9a-f]+$", "", b["name"])] += 1

print("failures parsed:", len(blocks))
for key, n in cats.most_common(25):
    print("%4d  %s" % (n, key))
    print(
        "        e.g. %s; families: %s" % (examples[key], ", ".join("%s(%d)" % kv for kv in fams[key].most_common(6)))
    )
