#!/usr/bin/env bash
# The single locked entry point for ZenAgent CI verification.
#
# Runs only inside the disposable Forgejo job container (see docs/ci/README.md).
# Never invoked on the host: AGENTS.md forbids host compilation, and
# container-guard.sh refuses to continue outside the container.
#
# Usage: checks.sh <build|test|all>
#   build  bazel build //agentflow/...
#   test   bazel test  //tests/unit/...
#   all    both, in that order
#
# Environment (set by .forgejo/workflows/ci.yml):
#   CI_WORKSPACE        private source copy inside the container (required)
#   CI_SCRATCH          private scratch dir for bazel output and reports
#   CI_MIN_DISK_GIB     minimum free disk before starting; default is
#                       min_disk_gib in deploy/ci/bazel-ci-config.json
#   CI_RESULTS          results directory (default $CI_SCRATCH/results)
#
# Test evidence: $CI_RESULTS/testlogs/<package>/<target>/test.xml holds the raw
# GTest XML for every executed target, copied out of the Bazel output tree so it
# survives the disposable container.
set -euo pipefail
control=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$control/../.." && pwd)

[[ $# == 1 ]] || { echo 'usage: checks.sh <build|test|all>' >&2; exit 1; }
scope=$1
case $scope in build|test|all) ;; *) echo "unknown scope: $scope" >&2; exit 1 ;; esac

source "$control/container-guard.sh"

workspace=$(realpath -- "$CI_WORKSPACE")
scratch=$(realpath -m -- "${CI_SCRATCH:-/workspace/.ci}")
results=$(realpath -m -- "${CI_RESULTS:-$scratch/results}")
mkdir -p "$scratch" "$results"
[[ $scratch != "$workspace" && $scratch != "$workspace"/* ]] \
  || { echo 'CI_SCRATCH must be outside the source tree' >&2; exit 1; }

status=0
log() { printf '[%s] %s\n' "$(date -u +%H:%M:%SZ)" "$*"; }
# Free space on the scratch filesystem, in GiB. Logged around every scope so a
# run records the peak it actually needed instead of the guess in the config.
free_gib() { df -Pk "$scratch" | awk 'NR==2 {printf "%d", $4 / 1024 / 1024}'; }

# ── Preflight ───────────────────────────────────────────────────────
source "$control/contract-check.sh"

config_min_disk_gib=$(python3 -c \
  'import json,sys;print(json.load(open(sys.argv[1]))["min_disk_gib"])' \
  "$control/bazel-ci-config.json")
min_disk_gib=${CI_MIN_DISK_GIB:-$config_min_disk_gib}
available_gib=$(free_gib)
[[ $available_gib -ge $min_disk_gib ]] || {
  echo "ci preflight: ${available_gib}GiB free, ${min_disk_gib}GiB required" >&2
  exit 1
}
log "preflight: ${available_gib}GiB free (${min_disk_gib}GiB required), image $CI_IMAGE, bazel $CI_BAZEL_VERSION"

source_sha=$(git -C "$workspace" rev-parse HEAD 2>/dev/null || echo unknown)
configuration_sha=$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)
python3 - "$results/identity.json" "$source_sha" "$configuration_sha" <<'PY'
import json, os, sys
path, source_sha, configuration_sha = sys.argv[1:4]
identity = {
    'schema_version': 1,
    'source_sha': source_sha,
    'configuration_sha': configuration_sha,
    'image': os.environ.get('CI_IMAGE'),
    'bazel_version': os.environ.get('CI_BAZEL_VERSION'),
    'run_id': os.environ.get('GITHUB_RUN_ID'),
    'job': os.environ.get('GITHUB_JOB'),
    'attempt': os.environ.get('GITHUB_RUN_ATTEMPT'),
}
with open(path, 'w', encoding='utf-8') as handle:
    json.dump(identity, handle, indent=2)
    handle.write('\n')
PY
log "source $source_sha (config $configuration_sha)"

# ── Bazel invocation ────────────────────────────────────────────────
proxy_rc=$scratch/bazel-proxy.rc
bash "$control/proxy-bazelrc.sh" "$proxy_rc"

mapfile -t startup < <(python3 -c 'import json,sys;print("\n".join(json.load(open(sys.argv[1]))["startup_options"]))' "$control/bazel-ci-config.json")
mapfile -t build_options < <(python3 -c 'import json,sys;print("\n".join(json.load(open(sys.argv[1]))["build_options"]))' "$control/bazel-ci-config.json")
mapfile -t test_options < <(python3 -c 'import json,sys;print("\n".join(json.load(open(sys.argv[1]))["test_options"]))' "$control/bazel-ci-config.json")

bazel_cmd=(bazel "${startup[@]}" "--output_user_root=$scratch/bazel" "--bazelrc=$proxy_rc")

run_build() {
  local bep=$results/build.bep.jsonl
  log "build: //agentflow/... ($(free_gib)GiB free)"
  ( cd "$workspace" && "${bazel_cmd[@]}" build //agentflow/... "${build_options[@]}" \
      "--build_event_json_file=$bep" ) || status=1
  python3 "$control/verify-results.py" build "$bep" || status=1
  log "build finished; $(free_gib)GiB free"
}

run_test() {
  local bep=$results/test.bep.jsonl
  log "test: //tests/unit/... ($(free_gib)GiB free)"
  ( cd "$workspace" && "${bazel_cmd[@]}" test //tests/unit/... "${test_options[@]}" \
      "--build_event_json_file=$bep" ) || status=1
  python3 "$control/verify-results.py" test "$bep" \
    "--testlogs=$workspace/bazel-testlogs" \
    "--xml-out=$results/testlogs" \
    "--report=$results/test-report.json" || status=1
  log "test finished; $(free_gib)GiB free"
}

case $scope in
  build) run_build ;;
  test) run_test ;;
  all) run_build; run_test ;;
esac

log "scope $scope finished with status $status"
exit "$status"
