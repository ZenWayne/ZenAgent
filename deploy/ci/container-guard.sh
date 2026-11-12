#!/usr/bin/env bash
# Refuse to do any project work outside the disposable Forgejo job container.
#
# Every deploy/ci entry point sources this first. Host compilation is forbidden
# by AGENTS.md / docs/ci/README.md; the only allowed host-side build is the CI
# image itself (deploy/ci/Containerfile).
set -euo pipefail

die() { echo "ci container guard: $*" >&2; exit 1; }

[[ ${CI_CONTAINER:-0} == 1 ]] || die 'CI_CONTAINER=1 is not set; run inside the Forgejo job container'
[[ -f /run/.containerenv || -f /.dockerenv ]] || die 'no container marker; host execution is forbidden'
[[ -n ${CI_IMAGE:-} && -n ${CI_BAZEL_VERSION:-} ]] || die 'CI_IMAGE / CI_BAZEL_VERSION image markers are missing'

# The workspace must be a private copy inside the container filesystem. A
# bind-mounted host checkout would write bazel-out/ into the host tree and turn
# CI artifacts into host artifacts.
workspace=$(realpath -- "${CI_WORKSPACE:-}")
[[ -n $workspace && -d $workspace ]] || die 'CI_WORKSPACE is not an existing directory: '${CI_WORKSPACE:-}
[[ $workspace != / ]] || die 'workspace must not be the container root'
if awk -v target="$workspace" '$2=="\""target"\"" {found=1} END {exit !found}' /proc/self/mountinfo; then
  die "workspace $workspace is a mount point; CI must build a private copy, not a host mount"
fi
[[ -w $workspace ]] || die "workspace $workspace is not writable"
