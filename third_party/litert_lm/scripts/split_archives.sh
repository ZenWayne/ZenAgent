#!/usr/bin/env bash
# Reproducibly split the LiteRT-LM C-engine link payload into the two
# prebuilt archives Bazel imports (see third_party/litert_lm/BUILD.bazel):
#   libce_staging.a  - LiteRT-LM-authored component libs (alwayslink=True)
#   libce_external.a - all other linked deps EXCEPT abseil (Bazel supplies abseil)
#
# Source of truth: the litert_lm_main link line
#   <BUILD_DIR>/CMakeFiles/litert_lm_main.dir/link.txt
# which lists every .a the final binary links. staging archives live under
# staging/lib/; abseil archives (external/abseil-cpp/.../libabsl_*.a) are
# excluded from the external aggregate. The kissfft shared lib is copied under
# its SONAME libkissfft-float.so.131.
#
# Usage: split_archives.sh <CMAKE_BUILD_DIR> <OUTPUT_DIR>
#   CMAKE_BUILD_DIR e.g. LiteRT-LM/cmake/build/android-arm64/litert_lm/build
#                        (the dir containing CMakeFiles/litert_lm_main.dir/link.txt)
#   OUTPUT_DIR      e.g. third_party/litert_lm/lib/arm64-v8a
set -euo pipefail

BUILD_DIR="${1:?need CMAKE_BUILD_DIR (dir containing CMakeFiles/litert_lm_main.dir/link.txt)}"
OUT_DIR="${2:?need OUTPUT_DIR}"
AR="${AR:-ar}"

LINK_TXT="$BUILD_DIR/CMakeFiles/litert_lm_main.dir/link.txt"
[ -f "$LINK_TXT" ] || { echo "ERROR: link.txt not found: $LINK_TXT" >&2; exit 1; }

mkdir -p "$OUT_DIR"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# All .a paths referenced by the final link, relative to BUILD_DIR.
mapfile -t ALL_ARCHIVES < <(tr ' ' '\n' < "$LINK_TXT" | grep -E '\.a$' | sort -u)

STAGING_ARCHIVES=(); EXTERNAL_ARCHIVES=()
for a in "${ALL_ARCHIVES[@]}"; do
  abspath="$BUILD_DIR/$a"
  [ -f "$abspath" ] || { echo "WARN: missing archive $a" >&2; continue; }
  case "$a" in
    staging/lib/*)                              STAGING_ARCHIVES+=("$abspath") ;;
    external/abseil-cpp/install/lib/libabsl_*.a) : ;;   # abseil excluded; Bazel supplies it
    *)                                          EXTERNAL_ARCHIVES+=("$abspath") ;;
  esac
done

echo "staging source archives:  ${#STAGING_ARCHIVES[@]}"
echo "external source archives: ${#EXTERNAL_ARCHIVES[@]} (abseil excluded)"

# Combine a list of .a files into one archive.
#
# CRITICAL: many of the component archives contain MULTIPLE members with the
# SAME basename (e.g. libXNNPACK.a has two `pack-lh.c.o`; libcpuinfo.a has two
# `cache.c.o` / `init.c.o`; libtensorflow-lite.a has 8 duplicated basenames).
# A plain `ar x` extracts every member of an archive into one directory, so the
# second same-named member OVERWRITES the first and its objects (and symbols)
# are silently lost. That is the bug that produced the old, too-small arm64
# libce_external.a (missing cpuinfo_*, xnn_*_pack_lh_*, TfLite C API symbols).
#
# Fix: extract each member of each source archive INDIVIDUALLY by ordinal index
# (`ar xN <archive> <member>`), writing each to a globally-unique path. This
# preserves every duplicate-named member.
#
# EXCLUDED OBJECTS (see third_party/litert_lm/BUILD.bazel + README.android.md):
#   litert_runtime_no_builtin.cc.o defines a bogus `R kLiteRtRuntimeBuiltin`
#   (read-only, value 0 == nullptr). The real def is `D kLiteRtRuntimeBuiltin`
#   in litert_runtime_builtin.cc.o. Under -Wl,--allow-multiple-definition the
#   null `R` def can win over the real `D` def, which crashes
#   litert::Environment::Create (CHECK_NOTNULL). The host link.txt never pulls
#   the no_builtin object; the arm64 aggregate accidentally did. Drop it so the
#   real (non-null) def is the only definition. This object carries ONLY that
#   one symbol, so excluding it loses nothing else.
EXCLUDE_OBJECT_BASENAMES=(
  "litert_runtime_no_builtin.cc.o"
)
is_excluded() {
  local base="$1" ex
  for ex in "${EXCLUDE_OBJECT_BASENAMES[@]}"; do
    [ "$base" = "$ex" ] && return 0
  done
  return 1
}
combine() {
  local out="$1"; shift
  local extract="$WORK/$(basename "$out").d"
  rm -rf "$extract"; mkdir -p "$extract"
  local i=0
  for src in "$@"; do
    local sub="$extract/$i"; mkdir -p "$sub"; i=$((i+1))
    # List members; for each (1-based) occurrence of a name, extract that exact
    # ordinal so duplicate basenames are all preserved under unique filenames.
    local -A seen=()
    local idx=0 name occ dest
    while IFS= read -r name; do
      idx=$((idx+1))
      occ=$(( ${seen["$name"]:-0} + 1 ))
      seen["$name"]=$occ
      # Skip objects that introduce a colliding bogus symbol definition.
      if is_excluded "$name"; then
        echo "EXCLUDING object from $(basename "$out"): $name" >&2
        continue
      fi
      dest="$sub/$(printf '%05d_%d_%s' "$idx" "$occ" "$name")"
      # `ar xN N` extracts the Nth member matching <name> to <name> in CWD.
      ( cd "$sub" && "$AR" xN "$occ" "$src" "$name" && mv -f "$name" "$(basename "$dest")" )
    done < <("$AR" t "$src")
  done
  rm -f "$out"
  # Collect all extracted object members (every unique on-disk path).
  find "$extract" -type f \( -name '*.o' -o -name '*.obj' \) -print0 \
    | xargs -0 "$AR" rcs "$out"
}

combine "$OUT_DIR/libce_staging.a"  "${STAGING_ARCHIVES[@]}"
combine "$OUT_DIR/libce_external.a" "${EXTERNAL_ARCHIVES[@]}"

# kissfft shared lib under its SONAME.
KISS="$(find "$BUILD_DIR" -name 'libkissfft-float.so.131' -type f 2>/dev/null | head -1)"
if [ -z "$KISS" ]; then
  KISS="$(find "$BUILD_DIR" -name 'libkissfft-float.so.131*' 2>/dev/null | head -1)"
fi
[ -n "$KISS" ] || { echo "ERROR: kissfft .so not found under $BUILD_DIR" >&2; exit 1; }
cp -f "$KISS" "$OUT_DIR/libkissfft-float.so.131"

echo "=== output ==="
ls -la "$OUT_DIR"
