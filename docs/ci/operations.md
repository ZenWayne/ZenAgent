# Forgejo 与 CI 操作手册

平台设计、参数与门槛见 [平台实现](platform.md)；检查范围见 [CI 入口](README.md)。

## 1. 平台与凭据

GitHub `origin` 是代码与 PR 主仓库；私有 Forgejo `wayne/zenagent` 保存待验证源码并执行检查。作者在 GitHub review 与 merge，代理不代为合并、不推 `master`。

- Forgejo 页面：<http://100.121.231.24:3001/wayne/zenagent>
- 本机 API / CLI 入口：`http://127.0.0.1:3001`
- Git remote：`forgejo`（`git remote -v` 核对，不覆盖已有 remote）
- 运行标签：`linux-small`

日常查询、派发、等待、下载 artifacts 一律走 `fj`（v0.7.0，认证交给 `fj auth`）：

```bash
fj --version
fj auth status
fj run list --host 127.0.0.1:3001 --repo wayne/zenagent -L 10
```

`--host` 是登录项名称（`127.0.0.1:3001`），不是 URL；不要加 `http://`。令牌不写进命令参数、remote URL、配置或日志。

## 2. 固定待验证源码

```bash
git status --short
git branch --show-current
git rev-parse HEAD
git remote -v
```

功能分支先推 GitHub `origin`，再推 Forgejo `forgejo`，核对两个远端的完整 SHA 与本地 `HEAD` 一致；不强推，不把部分成功说成同步完成。

```bash
git push origin HEAD
git push forgejo HEAD
git rev-parse HEAD
git ls-remote origin "$(git rev-parse --abbrev-ref HEAD)"
git ls-remote forgejo "$(git rev-parse --abbrev-ref HEAD)"
```

## 3. 派发检查

```bash
fj workflow list --host 127.0.0.1:3001 --repo wayne/zenagent
fj workflow run ci.yml \
  --host 127.0.0.1:3001 --repo wayne/zenagent \
  --ref <config-branch> \
  --field source_ref=<完整 40 位 SHA> \
  --field scope=all
```

- `source_ref` 用完整 SHA，不用分支名：分支会在排队期间移动。
- `scope`：`build`（只编译）、`test`（只跑单元测试）、`all`（默认）。
- 派发成功不等于通过；排队和客户端报错都不是结论。
- `fj workflow run` 可能报 `dispatched ... but could not find the triggered run`：运行通常已经创建，先用 `fj run list` 按 workflow、`source_ref`、时间核对，**不要重复派发**。无法唯一关联时保留"派发未确认"。
- 自动触发只在向 `main` 推送时生效；镜像仓库 `wayne/zenagent` 当前默认分支是 CI 配置分支 `ci/forgejo-ci`，所以实际运行以 `workflow_dispatch` 为主。`source_ref` 写的必须是已同步到 Forgejo 的真实 40 位 SHA，写错会在克隆步骤直接失败。

## 4. 观察与取证据

```bash
fj run list --host 127.0.0.1:3001 --repo wayne/zenagent -L 10
fj run view <run_number> --host 127.0.0.1:3001 --repo wayne/zenagent
fj run logs <run_number> --host 127.0.0.1:3001 --repo wayne/zenagent
fj run watch <run_number> --host 127.0.0.1:3001 --repo wayne/zenagent --exit-status
fj run download <run_number> --host 127.0.0.1:3001 --repo wayne/zenagent
```

`run_number` 是 `fj run list` 的 `#` 列（页面序号），REST `/actions/runs/{id}` 用的是原生 `id`，两者不能混用；job 级结果用 `--job` 的零基索引区分。

必须核对的 artifacts：

| artifact | 用途 |
| --- | --- |
| `ci-source-identity` | `identity.json`：实际 checkout 的 `source_sha`、`configuration_sha`、镜像、Bazel 版本、run/job/attempt |
| `ci-bazel-evidence` | `build.bep.jsonl`、`test.bep.jsonl`、`test-report.json`：目标数、用例数、每个目标状态；`testlogs/`：每个已执行目标的原始 GTest `test.xml`（按 label 路径，从 Bazel 输出树复制出来） |
| `ci-log` | 完整执行日志（含预检、参数、失败原因） |

`status=success` 与 commit 显示都不算证据。结论要求：`identity.json` 的 `source_sha` 等于待验证 SHA；`test-report.json` 的 `targets` 覆盖本次应有目标且 `test_cases > 0`；`testlogs/` 里有对应目标的原始 XML；日志里没有 skip/缓存命中。

## 5. 本地复现

**复现也是在容器里跑，产物随容器销毁，不落宿主。** 宿主机禁止 `bazel`/`cmake`/`g++` 编译（见 [平台实现](platform.md#为什么不允许宿主编译)）。

```bash
export http_proxy=http://127.0.0.1:10808 https_proxy=http://127.0.0.1:10808
bash deploy/ci/reproduce.sh all            # 或 build / test
bash deploy/ci/reproduce.sh test /path/to/worktree
```

它把当前 worktree 的已跟踪文件 + 未忽略的新文件打成快照，挂进一次性容器，以与 CI 相同的 `deploy/ci/checks.sh` 执行；宿主只负责启动容器。复现结果只用于定位失败原因，不能替代目标 SHA 的 CI 记录（工作区可能脏）。

镜像缺失时先构建（宿主只装工具，不编项目）：

```bash
podman build --network host -t localhost/zen-ci:ubuntu24.04-bazel8.4.2 \
  -f deploy/ci/Containerfile .
```

## 6. 结果复用与重跑

- 同一 `source_ref` + `scope` 已有成功运行且证据完整时，复用那条运行，不重复派发。
- 只改了文档、CI 注释或 artifact 名称，而源码与参数未变时，不需要重跑完整构建；说明复用哪条运行和为什么等价。
- 代码、`.bazelversion`、`deploy/ci/bazel-ci-config.json`、`Containerfile`、`checks.sh` 改动都算构建输入变化，必须跑新的检查。
- 重跑只针对失败的部分；失败后补跑受影响范围，不因文档或提交 SHA 变化触发全量重跑。
- 取消与重跑属于写操作，只在明确需要时执行：`fj run cancel <n>`、`fj run rerun <n>`。

## 7. 禁止的做法

| 做法 | 为什么不行 |
| --- | --- |
| 在宿主 checkout 跑 `bazel build/test`、`cmake`、`make`、`g++` | 违反宿主禁编边界；产物不可复现、占满共享磁盘 |
| 用宿主残留的 `bazel-out/`、`~/.cache/bazel/` 结果当证据 | 无法证明对应提交，也无法证明是本次环境 |
| 把 CI 结论写成"最新一次运行成功" | 未核对 SHA 的运行可能属于另一个提交 |
| 为了通过而放宽 `checks.sh` 门槛、删除测试目标或加 `--cache_test_results` | 让门禁失去意义 |
| 在工作流里 `apt-get install` 工具链或改用公网镜像 | 破坏固定的运行环境；工具必须在镜像里 |
| 提交、日志或 PR 里出现令牌、宿主绝对路径 | 泄漏凭据 / 违反可移植性 |
