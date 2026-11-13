# CI 入口

本项目的源码与 PR 在 GitHub `origin`；**所有编译和测试只在私有 Forgejo 的任务容器里执行**。本页是路由，先确认要选择范围、核验结果、派发检查，还是修改 CI 实现。

> **硬边界：严禁在宿主机编译项目。** 宿主机上只允许编辑源码、`git` 操作，以及构建 CI 镜像（`deploy/ci/Containerfile`）。`bazel`、`cmake`、`g++`、`gradle`、`make` 在任何宿主机 checkout 中执行都被视为违规，产出的 `bazel-*` / `bazel-out` 不能作为验证证据。原因见 [平台实现](platform.md#为什么不允许宿主编译)。

| 当前动作 | 必读内容 |
| --- | --- |
| 选择检查范围、判断能否复用结果 | [平台实现：检查范围与目标选择](platform.md#检查范围与目标选择) |
| 派发、等待、读取运行结果 | [操作手册](operations.md) |
| 本地复现一次 CI 检查 | [操作手册：本地复现](operations.md#本地复现) |
| 修改工作流、镜像、Bazel 参数或门槛 | [平台实现](platform.md) |
| 排障（OOM、磁盘、代理、镜像缺失） | [平台实现：排障](platform.md#排障) |
| 写 PR 说明或审查记录 | 附上 `ci-source-identity` 与 `ci-bazel-evidence` artifacts，不只贴状态 |

## 一次检查包含什么

| 阶段 | 位置 | 门槛 |
| --- | --- | --- |
| 静态契约 | `deploy/ci/contract-check.sh` | 镜像版本 = `.bazelversion` = Containerfile ARG；Bazel 参数 JSON 合法；脚本语法正确；工作流必须在容器里跑 |
| 预检 | `deploy/ci/checks.sh` | 容器内标识存在；工作区是容器内私有副本而非宿主挂载；可用磁盘 ≥ `CI_MIN_DISK_GIB`（默认 6 GiB） |
| 构建 | `checks.sh build` | `bazel build //agentflow/...`，BEP 出现成功 `buildFinished` 且确有 action 执行 |
| 测试 | `checks.sh test` | `bazel test //tests/unit/...`，每个目标的 GTest XML 非空、用例数 > 0、无失败；结果不得来自测试缓存 |
| 证据 | `deploy/ci/verify-results.py` | 写入 `identity.json`、`*.bep.jsonl`、`test-report.json`，由工作流上传为 artifacts |

源码克隆用 `git clone` + `git fetch --detach`，不用 `actions/checkout`：本仓库有 gitlink 缺 `.gitmodules` 条目，checkout 的 `git submodule foreach` 会直接失败（见 [平台实现](platform.md#任务镜像)）。

普通 CI 不构建 Android arm64（需要 NDK，见 [平台实现](platform.md#不做的检查)），不部署、不发布、不访问业务凭据。

## 目录

- `.forgejo/workflows/ci.yml` — 工作流定义（`quality` + `verify` 两个 job）
- `deploy/ci/Containerfile` — 任务镜像 `localhost/zen-ci:ubuntu24.04-bazel8.4.2`
- `deploy/ci/contract-check.sh` — 静态契约
- `deploy/ci/container-guard.sh` — 拒绝容器外执行
- `deploy/ci/checks.sh` — 唯一执行入口
- `deploy/ci/bazel-ci-config.json` — 固定 Bazel 启动/构建/测试参数
- `deploy/ci/proxy-bazelrc.sh` — 从环境变量派生 Bazel JVM 代理
- `deploy/ci/verify-results.py` — 结果证据核验
- `deploy/ci/reproduce.sh` — 在临时容器里复现检查，产物不落宿主
