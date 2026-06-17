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

# Combine a list of .a files into one thin-member archive by extracting all
# members into a private dir (disambiguating same-named members) then `ar rcs`.
combine() {
  local out="$1"; shift
  local extract="$WORK/$(basename "$out").d"
  rm -rf "$extract"; mkdir -p "$extract"
  local i=0
  for src in "$@"; do
    local sub="$extract/$i"; mkdir -p "$sub"; i=$((i+1))
    ( cd "$sub" && "$AR" x "$src" )
  done
  rm -f "$out"
  # Collect all object members across subdirs (unique paths, dup names kept separate).
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
