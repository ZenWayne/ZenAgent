# 平台实现与维护

修改工作流、镜像、Bazel 参数、门槛或 Runner 时读本文。日常派发与核验见 [操作手册](operations.md)。

## 目录

- [为什么不允许宿主编译](#为什么不允许宿主编译)
- [Runner 与任务容器](#runner-与任务容器)
- [任务镜像](#任务镜像)
- [Bazel 参数来源](#bazel-参数来源)
- [检查范围与目标选择](#检查范围与目标选择)
- [证据门槛](#证据门槛)
- [缓存策略](#缓存策略)
- [不做的检查](#不做的检查)
- [排障](#排障)

## 为什么不允许宿主编译

宿主机同时承载 Forgejo 服务、Runner 和多个业务栈，磁盘长期处于高位；在宿主 checkout 里编译会：

1. 产生无法归属的 `bazel-out/`、`~/.cache/bazel/` 副产物，占满共享磁盘并拖垮其他栈；
2. 让"这次提交通过了吗"变成"这台机器现在的环境能编过吗"——宿主目录里可能残留旧输出、旧工具链和旧 `bazel-*` 软链；
3. 让一次 `rm -rf` 或缓存清理就能改变结论，且没有任何证据可以复现。

因此编译只发生在 Forgejo 为本次运行新建的一次性容器里：镜像固定、参数固定、输出随容器销毁。宿主机唯一允许的构建动作是 `podman build -f deploy/ci/Containerfile`（只装工具链，不编项目源码）。

## Runner 与任务容器

- Forgejo：`http://100.121.231.24:3001/`（本机回环入口 `http://127.0.0.1:3001/`），仓库 `wayne/zenagent`。
- Runner：`forgejo-runner` 13.1.0 容器，通过本机 rootless Podman socket 创建任务容器；标签 `linux-small`；并发 3。
- 任务容器上限（Runner 配置，不由工作流放宽）：内存 3 GiB、`--memory-swap` 3 GiB、4 CPU、512 进程；任务超时 45 分钟。
- Runner 注入代理 `host.containers.internal:10809`（宿主代理的容器侧地址），并设置 `NO_PROXY`。
- Runner 把 `GITHUB_WORKSPACE`（`/workspace/<owner>/<repo>`）本身挂进任务容器，这是 act 的固定行为，也是工作流 clone 源码的位置；工作流没有其它挂载，也不使用共享协调卷。因此 `container-guard.sh` 允许工作区位于**这个**挂载之下，但拒绝两种情形：工作区自己就是挂载点，或工作区位于其它挂载之下。`GITHUB_WORKSPACE` 缺失时一律拒绝（fail closed）。
- 同一 `source_ref` 通过 concurrency group 串行，不同源码可并行。

Bazel 的运行预算必须装进 3 GiB：`--jobs=2`、`--local_ram_resources=2048`、`--local_test_jobs=1`、JVM `-Xmx1g`。调大任何一个之前先按 [排障](#排障) 复现一次峰值，不要把"能跑完一次"当作余量证明。

## 任务镜像

`deploy/ci/Containerfile` → `localhost/zen-ci:ubuntu24.04-bazel8.4.2`。

- 基础：`docker.io/library/ubuntu:24.04`。
- 工具链：`g++`/`make`/`patch`（`.bazelrc` 与 rules_cc 的宿主 C++20 工具链）、`git`（`git_override` protobuf、子模块相关 checkout）、`zlib1g-dev`（protobuf）、`python3`（配置与结果核验）、`openjdk-21-jdk-headless`（`//agentflow/jni` 的 Kotlin 目标需要 `@local_jdk`）。
- Node 24.22.3（官方 tarball + sha256）：Forgejo 用 `node` 执行 `actions/checkout@v4` 与 `actions/upload-artifact@v3`，镜像里没有 `node` 会让 job 在 checkout 阶段以 exit 127 立刻失败。

工作流不用 `actions/checkout`，改用普通 `git clone` + `git fetch`（见下）。原因：仓库里的 `edge-agent-workspace`、`rig` 是 gitlink，长期没有对应 `.gitmodules` 条目，`actions/checkout` 收尾时无条件执行 `git submodule foreach --recursive`，会以 `fatal: No url found for submodule path` 退出 128，job 在第一步就失败。

`.gitmodules` 已补齐：`rig` 指向 `https://github.com/0xPlaygrounds/rig.git`（索引里的提交 `fdd3757` 在该仓库存在）；`edge-agent-workspace` 的提交 `c97f96e0` 在任何远端都不存在，因此保留路径映射、写占位 url（`example.invalid`）并设 `update = none`，让 `status`/`foreach`/`update --init` 都返回 0 而不去拉取不存在的仓库。补条目前后都实测过全新 clone。

CI 仍然用 `git clone` 而不是 Action：它少依赖一个需要 `node` 的远程 Action，行为完全在本仓库可见。CI 不需要任何子模块；如需改回 `actions/checkout`，先按 [操作手册](operations.md) 的方式验证一次 `git submodule foreach --recursive` 在全新 clone 上返回 0。

> 派发后的 run 只取 `source_ref`；`github.token` 通过 `http.extraHeader` 传给 `git clone`，令牌不出现在 URL 或日志里。
- Bazel 8.4.2 二进制从 `releases.bazel.build` 下载并按 sha256 校验后写入 `/usr/local/bin/bazel`；版本必须等于 `.bazelversion`。
- 镜像自带标记 `CI_CONTAINER=1`、`CI_IMAGE`、`CI_BAZEL_VERSION`，`container-guard.sh` 与 `contract-check.sh` 依赖它们判定"确实在 CI 运行时里"。

更新镜像（宿主执行，只装工具，不编项目）：

```bash
podman build --network host -t localhost/zen-ci:ubuntu24.04-bazel8.4.2 \
  -f deploy/ci/Containerfile .
```

改动 Bazel 版本时三处必须同步：`.bazelversion`、`Containerfile` 的 `ARG BAZEL_VERSION`（含新的 `BAZEL_SHA256`）、以及镜像 tag；`contract-check.sh` 会拒绝不一致。

已知缺口：`identity.json` 只记镜像 tag，不记镜像 ID/digest。镜像被本地重建后（例如被别人的 `podman image prune` 清掉再重建），artifacts 看不出内容已经变了。需要严格复现时，派发前自行核对 `podman images --format '{{.ID}}' localhost/zen-ci:ubuntu24.04-bazel8.4.2`，或在后续把 digest 写进 identity。

## Bazel 参数来源

`deploy/ci/bazel-ci-config.json` 是**唯一**的启动/构建/测试参数来源，`checks.sh` 从它读参数，不在工作流里另写一份：

- `startup_options`：`--batch`（一次性服务器，避免遗留 daemon）、`--nosystem_rc`/`--nohome_rc`（不受宿主或镜像里未知 bazelrc 影响）、JVM 堆与 CPU 数。
- `build_options`：`--jobs=2`、`--local_ram_resources=2048`、`--verbose_failures`。
- `test_options`：`--nocache_test_results`（必须真实执行）、`--local_test_jobs=1`、`--test_output=errors`、`--test_summary=detailed`。

Bazel 的下载（BCR 模块、`http_archive`、GitHub release、maven）走 Bazel 服务器 JVM，不认 `http_proxy`。`deploy/ci/proxy-bazelrc.sh` 把 Runner 注入的代理环境变量转成 `startup --host_jvm_args=-Dhttps.proxyHost=...`，**不把任何机器地址写进仓库**。没有代理时它生成空 rc，构建照常进行。

## 检查范围与目标选择

普通 CI 固定两个范围：

| scope | 命令 | 覆盖 |
| --- | --- | --- |
| `build` | `bazel build //agentflow/...` | 框架全部 C++ 库、proto、JNI/Kotlin 目标编译通过 |
| `test` | `bazel test //tests/unit/...` | 单元测试全部执行且通过 |
| `all`（默认） | 先 build 后 test | 推送分支和合并前的标准检查 |

目前不做按改动路径的选测：完整 `//agentflow/...` + `//tests/unit/...` 实测一次约 15 分钟（run #4：构建 11.5 分钟、31 个目标 / 1286 actions / 776 编译动作；随后 55 个测试目标 531 用例 3.5 分钟，因为共用同一个输出基），比维护一套易错的路径映射更可靠。等实测稳定超过 35 分钟再引入选测，并同时补"选择原因"证据。

新增/移动测试目标不需要改 CI；只要在 `//tests/unit/` 下，就会自动纳入。`//tests/integration`、`//tests/smoke`、`//tests/repro` 依赖模型文件或外部服务，不在普通 CI 范围。

## 证据门槛

`deploy/ci/verify-results.py` 不接受"退出码 0"当作结论：

- build：BEP 必须有 `buildFinished{overallSuccess:true}`，并且存在 `actionCompleted` 事件（否则是"分析通过、什么都没编"）。
- test：对 BEP 中每个 `testSummary`，`bazel-testlogs/<label>/test.xml` 必须存在、非空，`tests > 0`，`failures/errors == 0`，Bazel 状态 `PASSED`；出现 `testResult{cachedLocally:true}` 直接失败。
- 结果写入 `results/identity.json`、`results/*.bep.jsonl`、`results/test-report.json`、`results/testlogs/<package>/<target>/test.xml`（原始 GTest XML，从 Bazel 输出树复制，因为 `bazel-testlogs` 是指向 output base 的符号链接，会随一次性容器消失），工作流始终上传为 `ci-source-identity`、`ci-bazel-evidence`、`ci-log`（保留 7 天）。

`identity.json` 记录 `source_sha`、`configuration_sha`、镜像 tag、Bazel 版本、run/job/attempt。派发时传入的 `source_ref` 只是声明；结论必须同时核对 checkout 出来的 SHA 与 artifacts。

## 缓存策略

不共享 Bazel 编译缓存、不复用 `bazel-out`：每次运行使用容器内私有 `--output_user_root`，测试强制 `--nocache_test_results`。共享缓存要么让"通过"来自别人的产物，要么需要可信 writer 隔离与容量治理，两者当前都不具备。Repository 下载（模块、预编译 LiteRT 归档）每次重新拉取；实测稳定后若成为瓶颈，再加只读的 `--repository_cache`，且必须固定目录所有权。

## 不做的检查

- **Android arm64 交叉编译**：需要 NDK r28b（约 4 GiB）与单独工具链注册，超出 3 GiB 任务预算和当前磁盘余量。需要时另建专用镜像与工作流，不与普通 CI 混跑。
- **ASan/UBSan**：`.bazelrc` 已有 `--config=asan/ubsan`，但同机运行会让运行时翻倍。作为独立可选工作流再加。
- **Kotlin/Gradle 全量构建**：`//agentflow/...` 已覆盖 JNI/Kotlin 目标编译；Android 打包不在本仓库 CI 范围。
- **部署、发布、模型调用**：普通 CI 不接触业务凭据，不调用付费模型。

## 排障

| 现象 | 判断 | 处理 |
| --- | --- | --- |
| `Server terminated abruptly (error code: 14 ... recvmsg:Connection reset by peer)` | Bazel 服务器 JVM 被 cgroup OOM kill | 降低 `--jobs` 或 `--local_ram_resources`；不要抬高容器内存上限来"修好"一次运行 |
| `ci preflight: NGiB free, 6GiB required` | 容器内可用磁盘不足 | 先在宿主腾出空间（见下），再重跑；不要降低门槛掩盖 |
| `ci contract: ... differs from the job image` | `.bazelversion`、图片 ARG、镜像 tag 不一致 | 同步三处并重建镜像 |
| `missing image localhost/zen-ci:...` | Runner 主机没有该镜像 | 在宿主按 [任务镜像](#任务镜像) 构建；不要改成拉取公网镜像 |
| `container guard: workspace ... is a mount point` | 有人把宿主 checkout 挂进容器编译 | 改为容器内私有副本（`reproduce.sh` 或工作流 checkout） |
| 下载超时 / `Connection reset` 指向 github/BCR | 代理未传入 Bazel JVM | 确认 Runner 注入的 `https_proxy` 存在，`proxy-bazelrc.sh` 输出非空 |
| `no field ... ` / artifact 为空 | 该次运行没跑到该步骤 | 按 artifacts 判断实际执行到哪一步，不要用状态位推断 |
| `fj run logs` 报 `querying Loki for workflow logs` | 本机未部署 Loki 日志后端 | 读 Forgejo 存储里的原始任务日志（只读）：`podman cp forgejo-server:/data/gitea/actions_log/wayne/zenagent/<bucket>/<task>.log.zst /tmp/x.log.zst && zstd -dc /tmp/x.log.zst`；不要为此改造 Forgejo 或另写日志客户端 |
| `Job 'CI contracts' failed` 但日志无错 | 先看任务日志里的第一次错误；`docker exec ... node` 报 `executable file 'node' not found` 是镜像缺 node | 重建镜像并核对 `node --version`；不要在工作流里临时装 node |
| 任务在 `docker pull image=localhost/zen-ci:...` 处报 `Error response from daemon: connection refused` | 宿主 `podman.service` 正在重启（其他栈的 compose 活动也会触发），与 CI 配置无关 | 确认 `podman info` 与 socket `_ping` 恢复后重新派发；不要因此改工作流或镜像 |
| 镜像突然「不存在」/ `image not known`，本机也没人删过 | 其他任务执行了 `podman image prune`（本机曾把 `zen-ci` 和 `ubuntu:24.04` 一起清掉） | 按 [任务镜像](#任务镜像) 重建后再派发；镜像 tag 不进 `identity.json`，所以镜像被静默重建无法从 artifacts 看出，重要结论要同时核对镜像 ID |

宿主磁盘清理只针对可再生成的内容（镜像缓存、旧容器、悬空卷），清理前确认没有正在运行的 CI 或业务栈；不要删别人的卷来腾空间。
