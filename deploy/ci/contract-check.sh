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
min_disk = config.get('min_disk_gib')
if not isinstance(min_disk, int) or min_disk < 6:
    raise SystemExit('ci contract: bazel-ci-config min_disk_gib must be an integer >= 6')
print(f'ci contract: min_disk_gib {min_disk}')
PY

# The workflow used to pin its own CI_MIN_DISK_GIB, which silently overrode the
# config. One threshold, one place.
if grep -q 'CI_MIN_DISK_GIB' "$root"/.forgejo/workflows/*.yml; then
  die 'the workflow must not set CI_MIN_DISK_GIB; use bazel-ci-config min_disk_gib'
fi

for script in "$control"/*.sh; do
  bash -n "$script" || die "syntax error in ${script#"$root"/}"
done

# The workspace mount guard is the only thing keeping a bind-mounted host
# checkout out of the build. Exercise the predicate against fixtures: the runner
# workspace mount is expected, a workspace that is itself a mount or sits below
# any other mount is not, and mountinfo's \040 escapes must be decoded.
#
# Everything here is local to the function: this file is sourced by checks.sh
# after it has already computed its own `workspace`.
check_mount_guard() {
  local probe runner_ws build_ws guard fixture sample case_workspace case_runner
  probe=$(mktemp -d)
  runner_ws=/workspace/wayne/zenagent
  build_ws=$runner_ws/source
  printf '%s\n' \
    '22 1 0:21 / / rw,relatime - overlay overlay rw,lowerdir=/a' \
    '31 22 0:29 / /proc rw,nosuid - proc proc rw' \
    >"$probe/image-fs"
  printf '%s\n' \
    '22 1 0:21 / / rw,relatime - overlay overlay rw,lowerdir=/a' \
    "56 22 0:60 $runner_ws $runner_ws rw,relatime - ext4 /dev/nvme0n1p2 rw" \
    >"$probe/runner-workspace"
  printf '%s\n' \
    '22 1 0:21 / / rw,relatime - overlay overlay rw,lowerdir=/a' \
    "56 22 0:60 /home/wayne/tools/zen $build_ws rw,relatime - ext4 /dev/nvme0n1p2 rw" \
    >"$probe/checkout-at-workspace"
  cat "$probe/runner-workspace" >"$probe/nested-checkout"
  tail -n 1 "$probe/checkout-at-workspace" >>"$probe/nested-checkout"
  cat "$probe/checkout-at-workspace" >"$probe/nested-checkout-reversed"
  tail -n 1 "$probe/runner-workspace" >>"$probe/nested-checkout-reversed"
  printf '%s\n' \
    '22 1 0:21 / / rw,relatime - overlay overlay rw,lowerdir=/a' \
    '57 22 0:61 /mnt/scratch /mnt/scratch rw,relatime - ext4 /dev/sdb rw' \
    >"$probe/foreign-mount"
  printf '%s\n' \
    '22 1 0:21 / / rw,relatime - overlay overlay rw,lowerdir=/a' \
    '56 22 0:60 /mnt/my\040checkout /mnt/my\040checkout rw,relatime - ext4 /dev/sdb rw' \
    >"$probe/escaped-mount"

  guard="$control/container-guard.sh"
  bash "$guard" --probe "$probe/image-fs" "$build_ws" \
    || die 'mount guard rejects a workspace on the container filesystem'
  bash "$guard" --probe "$probe/runner-workspace" "$build_ws" "$runner_ws" \
    || die 'mount guard rejects a workspace inside the runner workspace mount'
  for fixture in "runner-workspace|$build_ws|" \
                 "checkout-at-workspace|$build_ws|$runner_ws" \
                 "nested-checkout|$build_ws|$runner_ws" \
                 "nested-checkout-reversed|$build_ws|$runner_ws" \
                 "nested-checkout|$build_ws/child|$runner_ws" \
                 "nested-checkout-reversed|$build_ws/child|$runner_ws" \
                 "foreign-mount|/mnt/scratch/build|$runner_ws"; do
    IFS='|' read -r sample case_workspace case_runner <<<"$fixture"
    if bash "$guard" --probe "$probe/$sample" "$case_workspace" \
        ${case_runner:+"$case_runner"} >/dev/null; then
      die "mount guard misses a host mount (fixture $sample)"
    fi
  done
  if bash "$guard" --probe "$probe/escaped-mount" '/mnt/my checkout/zen' \
      "$runner_ws" >/dev/null; then
    die 'mount guard does not decode mountinfo path escapes'
  fi
  rm -rf "$probe"
}

check_mount_guard

[[ $(sed -n 's/^ARG BAZEL_SHA256=\(.*\)$/\1/p' "$control/Containerfile") =~ ^[0-9a-f]{64}$ ]] \
  || die 'deploy/ci/Containerfile must pin a full BAZEL_SHA256'

for workflow in "$root"/.forgejo/workflows/*.yml; do
  [[ -f $workflow ]] || continue
  grep -q 'runs-on:' "$workflow" || die "${workflow#"$root"/} has no runs-on"
  grep -q 'container:' "$workflow" || die "${workflow#"$root"/} must run in a container, never on the runner host"
done

echo "ci contract: ok (image $CI_IMAGE, bazel $CI_BAZEL_VERSION, workspace $CI_WORKSPACE)"
