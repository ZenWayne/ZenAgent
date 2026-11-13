#!/usr/bin/env bash
# Refuse to do any project work outside the disposable Forgejo job container.
#
# Every deploy/ci entry point sources this first. Host compilation is forbidden
# by AGENTS.md / docs/ci/README.md; the only allowed host-side build is the CI
# image itself (deploy/ci/Containerfile).
set -euo pipefail

die() { echo "ci container guard: $*" >&2; exit 1; }

# Prints the deepest mount that provides $1 when that is not the container's own
# filesystem, empty otherwise. A bind-mounted host checkout is exactly what must
# never become the workspace: Bazel would write bazel-out/ into the host tree
# and CI artifacts would become host artifacts. Both shapes count — the
# workspace being a mount point, and the workspace sitting below one.
#
# mountinfo rows are `id parent-id major:minor root mount-point options ...`;
# the mount point is field 5, not field 2, and it escapes space, tab, newline
# and backslash as \040, \011, \012 and \134.
workspace_host_mount() {
  local target=$1 source=${2:-/proc/self/mountinfo}
  awk -v target="$target" '
    {
      mount = $5
      gsub(/\\040/, " ", mount)
      gsub(/\\011/, "\t", mount)
      gsub(/\\012/, "\n", mount)
      gsub(/\\134/, "\\\\", mount)
      if (mount == "/") next
      if ((target == mount || index(target, mount "/") == 1) &&
          length(mount) > length(deepest)) deepest = mount
    }
    END { if (deepest != "") print deepest }
  ' "$source"
}

# Exit 0 when $1 is a safe workspace for the given mountinfo ($2) and runner
# workspace ($3). Otherwise prints the offending mount and returns 1.
#
# The runner mounts its own workspace directory into the job container, so being
# below that one mount is normal and expected. A checkout mounted *at* the
# workspace, or a workspace below any other mount, would put Bazel output in a
# host tree.
workspace_mount_problem() {
  local target=$1 source=$2 runner=${3%/}
  local mount
  mount=$(workspace_host_mount "$target" "$source")
  [[ -n $mount ]] || return 0
  [[ $mount != "$target" ]] || { printf '%s\n' "$mount"; return 1; }
  [[ -n $runner && $mount == "$runner" ]] && return 0
  printf '%s\n' "$mount"
  return 1
}

# `--probe <mountinfo> <workspace> [runner-workspace]` evaluates only the
# workspace predicate, so deploy/ci/contract-check.sh can exercise it against
# fixtures without a container. Sourcing this file — the real entry point —
# never sees arguments.
if [[ ${1:-} == --probe ]]; then
  [[ $# == 3 || $# == 4 ]] || {
    echo 'usage: container-guard.sh --probe <mountinfo> <workspace> [runner-workspace]' >&2
    exit 2
  }
  if problem=$(workspace_mount_problem "$3" "$2" "${4:-}"); then
    exit 0
  fi
  echo "$problem"
  exit 1
fi

[[ ${CI_CONTAINER:-0} == 1 ]] || die 'CI_CONTAINER=1 is not set; run inside the Forgejo job container'
[[ -f /run/.containerenv || -f /.dockerenv ]] || die 'no container marker; host execution is forbidden'
[[ -n ${CI_IMAGE:-} && -n ${CI_BAZEL_VERSION:-} ]] || die 'CI_IMAGE / CI_BAZEL_VERSION image markers are missing'

# The workspace must be a private copy inside the container filesystem.
workspace=$(realpath -- "${CI_WORKSPACE:-}")
[[ -n $workspace && -d $workspace ]] || die 'CI_WORKSPACE is not an existing directory: '${CI_WORKSPACE:-}
[[ $workspace != / ]] || die 'workspace must not be the container root'
if problem=$(workspace_mount_problem "$workspace" /proc/self/mountinfo \
    "${GITHUB_WORKSPACE:-}"); then
  :
else
  die "workspace $workspace is on the mount $problem, which is not the runner workspace ('${GITHUB_WORKSPACE:-}'); CI must build a private copy, not a host mount"
fi
[[ -w $workspace ]] || die "workspace $workspace is not writable"
