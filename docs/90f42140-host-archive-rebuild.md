# LiteRT-LM 90f42140 host 归档重建 — 进展与卡点

> 状态: **host 归档已构建 + 打包 + 符号核对通过;链接验证通过(header test + //agentflow/... 全量构建绿);待发布** · 最后更新: 2026-08-31 23:40 · 用途: 给需要接手/会诊的人

## 0. 最新进展(2026-08-31 深夜)

上一节卡点 B/C/D 已被并行会话修复(修复在 ZenWayne/LiteRT-LM `feat/upstream-90f-port`:
`6738248d` honor SAMSUNG instead of patching clone、`bf6197c9` only map Samsung archives when enabled),
host 构建完成,归档发布于 `litert_build_90f/dist-host-90f/`,`litert_lm_engine_tokenize` 确认导出。

本轮(链接验证阶段)新增的修复:

1. **`tests/smoke/litert_lm_header_test.cc` 重写**:原测试用了假 API(`LiteRtLmSamplerParams` 值语义、
   `kGreedy`),真实 C 头是不透明指针 + `kLiteRtLmSamplerTypeGreedy`。重写为:真实 create/set/delete
   演练 + 引用 `litert_lm_engine_tokenize/detokenize`、`conversation_config_create` 符号(编译+链接证明)。
2. **abseil 提升 `20260107.0` → `20260526.0`**:90f42140 归档按 `lts_20260526` 内联命名空间编译,
   旧 pin 匹配的是 `lts_2026010712`(26df558b 线)。链接报
   `undefined ref absl::lts_20260526::...`,换 pin 后解析正确。
3. **Bzlmod 兼容级别冲突**:BCR 的 `abseil-cpp@20260526.0` 未声明 `compatibility_level`(隐式 0),
   而 protobuf 依赖链要求 compat 1;Bazel 7.4.1 的 `single_version_override` 无 compat 参数。
   解法:**自定义 registry** `.bcr-override/`(模块 `compatibility_level = 1` + 官方 20260526.0 tarball
   sha256)- `single_version_override(module_name="abseil-cpp", version="20260526.0", registry="file://.../.bcr-override")`。
4. **缺 `@abseil-cpp//absl/status:status_builder`**:新归档代码用了 `StatusBuilder`
   (`absl/status/status_builder.cc`,独立 target,不在 `:status` 聚合里),旧归档没用过。

验证结果:
- `bazel test //tests/smoke:litert_lm_header_test` ✅
- `bazel build //agentflow/...`(含工作树未提交的框架移植文件)✅ 665 actions
- 归档 sha256: staging `680d3e64f4abb33bae3278caa7da0787b5ad27303e9e69702ebd179cb918c00b` /
  external `74de2728592f2f3449957c46a4d22ea30f83938986af0ac8079fb934680ebcd4`

剩余步骤:
- **发布 tag(卡点,待用户确认)**:约定名 `litert-lm-prebuilt-20260831`
- MODULE.bazel 正式重指(file:// → release URL + sha256,替换临时覆盖)
- 提交框架移植 + 子模块 bump 到 `bf6197c9` + abseil/registry/测试改动
- arm64 归档重建(90f42140)→ 开启 `AGENTFLOW_ENABLE_LITERTLM_TOKENIZER_API` → repro + 4 会话 deep-search 验证

## 1. 背景与目标

ZenAgent 项目的 vendored LiteRT-LM 原来基于私有线 `26df558b`,引擎存在多会话 teardown bug
(退出时 `free(): invalid next size`)。为修复,我们把引擎升级到上游 `90f42140` 并重放了本地
LLGuidance 约束对话 C bridge 移植(已合并: ZenWayne/LiteRT-LM **PR #1**, 内容 commit
`9b7dc3f2`)。

升级链路(当前进行到第 3 步,卡在第 3 步):

1. ✅ 引擎移植合并(PR #1)
2. ✅ 子模块指针提升(`26df558b` → `9b7dc3f2`,ZenAgent 分支 `feat/engine-upgrade-90f`, commit `4074c60`)
3. 🟢 **重建 host 预编译归档**(`libce_staging.a` / `libce_external.a`,从 90f42140 源码)→ 发布 → 重指 `MODULE.bazel`
4. ⏳ 提交框架移植(uncommitted 的 `litert_lm_conversation.*` / `litert_lm_session.*` / `engine.h` / `conversation.h`)
5. ⏳ arm64 归档重建(90f42140)
6. ⏳ 验证:repro 套件 + 本地 deep-search 4 会话 + bug 文档结论

原卡点(configure Generate 失败)已解决,详见 §4 卡点 C。当前待办: 发布 release + 重指 `MODULE.bazel`。

## 2. 环境(机器 A,一台 Arch Linux 工作机)

| 项 | 值 |
|---|---|
| CMake | 4.3.2 |
| 编译器 | GCC 16.1.1(host,x86_64);clang++ 22.1.3 也可用(未验证全链) |
| 构建树 | `/home/wayne/tools/zen/litert_build_90f`(out-of-tree,top-level: `cmake -B litert_build_90f` 于子模块源码 `LiteRT-LM@9b7dc3f2`) |
| 结构 | `prebuild/`(host 工具阶段)+ `litert_lm/main`(主构建) |
| 网络 | 下载必须走代理 `http://127.0.0.1:10808`;直连 github 不稳定(status 56 中断) |
| 源码 | `litert_build_90f/litert_lm/build/external/{tensorflow,litert}/src/*` 是手工浅克隆(README.android.md 流程),tensorflow@`862baf45`、litert@`fb16353a` |

## 3. 已过节点

- prebuild 阶段(host 工具:protobuf/flatc/sentencepiece 等): ✅ 完成
- 主树 configure: ✅(link.txt 已生成)
- 主构建进度: 曾到 ~56%(externals 大部完成);
  `tflite_external` 在修复下面卡点 A 后: ✅ 完成(xnnpack-microkernels 编译 + `Built target tflite_external`)
- litert 主库编译: 曾到 79%(`libLiteRt.so` 已在链接)后撞上卡点 B

## 4. 卡点清单(按发现顺序)

### 卡点 A: tflite_external 的 pthreadpool 下载失败 + 配置死锁(已绕过)

现象:
```
error: downloading 'https://github.com/google/pthreadpool/archive/02460584c6092e527c8b89f7df4de143d70e801f.zip' failed
      status_code: 56  "Failure when receiving data from the peer"
```
随后 XNNPACK configure 报 `ADD_SUBDIRECTORY given source ...pthreadpool-source which is not an existing directory`。

结构问题(关键): XNNPACK 的 `CMakeLists.txt:1295` 在 **configure 阶段**用
`ADD_SUBDIRECTORY("${PTHREADPOOL_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/pthreadpool")` 直接引用源目录,
而该源目录由 `pthreadpool-download`(ExternalProject,**build 阶段**)填充。中断续跑后顺序倒挂:
configure 先跑 → 源目录不存在 → 报错;下载步骤永远没机会跑。同时老目录 `pthreadpool` 占用了
binary dir 名,产生第二个冲突(already used)。

绕过(已做,已在构建树内生效):
- 用 `curl -x 127.0.0.1:10808 -L --retry 8` 手动下载该 zip → 直接解压填充
  `tflite_external-build/pthreadpool-source`,删除 `pthreadpool*` 旧目录后重跑 configure → 通过。

风险评估: 这是"预置源"绕过,不是流程修复。任何**全新**构建树(未中断)不会有此问题——
初始化下载顺序是正常的。因此若重建树,此卡点自愈。

### 卡点 B: GCC 16.1.1 内部编译错误(ICE)@ Samsung NPU vendor(已绕过)

现象:
```
vendors/samsung/ai_litecore_manager.h:83:27: internal compiler error: 段错误
```
GCC 16.1.1 在 `-O3` 下编译 `vendors/samsung/ai_litecore_manager.cc` 直接 SIGSEGV(编译器崩,非代码错)。
重复必现。clang++ 22.1.3 编译同一文件成功(55KB 对象)。

根因分析(为什么关不掉 Samsung):
`litert/vendors/CMakeLists.txt` 的逻辑:
```cmake
option(LITERT_ENABLE_SAMSUNG "Enable Samsung NPU build" ON)          # 默认 ON
if(LITECORE_HEADERS_DIR AND NOT LITERT_ENABLE_SAMSUNG)
  set(LITERT_ENABLE_SAMSUNG ON CACHE BOOL "" FORCE)                  # ← 无视 -D 强制拉回 ON
endif()
```
而 `LITECORE_HEADERS_DIR` 在每个 configure 里被**无条件自动填充**:
```cmake
if(NOT LITECORE_HEADERS_DIR)
  set(LITECORE_HEADERS_URL "https://soc-developer.semiconductor.samsung.com/...ai-litecore-...tar.gz")
  file(DOWNLOAD "${LITECORE_HEADERS_URL}" ...)                       # 自动下载 Samsung headers
  set(LITECORE_HEADERS_DIR "${...}/_deps/litecore_headers" ...)
endif()
```
结论: 只要 headers 目录存在(自动下载必有),`-DLITERT_ENABLE_SAMSUNG=OFF` 必被 FORCE 回 ON。
唯一约束点是 FORCE 那行。

绕过(已做,**构建树内**的局部补丁):
- 编辑 `litert_external/litert/vendors/CMakeLists.txt`,注释掉 FORCE 行(保留检测 message 改为提示)。
- 该文件是 build-tree 内的克隆,且 patch 步骤会 `git checkout -- . && git clean -df`,
  所以把补丁 **commit 到该克隆**(`local: respect explicit LITERT_ENABLE_SAMSUNG=OFF...`)以存活。
- 重配后确认: `LITERT_ENABLE_SAMSUNG:BOOL=OFF`,configure 输出
  `Skipping Samsung dispatch: LITECORE_HEADERS_DIR not set` → Samsung 子目录不再编译。
  (注: 自动下载 Samsung headers 仍发生——下载段未动,只禁用了子目录)

风险: 局部 hack。若重建树,需重新打。更干净的长期解法见 §6 建议。

### 卡点 C(**已解决**): litert configure Generate 失败 — `LiteRTLM::LiteRTLM::absl::shim`

SAMSUNG=OFF 之后,litert_external 的 configure 走到 **Generate 阶段**报错:

```
CMake Error at vendors/CMakeLists.txt:242 (target_link_libraries):
  Target "dispatch_api_Arm_so" links to:
    LiteRTLM::LiteRTLM::absl::shim
  but the target was not found. ...
Call Stack:  vendors/CMakeLists.txt:296 (_litert_add_dispatch_so)
```

**根因(已确认,与 cache 污染无关):补丁被应用了两次。**

LiteRT-LM 的 `cmake/packages/litert/litert_patcher.cmake` 会对 litert 克隆里所有
`CMakeLists.txt` 做正则替换:

```cmake
patch_file_content("${C_FILE}" "absl::[a-zA-Z0-9_]+" "LiteRTLM::absl::shim" TRUE)
```

这个替换**不是幂等的**: 对已经替换过的文本再跑一次,`LiteRTLM::absl::shim` 里的
`absl::shim` 会再次命中 `absl::[a-zA-Z0-9_]+`,产出 `LiteRTLM::LiteRTLM::absl::shim`。

正常情况下这不会发生,因为 litert 的 `PATCH_COMMAND` 以 `git checkout -- . && git clean -df`
开头,每次都把源码恢复到**未打补丁**的上游状态再打一次。

而卡点 B 的绕过把补丁 **commit 进了 litert_external 克隆**——问题在于当时工作区里
`litert/vendors/CMakeLists.txt` 已经被 patcher 改过了,那次 commit(`ac9f5118`)
把 patcher 的**全部产物**一起提交了进去(diff 22 插入 / 62 删除,包含 11 行
`absl::xxx` → `LiteRTLM::absl::shim`、`FetchContent_*` 抑制、MediaTek flatc 段等)。
于是 `git checkout -- .` 之后源码已经是"打过补丁"的状态,patcher 再打一次 → 双重前缀 →
`dispatch_api_Arm_so` 链接到一个不存在的 target。

排除的假设(供记录):
- ✗ absl shim 注入逻辑失效 —— `LiteRTLM::absl::shim` 在 `absl_aggregate.cmake:36` 正常定义为
  GLOBAL IMPORTED INTERFACE,一直存在;
- ✗ 独立重配绕过了 `litert_config.cmake` 的参数链 —— 独立重配确实缺参数,但报错的
  target 名里那个双重命名空间是**源文件里已经写死的**,和传什么 `-D` 无关;
- ✗ 需要关掉 QUALCOMM —— 前缀修好后 Qualcomm/Arm dispatch 正常解析,不必关。

**修复(已做,落在 LiteRT-LM fork,分支 `feat/upstream-90f-port`):**

1. `cmake/packages/litert/litert_patcher.cmake`: 在 patcher 循环之后加一条**幂等**的字面量替换,
   删掉上游 vendors 的 FORCE 行:
   ```cmake
   patch_file_content("${LITERTLM_LITERT_SRC_DIR}/vendors/CMakeLists.txt"
       "set(LITERT_ENABLE_SAMSUNG ON CACHE BOOL \"\" FORCE)"
       "# [LiteRTLM] Suppressed: honour explicit LITERT_ENABLE_SAMSUNG"
       FALSE)
   ```
   (替换完匹配串就消失,重复跑无副作用;已用 `cmake -P` 单独验证过。)
2. `cmake/packages/litert/litert.cmake`: 新增选项并透传
   ```cmake
   option(LITERTLM_LITERT_ENABLE_SAMSUNG "Build the LiteRT Samsung NPU dispatch backend" OFF)
   ...
   -DLITERT_ENABLE_SAMSUNG=${LITERTLM_LITERT_ENABLE_SAMSUNG}
   ```
3. litert_external 克隆里 `git reset --hard HEAD~1` 丢掉 `ac9f5118`,恢复纯净上游
   (`dc32e93f6`)。**以后不要再往这个克隆里 commit**——它是一次性的构建产物,
   所有本地补丁都必须放在 LiteRT-LM 的 patcher 里才能存活。

这样卡点 B 的绕过也从"构建树内的一次性 hack"升级成了版本受控、可重建的修复。

### 重建执行(2026-08-31 晚)

保留的部分(未重建,省掉数小时):
- `prebuild/` host 工具阶段;
- `tflite_external`(2.0G,`tflite_external-done` 已打戳,`libtensorflow-lite.a` 就位),
  因此卡点 A 的 pthreadpool 预置源绕过继续有效,不需要重来。

重置的部分:
```bash
cd litert_build_90f/litert_lm/build/external/litert/src
rm -rf litert_external-build && mkdir litert_external-build
rm -f litert_external-stamp/litert_external-{configure,patch}
```
(那个 4.1G 的 `litert_external-build` 里有 22:19 那次独立重配写下的污染 CMakeCache,
缺整条 LiteRT-LM `-D` 参数链,不能在上面增量续跑。)

然后从顶层重跑:
```bash
cd /home/wayne/tools/zen
export https_proxy=http://127.0.0.1:10808 http_proxy=http://127.0.0.1:10808
cmake --build litert_build_90f -j6      # -j6 而非 -j20: 只有 15G 内存
```

保留生效确认: 日志开头 `[ 50%] Built target prebuild`,tflite_external 未重跑。
主树因 `litert.cmake` 变更自动重配,连带重编了 `_deps`(antlr / protobuf / re2)。

**验证结果(卡点 B + C 均已消解):**

补丁恰好应用一次(重跑 patch 步骤后在克隆里实测):
```
$ grep -c 'LiteRTLM::LiteRTLM' .../litert/vendors/CMakeLists.txt   → 0
$ grep -c 'LiteRTLM::absl::shim' .../litert/vendors/CMakeLists.txt → 11
```

litert_external configure 通过(此前在此崩):
```
-- Skipping Samsung dispatch: LITECORE_HEADERS_DIR not set
-- Configuring done (331.2s)
-- Generating done (0.1s)
-- Build files have been written to: .../litert_external-build
```
Qualcomm / Arm dispatch 保持 ON 且正常解析,不需要像先前猜测那样关掉。

> 小瑕疵(仅影响日志可读性,不影响构建): 上一行还会打印
> `-- Enabling Samsung dispatch: Samsung headers detected at ...`。
> 我们只删了 FORCE 那一行,把它上面那句 message 留着了,所以两条相邻的日志看起来自相矛盾。
> 以最终那句 `Skipping Samsung dispatch` 为准。下次动 patcher 时顺手把 message 也换掉。

**LiteRT-LM fork 侧已提交并推送:** `feat/upstream-90f-port` → `6738248d`
`build(litert): honour LITERT_ENABLE_SAMSUNG instead of patching the clone`。
(注: PR #1 合并后 origin 上的分支被删过,这次 push 是重新建的分支。)

⚠️ **待办(已决定归属: 第 4 步)**: ZenAgent 的子模块指针还停在 `9b7dc3f2`,不含 `6738248d`。
在提升之前,**本次构建只在本机可复现**——干净检出会拿到没有 patcher 修复的 `9b7dc3f2`,
一 configure 就会再次撞上卡点 B 与卡点 C。

决定: 不单独提交指针,而是在第 4 步(提交框架移植: `litert_lm_conversation.*` /
`litert_lm_session.*` / `engine.h` / `conversation.h`)时把子模块指针一并提到 `6738248d`,
再推 `feat/engine-upgrade-90f`(会连带推出目前未推送的 `4074c60`)。
理由: 指针提升和框架移植本来就是同一批改动,合并提交历史更干净。

## 5. 为什么此前(26df558b 线)没有这些问题

- 26df558b 的 host/arm64 归档(6 月、8-25 发布)用同一套 CMake 流程构建成功过;
- 本次差异: (a) 90f42140 上游移动/重写了 samsung/qualcomm dispatch 代码
  (`ai_litecore_manager.h` 是 90f42140 才有的);
  (b) 本机 gcc 16.1.1 对 samsung 头模板 ICE(旧构建或许用的旧 gcc 或 clang);
  (c) 中断-续跑-重配的混乱状态放大了配置敏感度。

## 6. 建议方向(供会诊)

1. **优先: fresh 重建**。当前 `litert_build_90f` 是"中断 → 手工绕过 → 半重配"的混合状态,
   cache 污染难以逐一排查。新建干净构建树全量重跑(约 1-2 小时 + 下载,代理 10808),
   大概率同时消解卡点 A(初始顺序正常)与 C(参数链完整);
   - 若 fresh 构建仍撞 SAMSUNG(卡点 B): 在 `litert_config.cmake` 注入
     `-DLITERT_ENABLE_SAMSUNG=OFF`(连同上面的 FORCE 补丁),或整体改用 clang++
     (clang 22.1.3 可编译该文件)规避 GCC ICE。
2. **或: 关 Qualcomm**。`-DLITERT_ENABLE_QUALCOMM=OFF`(SAMUSNG 同理)——CPU-only 用不到
   dispatch SO;但需确认 C API 导出(47 符号)不依赖这些 vendor 路径,且会诊 LiteRT-LM 侧
   有无"仅 NPU=ON 时才有 shim"的语义。
3. **验证 tokenizer API 是否在 90f42140 归档导出**(升级后解码器探测路径的依赖):
   之前 26df558b split 归档只有 47 个 `litert_lm_*` 导出、**无 `litert_lm_engine_tokenize`**;
   需在 90f42140 归档 split 后用
   `llvm-nm --defined-only libce_staging.a | grep tokenize` 确认。

## 7. 未动/未提交的关联物

- ZenAgent 分支 `feat/engine-upgrade-90f`: 已提交 `4074c60`(仅子模块指针),**未推送**;
- 框架移植文件(uncommitted): `agentflow/inference/litert_lm_conversation.{cc,h}`、
  `litert_lm_session.{cc,h}`、`third_party/litert_lm/include/c/engine.h`(90f42140 头)、
  未跟踪 `third_party/litert_lm/include/c/conversation.h`;
- 归档还指向旧的 `litert-lm-prebuilt-20260825`(host)/`litert-lm-prebuilt-arm64-20260831`;
- decoder 重构(已合并 PR #39)已含宏
  `AGENTFLOW_ENABLE_LITERTLM_TOKENIZER_API`(默认 0),升级归档就绪后置 1;
- 磁盘: 构建树 ~10G+(`litert_build_90f`),机器 34G 空闲。

## 8. 复现当前错误的最小命令

```bash
cd /home/wayne/tools/zen/litert_build_90f/litert_lm/build/external/litert/src
export https_proxy=http://127.0.0.1:10808 http_proxy=http://127.0.0.1:10808
cmake -S litert_external/litert -B litert_external-build -DLITERT_ENABLE_SAMSUNG=OFF
# → 在 Generate 阶段重现:
#   "Target dispatch_api_Arm_so links to LiteRTLM::LiteRTLM::absl::shim ... not found"
```

(注: 上述命令依赖 §4 卡点 B 的 FORCE 补丁已 commit 在 `litert_external` 克隆内,
否则 SAMSUNG 会被 FORCE 回 ON 并先撞 GCC ICE。)


## 9. 构建完成与打包(2026-08-31 深夜)

### 卡点 D: litert_lm_main 链接缺 Samsung 归档(已修)

全树编到 100% 后,最后一步链接失败:
```
没有规则可制作目标"external/litert/src/litert_external-build/vendors/samsung/libsamsung_soc_model.a",
由"litert_lm_main"需求
```
根因: `cmake/packages/litert/litert_target_map.cmake` 无条件列出三个 Samsung vendor 归档,
而这些路径会原样进入 `INTERFACE_LINK_LIBRARIES`;Samsung 一关文件就不产出。
这是被"Samsung 默认 ON"长期掩盖的既有耦合,不是本次改动引入的。

修复: 三条改为 `if(LITERTLM_LITERT_ENABLE_SAMSUNG)` 条件追加。
`liblitert_tool_flags_samsung.a` 保持无条件——它只是 flags,不依赖 `ai_litecore_manager`,
两种配置下都正常产出。

**构建结果: `Linking CXX executable litert_lm_main` → `Built target litert_lm`,零错误。**

### split_archives.sh 的两处适配(改动在 ZenAgent,**未提交**)

1. **staging 路径变了**。脚本按 `staging/lib/*` 分类,但 90f42140 里
   `cmake/packages/litert_lm/CMakeLists.txt:65` 把 `LITERTLM_STAGING_DIR` 覆盖成了
   `${CMAKE_BINARY_DIR}/../tmp/lib`(不再是 `litert_lm_config.cmake` 里的默认值)。
   不改的话 `staging source archives: 0`,`libce_staging.a` 会是空的。
   → 分类改为 `staging/lib/*|../tmp/lib/*`,两种布局都认。
2. **BUILD_DIR 必须是绝对路径**。`combine()` 里是
   `( cd "$sub" && ar xN ... "$src" )`,相对路径 chdir 之后就失效,
   表现为 `ar: ...libantlr4-runtime.a: 没有那个文件或目录`(文件其实存在)。
   → 开头加 `BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"`。

### 打包产物

```bash
bash third_party/litert_lm/scripts/split_archives.sh \
     litert_build_90f/litert_lm/build litert_build_90f/dist-host-90f
# staging source archives:  152
# external source archives: 109 (abseil excluded)   # 152+109=261,与 link.txt 全量一致,无遗漏
```

| 文件 | 大小 | 旧(26df558b) |
|---|---|---|
| `libce_staging.a` | 25,085,798 | 12,986,332 |
| `libce_external.a` | 161,417,706 | 155,258,080 |
| `libkissfft-float.so.131` | 41,448 | 41,448 |

产物位置: `litert_build_90f/dist-host-90f/`(构建树内,未放进仓库 `lib/`)。

### 符号核对结果 ✅

- **`litert_lm_engine_tokenize` 已导出** —— §6.3 的关键未知项有了结论。
  连带 `litert_lm_engine_detokenize` / `litert_lm_tokenize_result_*` /
  `litert_lm_detokenize_result_*` 一整套都在。
  → **`AGENTFLOW_ENABLE_LITERTLM_TOKENIZER_API` 可以置 1**(第 6 步)。
- `litert_lm_*` 导出: 47(旧) → **190**(新)。
- 新旧差集里唯一"丢失"的是 `litert_lm_log`: 我们代码零引用,新归档没有,
  90f42140 的 `c/` 源码里也已不存在 —— 上游删的,非构建回归。
- `kLiteRtRuntimeBuiltin`: 全归档**只有一个定义且为 `D`**,没有那个会让
  `litert::Environment::Create` CHECK_NOTNULL 崩掉的伪 `R` 定义。
- external 符号族: `cpuinfo_` 72 / `pack_lh` 33 / `TfLite` 310 / `xnn_` 6031,
  上次 arm64 出事时缺的几族这次都健在。
- external 总符号 62077(旧) → 63070(新)。差集里 15768 条绝大多数是 LLVM 匿名局部符号
  (哈希命名,跨构建必然不同);关键族里的 68 条全是 XNNPACK ukernel 变体,
  源于两个 TF 版本各自钉的 XNNPACK 版本不同,XNNPACK 内部靠自建配置表选核,自洽。

### LiteRT-LM fork 已推送

`feat/upstream-90f-port`:
- `6738248d` build(litert): honour LITERT_ENABLE_SAMSUNG instead of patching the clone
- `bf6197c9` build(litert): only map the Samsung vendor archives when Samsung is enabled

### 下一步(待你确认)

- [ ] **发布**: 上传 `dist-host-90f/` 三个文件到 `ZenWayne/ZenAgent` 新 release tag
      (对外动作,需你点头)
- [ ] **重指 `MODULE.bazel`**: host 的 `libce_staging` / `libce_external` 两处 `url` + `sha256`
- [ ] split_archives.sh 的两处适配需要提交(目前是 ZenAgent 工作区里的未提交改动)
