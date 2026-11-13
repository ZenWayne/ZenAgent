#!/usr/bin/env bash
# Translate the runner's http(s) proxy environment into Bazel JVM arguments.
#
# Bazel downloads modules / http_archive / maven artifacts from the Bazel server
# JVM, which ignores http_proxy and https_proxy. The runner provides the proxy
# as plain env vars (host.containers.internal:10809 in the Forgejo job), so the
# workflow derives a throwaway bazelrc instead of pinning a machine address in
# the repository.
#
# Usage: proxy-bazelrc.sh <output-file>
# Writes an empty (but valid) file when no proxy is configured.
set -euo pipefail

[[ $# == 1 ]] || { echo 'usage: proxy-bazelrc.sh <output-file>' >&2; exit 1; }
output=$1
: >"$output"

proxy=${https_proxy:-${HTTPS_PROXY:-${http_proxy:-${HTTP_PROXY:-}}}}
[[ -n $proxy ]] || { echo 'ci proxy: no proxy configured; Bazel downloads go direct'; exit 0; }

[[ $proxy =~ ^[a-z]+://([^/:]+):([0-9]+)/?$ ]] || {
  echo "ci proxy: unsupported proxy form (expected scheme://host:port): $proxy" >&2
  exit 1
}
host=${BASH_REMATCH[1]}
port=${BASH_REMATCH[2]}

{
  for scheme in http https; do
    printf 'startup --host_jvm_args=-D%s.proxyHost=%s\n' "$scheme" "$host"
    printf 'startup --host_jvm_args=-D%s.proxyPort=%s\n' "$scheme" "$port"
  done
  printf 'startup --host_jvm_args=-Dhttp.nonProxyHosts=localhost|127.0.0.1|[::1]\n'
} >>"$output"

echo "ci proxy: Bazel JVM uses $host:$port"
