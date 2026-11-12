#!/usr/bin/env bash
# Static contracts for the ZenAgent CI entry points. Runs no compiler.
#
# Used by the `quality` job of .forgejo/workflows/ci.yml and at the start of
# deploy/ci/checks.sh, so a broken CI contract fails before the expensive build.
set -euo pipefail
control=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$control/../.." && pwd)

source "$control/container-guard.sh"

die() { echo "ci contract: $*" >&2; exit 1; }

[[ -f $root/.bazelversion ]] || die '.bazelversion is missing'
pinned=$(tr -d '[:space:]' <"$root/.bazelversion")
[[ $pinned == "$CI_BAZEL_VERSION" ]] || die ".bazelversion ($pinned) differs from the job image ($CI_BAZEL_VERSION)"

image_bazel=$(sed -n 's/^ARG BAZEL_VERSION=\(.*\)$/\1/p' "$control/Containerfile")
[[ $image_bazel == "$CI_BAZEL_VERSION" ]] || die "deploy/ci/Containerfile ARG BAZEL_VERSION ($image_bazel) differs from the job image ($CI_BAZEL_VERSION)"

actual=$(bazel --version 2>/dev/null || true)
[[ $actual == "bazel $CI_BAZEL_VERSION" ]] || die "bazel inside the image is '$actual', expected 'bazel $CI_BAZEL_VERSION'"

python3 - "$control/bazel-ci-config.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as handle:
    config = json.load(handle)
if config.get('schema_version') != 1:
    raise SystemExit('ci contract: unsupported bazel-ci-config schema_version')
for key in ('startup_options', 'build_options', 'test_options'):
    value = config.get(key)
    if not isinstance(value, list) or not all(isinstance(item, str) and item.startswith('-') for item in value):
        raise SystemExit(f'ci contract: bazel-ci-config {key} must be a list of options')
for required in ('--nocache_test_results', '--test_output=errors'):
    if required not in config['test_options']:
        raise SystemExit(f'ci contract: test options must keep {required}')
PY

for script in "$control"/*.sh; do
  bash -n "$script" || die "syntax error in ${script#"$root"/}"
done

[[ $(sed -n 's/^ARG BAZEL_SHA256=\(.*\)$/\1/p' "$control/Containerfile") =~ ^[0-9a-f]{64}$ ]] \
  || die 'deploy/ci/Containerfile must pin a full BAZEL_SHA256'

for workflow in "$root"/.forgejo/workflows/*.yml; do
  [[ -f $workflow ]] || continue
  grep -q 'runs-on:' "$workflow" || die "${workflow#"$root"/} has no runs-on"
  grep -q 'container:' "$workflow" || die "${workflow#"$root"/} must run in a container, never on the runner host"
done

echo "ci contract: ok (image $CI_IMAGE, bazel $CI_BAZEL_VERSION, workspace $CI_WORKSPACE)"
