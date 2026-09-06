#!/usr/bin/env bash
#
# rebuild_re2_pic_host.sh — repair the host libce_external.a so it can be
# linked into a shared library.
#
# WHY
#   The host prebuilt was produced without -fPIC. Ten of its re2 objects carry
#   R_X86_64_PC32 relocations against global symbols, so ld.gold refuses to put
#   them in libagentflow_jni.so:
#
#     requires dynamic R_X86_64_PC32 reloc against '...' which may overflow at
#     runtime; recompile with -fPIC
#
#   That blocks the whole Kotlin JVM test layer (//kotlin: HostToolBridgeTest,
#   SmokeTest, WorkflowJsonTest), which loads the JNI shared lib. The arm64
#   archive is unaffected — it was already rebuilt this way in 5bc421a.
#
#   Only re2 is at fault, so this rebuilds re2 alone and swaps its objects into
#   the archive rather than rebuilding LiteRT-LM (hours, 13 GB of build tree).
#
# USAGE
#   ./rebuild_re2_pic_host.sh [<output.a>]
#
#   Then upload the result to a NEW release tag and update both `sha256` and
#   `urls` of the `litert_ce_external` http_file in MODULE.bazel. (The archive
#   is consumed by URL, so a local path is a verification-only shortcut — it is
#   machine-specific and must never be committed.)
#
# PREREQUISITES
#   The LiteRT-LM host build tree from the original build, which supplies both
#   the exact re2 sources and the matching abseil install.
set -euo pipefail

BUILD_TREE="${BUILD_TREE:-$(cd "$(dirname "$0")/../../.." && pwd)/litert_build_90f}"
RE2_SRC="$BUILD_TREE/litert_lm/build/external/re2/src/re2_external"
ABSL_DIR="$BUILD_TREE/litert_lm/build/external/abseil-cpp/install/lib/cmake/absl"
OUT="${1:-$BUILD_TREE/dist-host-90f-pic-libce_external.a}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for p in "$RE2_SRC" "$ABSL_DIR"; do
  [ -e "$p" ] || { echo "ERROR: missing $p (host build tree required)" >&2; exit 1; }
done

# The archive Bazel already downloaded is the base we patch.
BASE="$(bazel info output_base)/external/+_repo_rules2+litert_ce_external/file/libce_external.a"
[ -f "$BASE" ] || { echo "ERROR: fetch it first: bazel build //jni:libagentflow_jni.so" >&2; exit 1; }

# Configure exactly as the original host build did (CMAKE_BUILD_TYPE was empty
# there) so position-independent code is the only delta.
cmake -S "$RE2_SRC" -B "$WORK/re2" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DRE2_BUILD_TESTING=OFF \
  -Dabsl_DIR="$ABSL_DIR"
cmake --build "$WORK/re2" -j"$(nproc)"

cp "$BASE" "$OUT"
chmod u+w "$OUT"

# Swap each re2 object in. Members are named NNNNN_C_<basename>, so the name is
# matched anchored (`re2.cc.o` must not match `filtered_re2.cc.o`), and every
# replacement is checked to define a superset of the original's symbols — a PIC
# build adds only GCC `.localalias` entries. Anything else means the name
# collided with a different library, and we stop rather than corrupt 161 MB.
python3 - "$WORK/re2/libre2.a" "$OUT" "$WORK" <<'PY'
import os, re, subprocess, sys
pic_lib, archive, work = sys.argv[1:4]

def members(p):
    return subprocess.run(["ar","t",p],capture_output=True,text=True,check=True).stdout.split()

def defined(o):
    out = subprocess.run(["nm","--defined-only",o],capture_output=True,text=True).stdout
    return {l.split()[-1] for l in out.splitlines() if l.strip()}

arc = members(archive)
mapping = {}
for m in members(pic_lib):
    hits = [a for a in arc if re.fullmatch(r"\d+_\d+_" + re.escape(m), a)]
    if len(hits) > 1:
        sys.exit(f"ambiguous member name {m} -> {hits}")
    if hits:
        mapping[m] = hits[0]

stage, orig = os.path.join(work,"pic"), os.path.join(work,"orig")
os.makedirs(stage, exist_ok=True); os.makedirs(orig, exist_ok=True)
subprocess.run(["ar","x",pic_lib], cwd=stage, check=True)
subprocess.run(["ar","x",archive] + list(mapping.values()), cwd=orig, check=True)

renamed = []
for src, dst in mapping.items():
    before, after = defined(os.path.join(orig,dst)), defined(os.path.join(stage,src))
    extra = after - before
    if not before <= after or any(not e.endswith(".localalias") for e in extra):
        sys.exit(f"symbol mismatch on {dst}: missing={sorted(before-after)[:3]} extra={sorted(extra)[:3]}")
    os.rename(os.path.join(stage,src), os.path.join(stage,dst))
    renamed.append(dst)

subprocess.run(["ar","r",archive] + renamed, cwd=stage, check=True)
print(f"replaced {len(renamed)} re2 objects")
PY

echo
echo "→ $OUT"
sha256sum "$OUT"
echo
echo "Verify:  bazel build //jni:libagentflow_jni.so   # must link, x86-64"
echo "Then:    upload to a new release tag, update urls + sha256 in MODULE.bazel"
