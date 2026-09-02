#!/usr/bin/env bash
#
# build_jni_arm64.sh — Build the agentflow JNI shared library for Android
# arm64-v8a via Bazel and IMMEDIATELY stage it into the Android app's jniLibs
# so the artifact survives even if a later build step (or disk-pressure
# cleanup) wipes intermediate build trees.
#
# Why staging matters: the CMake/Bazel build trees self-clean / get pruned
# when the disk fills mid-build. Copying the finished .so into the app source
# tree the moment Bazel produces it makes the deliverable durable and usable.
#
# Usage:  ./build_jni_arm64.sh
#
set -euo pipefail

# --- config ----------------------------------------------------------------
ZEN_ROOT="/home/wayne/tools/zen"
TARGET="//jni:libagentflow_jni.so"
BAZEL_CONFIG="android_arm64"
ABI="arm64-v8a"
SO_NAME="libagentflow_jni.so"

JNILIBS_DIR="/home/wayne/tools/zen_mobile/app/src/main/jniLibs/${ABI}"
STAGED_SO="${JNILIBS_DIR}/${SO_NAME}"

export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$HOME/android-ndk/android-ndk-r28b}"
export ANDROID_NDK_ROOT="$ANDROID_NDK_HOME"
export https_proxy="${https_proxy:-http://127.0.0.1:10809}"
export http_proxy="${http_proxy:-http://127.0.0.1:10809}"

# --- build -----------------------------------------------------------------
cd "$ZEN_ROOT"
echo "[build_jni_arm64] building $TARGET (--config=$BAZEL_CONFIG)"
bazelisk build --config="$BAZEL_CONFIG" "$TARGET"

# --- locate the freshly built .so ------------------------------------------
# Resolve via `bazel cquery` so we pick the arm64 output, not a stale k8 one.
BIN_PATH="$(bazelisk --host_jvm_args=-Dhttps.proxyHost=127.0.0.1 --host_jvm_args=-Dhttps.proxyPort=10808 cquery --config="$BAZEL_CONFIG" --output=files "$TARGET" 2>/dev/null | tail -1)"
if [ -z "$BIN_PATH" ] || [ ! -f "$BIN_PATH" ]; then
  # Fallback: bazel-bin symlink (valid right after the build above).
  BIN_PATH="$ZEN_ROOT/bazel-bin/jni/${SO_NAME}"
fi

if [ ! -f "$BIN_PATH" ]; then
  echo "[build_jni_arm64] ERROR: built .so not found at: $BIN_PATH" >&2
  exit 1
fi

# --- verify it is actually arm64 BEFORE staging ----------------------------
ARCH="$(file -b "$BIN_PATH")"
case "$ARCH" in
  *aarch64*|*"ARM aarch64"*) : ;;  # good
  *)
    echo "[build_jni_arm64] ERROR: built .so is NOT arm64 (got: $ARCH)" >&2
    echo "  path: $BIN_PATH" >&2
    exit 1
    ;;
esac

# --- stage immediately ------------------------------------------------------
mkdir -p "$JNILIBS_DIR"
# Copy to a temp name then atomically move, so a partial copy never leaves a
# corrupt .so in jniLibs.
cp -f "$BIN_PATH" "${STAGED_SO}.tmp"
chmod u+w "${STAGED_SO}.tmp"
mv -f "${STAGED_SO}.tmp" "$STAGED_SO"

echo "[build_jni_arm64] STAGED -> $STAGED_SO"
ls -la "$STAGED_SO"
file -b "$STAGED_SO"
echo "[build_jni_arm64] done."
