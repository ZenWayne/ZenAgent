#!/usr/bin/env bash
# Build libagentflow_jni.so via Bazel and stage it where Gradle's
# java.library.path expects it.
set -euo pipefail

ROOT=$(realpath "$(dirname "$0")/../..")
PROXY=(
  --host_jvm_args=-Dhttps.proxyHost=127.0.0.1
  --host_jvm_args=-Dhttps.proxyPort=10809
  --host_jvm_args=-Dhttp.proxyHost=127.0.0.1
  --host_jvm_args=-Dhttp.proxyPort=10809
)

cd "$ROOT"
bazel "${PROXY[@]}" build //jni:libagentflow_jni.so
DEST="$ROOT/kotlin/build/libs/native"
mkdir -p "$DEST"
cp bazel-bin/jni/libagentflow_jni.so "$DEST/"
echo "Staged $DEST/libagentflow_jni.so"
