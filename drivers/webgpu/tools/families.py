import re, os, sys, collections

dump_dir = sys.argv[1]
log_path = sys.argv[2]

# All modules by family. File: <Family>_<variant>__<hash>.<stage>.spv  (base without stage = <Family>_<variant>__<hash>)
def family(base):
    return re.sub(r"_\d+__[0-9a-f]+$", "", base)

total = collections.Counter()
bases = set()
for f in os.listdir(dump_dir):
    if f.endswith(".spv"):
        base = f.rsplit(".", 2)[0]
        bases.add(base)
        total[family(base)] += 1

fail_reason = {}
cur = None
log = open(log_path, encoding="utf-8", errors="replace").read().split("\n")
blocks = []
for line in log:
    m = re.match(r"ERROR: WebGPU: Tint failed for '([^']+)'", line)
    if m:
        cur = {"name": m.group(1), "msgs": [], "first": line}
        blocks.append(cur)
        continue
    if line.startswith(("ERROR: Condition", "CONVERT")):
        cur = None
        continue
    if cur is not None and (line.startswith("error:") or line.startswith("spirv:") or "Failed to parse SPIR-V:" in line):
        cur["msgs"].append(line.strip())

def reason(b):
    text = " ".join(b["msgs"]) + " " + b["first"]
    for key, label in [
        ("arrays of handle types", "texture arrays"),
        ("arrays cannot be used in the <immediate>", "push-constant arrays"),
        ("uniform control flow", "non-uniform derivatives/barrier"),
        ("storage class: Image", "image atomics"),
        ("OpMemoryBarrier", "memoryBarrier"),
        ("IsInf", "isinf"),
        ("IsNan", "isnan"),
        ("vertex pipeline stage", "writable buffer in vertex stage"),
        ("point_size", "gl_PointSize"),
        ("HelperInvocation", "gl_HelperInvocation"),
        ("capability", "capability"),
    ]:
        if key in text:
            return label
    return "other"

failed = collections.defaultdict(collections.Counter)
failed_total = collections.Counter()
for b in blocks:
    fam = family(b["name"])
    failed[fam][reason(b)] += 1
    failed_total[fam] += 1

rows = []
for fam, n in total.items():
    f = failed_total.get(fam, 0)
    rows.append((fam, n, n - f, f, ", ".join("%s %d" % kv for kv in failed[fam].most_common(3))))
rows.sort(key=lambda r: (-r[3], r[0]))
print("%-40s %5s %5s %5s  %s" % ("family", "total", "ok", "fail", "reasons"))
for r in rows:
    print("%-40s %5d %5d %5d  %s" % r)
print()
print("modules: %d, ok: %d, failed: %d" % (sum(total.values()), sum(total.values()) - sum(failed_total.values()), sum(failed_total.values())))
