#!/usr/bin/env bash
# Reproduce the Forgejo CI check on a developer machine, without compiling on
# the host: the source snapshot, the Bazel output and every artifact stay inside
# a throwaway podman container that is deleted when the command ends.
#
# Usage: deploy/ci/reproduce.sh [build|test|all] [worktree-path]
#
# Requirements on the host: podman and the CI image (see deploy/ci/Containerfile).
# This script never invokes a compiler, bazel, or a build tool on the host.
set -euo pipefail
control=$(cd "$(dirname "$0")" && pwd)
scope=${1:-all}
worktree=${2:-$(cd "$control/../.." && pwd)}
image=${CI_IMAGE_TAG:-localhost/zen-ci:ubuntu24.04-bazel8.4.2}

case $scope in build|test|all) ;; *) echo "usage: reproduce.sh [build|test|all] [worktree]" >&2; exit 1 ;; esac
command -v podman >/dev/null || { echo 'reproduce: podman is required' >&2; exit 1; }
podman image exists "$image" || {
  echo "reproduce: missing image $image; build it with:" >&2
  echo "  podman build --network host -t $image -f deploy/ci/Containerfile ." >&2
  exit 1
}

worktree=$(cd "$worktree" && pwd)
snapshot=$(mktemp -t zen-ci-snapshot-XXXXXX.tar)
trap 'rm -f "$snapshot"' EXIT

# Tracked files plus uncommitted-but-not-ignored files: this reproduces what a
# push would contain, without host build artifacts or git metadata.
tar --null --no-recursion -C "$worktree" -cf "$snapshot" --files-from <(git -C "$worktree" ls-files -z -co --exclude-standard)
echo "reproduce: snapshot $(du -h "$snapshot" | cut -f1) from $worktree@$(git -C "$worktree" rev-parse --short HEAD 2>/dev/null || echo unknown)"

# The host proxy is reachable from the container only on the host network.
proxy_env=()
for name in http_proxy https_proxy HTTP_PROXY HTTPS_PROXY no_proxy NO_PROXY; do
  [[ -n ${!name:-} ]] && proxy_env+=(-e "$name=${!name}")
done

podman run --rm --interactive --network host \
  --memory=3g --memory-swap=3g --cpus=4 --pids-limit=512 \
  --volume "$snapshot:/src.tar:ro" \
  "${proxy_env[@]}" \
  "$image" bash -euo pipefail -c '
    mkdir -p /workspace/zen
    tar -xf /src.tar -C /workspace/zen
    export CI_WORKSPACE=/workspace/zen CI_SCRATCH=/workspace/.ci CI_RESULTS=/workspace/results
    cd /workspace/zen
    bash deploy/ci/checks.sh "$1"
    echo "--- artifact summary (inside the disposable container) ---"
    cat /workspace/results/identity.json
    [[ -f /workspace/results/test-report.json ]] && cat /workspace/results/test-report.json
    true
  ' bash "$scope"
