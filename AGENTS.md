# AGENTS.md

## Build & network

- **Downloads use proxy: `http://127.0.0.1:10808`** (http_proxy / https_proxy). Applies to: curl/wget/git clones, CFetchContent / ExternalProject / file(DOWNLOAD) during CMake configure-build, cargo/rustup, and bazel JVM args (`--host_jvm_args=-Dhttps.proxyHost=127.0.0.1 --host_jvm_args=-Dhttps.proxyPort=10808`).
- Bazel primary build: `bazel build //agentflow/...`; tests: `bazel test //tests/unit/...`.
- `--config=android_arm64` cross-compiles for Android arm64; requires `ANDROID_NDK_HOME=$HOME/android-ndk/android-ndk-r28b`.
- Direct github routes from this machine are flaky (interrupted transfers, status 56); prefer the proxy.
- Sandbox helpers: `.ultra_sandbox/bin/` contains a bundled toolchain (bazel, git, gh, ...) — use with care.
