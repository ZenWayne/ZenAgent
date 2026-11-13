# AGENTS.md

## 构建与验证：只在 Forgejo CI，禁止宿主编译

- **绝对禁止在宿主机编译项目。** 任何 checkout（含本 worktree）里都不允许执行 `bazel`（`build`/`test`/`run`）、`cmake`、`make`、`g++`、`gradle`；宿主残留的 `bazel-*`、`bazel-out/`、`~/.cache/bazel/` 不能当作验证证据。宿主上唯一允许的构建动作是 CI 镜像本身：`podman build --network host -f deploy/ci/Containerfile -t localhost/zen-ci:ubuntu24.04-bazel8.4.2 .`（只装工具链，不编项目源码）。
- 唯一验证入口是 Forgejo：`.forgejo/workflows/ci.yml` → `deploy/ci/checks.sh <build|test|all>`，在一次性任务容器里编译与测试。派发、观察和取证据见 `docs/ci/operations.md`；平台设计与门槛见 `docs/ci/platform.md`。
- 本地复现用 `bash deploy/ci/reproduce.sh all`：同样在临时容器里执行，产物随容器销毁，不写宿主目录，也不替代目标 SHA 的 CI 记录。
- 检查范围：`bazel build //agentflow/...` 与 `bazel test //tests/unit/...`。Android arm64（`--config=android_arm64`，需 `ANDROID_NDK_HOME`）不在普通 CI 范围，不要塞进现有工作流。
- 结论只看 artifacts：`ci-source-identity`（实际 checkout 的 SHA）、`ci-bazel-evidence`（BEP + GTest XML）、`ci-log`。运行状态 success 或提交号显示一致都不等于本次提交被验证。
- 参数与门槛在 `deploy/ci/bazel-ci-config.json` 和 `deploy/ci/verify-results.py`；不要为了通过而放宽 `--jobs`／删除目标／开启测试缓存。
- 下载走代理：宿主机 `http://127.0.0.1:10808`；CI 容器内由 Runner 注入 `host.containers.internal:10809`，`deploy/ci/proxy-bazelrc.sh` 从环境变量派生 Bazel JVM 代理参数，不把机器地址写进仓库。直连 GitHub 不稳定，下载统一走代理。
- `.ultra_sandbox/bin/` 里的 `bazel`/`gcc` 等同属宿主工具，不得用于项目编译。

## Framework boundaries

- `zen_chat` is a general agent framework. MCP callback handling must use a
  generic authenticated event envelope and must not branch on application
  tool names, resource types, or product-specific payload fields. Application
  adapters own any domain-specific rendering or state projection.
- MCP notifications, including asynchronous tool completion events, are
  appended to their originating conversation and resume its LLM. Use the
  existing MCP inbound notification handler and conversation queue; do not
  introduce a separate HTTP completion callback or LLM scheduler.
- Continuation may use tools only under the agent's explicit continuation
  policy and the user's existing authorized goal. It must preserve tool
  confirmation, event deduplication, caller/session isolation and cancellation;
  late notifications must not restart work after the user stops it.
